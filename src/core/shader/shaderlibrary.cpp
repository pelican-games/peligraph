#include "shaderlibrary.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../renderingpass/passshaderdefines.hpp"
#include "shadercompiler.hpp"
#include "../loader/engineresources.hpp"
#include "../loader/fileio.hpp"
#include "../loader/pathresolver.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"
#include "../watch/problemlist.hpp"
#include "../../project/materiallowering.hpp"
#include "../../project/sha256stream.hpp"
#include "../../project/shadersourceresolver.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <variant>

namespace Pelican {

namespace {

std::string lowerExtension(const std::filesystem::path &path) {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

ShaderStage shaderStageFromPath(const std::filesystem::path &path) {
    auto candidate = path;
    if (lowerExtension(candidate) == ".spv") {
        candidate = candidate.stem();
    }
    const auto extension = lowerExtension(candidate);
    for (const auto stage : shaderStages)
        if (extension == shaderStageSourceExtension(stage)) return stage;
    throw std::runtime_error(
        "Shader path does not encode an expected stage: " +
        path.string());
}

std::string memoryDiagnosticUri(std::string_view name) {
    if (name.find("://") != std::string_view::npos) {
        return std::string{name};
    }
    return "memory://" +
           (name.empty() ? std::string{"unnamed"} : std::string{name});
}

std::vector<uint32_t> bytesToSpirv(const std::string &data, const std::filesystem::path &path) {
    if (data.empty()) {
        throw std::runtime_error("Shader SPIR-V file is empty: " + path.string());
    }
    if (data.size() % sizeof(uint32_t) != 0) {
        throw std::runtime_error("Shader SPIR-V byte size is not a multiple of 4: " + path.string());
    }

    std::vector<uint32_t> spirv(data.size() / sizeof(uint32_t));
    std::memcpy(spirv.data(), data.data(), data.size());
    return spirv;
}

[[noreturn]] void throwUnsupportedFragment(const AssetFragmentRef &fragment) {
    throw std::runtime_error("Unsupported asset fragment kind: " + fragment.kind);
}

vk::ShaderStageFlagBits toVkStage(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::vertex:
        return vk::ShaderStageFlagBits::eVertex;
    case ShaderStage::fragment:
        return vk::ShaderStageFlagBits::eFragment;
    case ShaderStage::compute:
        return vk::ShaderStageFlagBits::eCompute;
    case ShaderStage::raygen:
        return vk::ShaderStageFlagBits::eRaygenKHR;
    case ShaderStage::miss:
        return vk::ShaderStageFlagBits::eMissKHR;
    case ShaderStage::closesthit:
        return vk::ShaderStageFlagBits::eClosestHitKHR;
    case ShaderStage::anyhit:
        return vk::ShaderStageFlagBits::eAnyHitKHR;
    case ShaderStage::intersection:
        return vk::ShaderStageFlagBits::eIntersectionKHR;
    case ShaderStage::callable:
        return vk::ShaderStageFlagBits::eCallableKHR;
    }
    throw std::runtime_error("unknown shader stage");
}

ShaderSourceStage sourceStage(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::vertex:
        return ShaderSourceStage::vertex;
    case ShaderStage::fragment:
        return ShaderSourceStage::fragment;
    case ShaderStage::compute:
        return ShaderSourceStage::compute;
    case ShaderStage::raygen:
        return ShaderSourceStage::raygen;
    case ShaderStage::miss:
        return ShaderSourceStage::miss;
    case ShaderStage::closesthit:
        return ShaderSourceStage::closesthit;
    case ShaderStage::anyhit:
        return ShaderSourceStage::anyhit;
    case ShaderStage::intersection:
        return ShaderSourceStage::intersection;
    case ShaderStage::callable:
        return ShaderSourceStage::callable;
    }
    throw std::runtime_error("unknown shader stage");
}

bool hasScheme(std::string_view ref) {
    return ref.find("://") != std::string_view::npos;
}

std::string joinTriedCandidates(const std::vector<std::string> &tried) {
    std::ostringstream stream;
    for (size_t i = 0; i < tried.size(); ++i) {
        if (i > 0) {
            stream << " -> ";
        }
        stream << tried[i];
    }
    return stream.str();
}

bool relativeWithin(const std::filesystem::path &root,
                    const std::filesystem::path &candidate,
                    std::filesystem::path &relative) {
    std::error_code error;
    const auto canonical_root = std::filesystem::weakly_canonical(root, error);
    if (error) return false;
    const auto canonical_candidate = std::filesystem::weakly_canonical(candidate, error);
    if (error) return false;
    relative = std::filesystem::relative(canonical_candidate, canonical_root, error);
    if (error || relative.empty() || relative.is_absolute()) return false;
    for (const auto &component : relative) {
        if (component == "..") return false;
    }
    return true;
}

void appendPathUnique(std::vector<std::filesystem::path> &paths,
                      const std::filesystem::path &path) {
    if (path.empty()) return;
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(path, error);
    if (error) normalized = std::filesystem::absolute(path, error).lexically_normal();
    if (std::find(paths.begin(), paths.end(), normalized) == paths.end()) {
        paths.push_back(std::move(normalized));
    }
}

bool isCompilerOwnedSurfaceDefine(
    std::string_view define) {
    return define.starts_with(
               surfaceScreenInputLocalReadDefinePrefix) ||
           define.starts_with(
               surfaceResourceLocalReadDefinePrefix);
}

std::vector<ShaderResourceInterfaceBinding>
compilerOwnedSurfaceResourceInterface(
    const SurfaceShaderComposition &composition,
    const SurfaceFormatDocument &surface) {
    std::vector<ShaderResourceInterfaceBinding>
        result;
    for (const auto &binding :
         composition.resource_interface) {
        const auto authored =
            std::any_of(
                surface.resource_ports.begin(),
                surface.resource_ports.end(),
                [&](const auto &port) {
                    return port.name ==
                           binding.port.name;
                });
        if (!authored) {
            result.push_back(binding);
        }
    }
    return result;
}

} // namespace

std::vector<ShaderBundleId> PreparedShaderReload::affectedBundleIds() const {
    std::vector<ShaderBundleId> result;
    result.reserve(candidates.size());
    for (const auto &candidate : candidates) result.push_back(candidate.id);
    return result;
}

ShaderLibrary::ShaderLibrary(ShaderLibraryModuleMode mode) : module_mode{mode} {}

std::optional<watch::AssetKey>
ShaderLibrary::logicalKeyForPath(const std::filesystem::path &path) const {
    const auto *resolver = FastModuleContainer::tryGet<PathResolver>();
    if (resolver == nullptr || !resolver->isSetup() || path.empty()) return std::nullopt;

    auto stores = resolver->stores();
    std::ranges::sort(stores, [](const auto &left, const auto &right) {
        return std::tuple{left.mount.empty(), left.name, left.root} <
               std::tuple{right.mount.empty(), right.name, right.root};
    });
    std::filesystem::path relative;
    for (const auto &store : stores) {
        if (relativeWithin(store.root, path, relative)) {
            return watch::makeAssetKey(store.mount, relative);
        }
    }
    if (relativeWithin(resolver->projectRoot(), path, relative)) {
        return watch::makeAssetKey({}, relative);
    }
    return std::nullopt;
}

std::optional<std::pair<std::filesystem::path, watch::AssetKey>>
ShaderLibrary::resolveReloadableSurface(std::string_view source_name) const {
    if (source_name.starts_with("engine://")) return std::nullopt;
    const auto *resolver = FastModuleContainer::tryGet<PathResolver>();
    if (resolver == nullptr || !resolver->isSetup()) return std::nullopt;
    try {
        const auto resolved = resolver->resolveProjectRef(source_name);
        const auto *path = std::get_if<std::filesystem::path>(&resolved);
        if (path == nullptr) return std::nullopt;
        const auto key = logicalKeyForPath(*path);
        if (!key) return std::nullopt;
        return std::pair{*path, *key};
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<watch::AssetKey>
ShaderLibrary::logicalDependencies(const ShaderBundle &bundle,
                                   std::optional<watch::AssetKey> primary) const {
    std::vector<watch::AssetKey> result;
    if (primary) result.push_back(std::move(*primary));
    for (const auto &path : bundle.dependency_paths) {
        if (const auto key = logicalKeyForPath(path)) result.push_back(*key);
    }
    if (const auto key = logicalKeyForPath(bundle.source_path)) result.push_back(*key);
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

void ShaderLibrary::registerFileReloadUnit(ShaderBundleId id,
                                           const ShaderBundle &bundle,
                                           std::vector<std::string> defines,
                                           std::vector<std::pair<std::string, std::string>>
                                               virtual_includes) {
    const auto source = logicalKeyForPath(bundle.source_path);
    if (!source) return;
    ReloadUnit unit{
        FileReloadRecipe{
            bundle.source_path, bundle.expected_stage,
            bundle.diagnostic_uri, std::move(defines),
            std::move(virtual_includes)},
        {id},
        *source,
        logicalDependencies(bundle, source),
    };
    const auto index = reload_units.size();
    reload_units.push_back(std::move(unit));
    unit_by_bundle.emplace(id, index);
    rebuildReverseDependencies();
}

void ShaderLibrary::registerSurfaceReloadUnit(
    SurfaceShaderBundleIds ids, const ShaderBundle &vertex_bundle,
    const ShaderBundle &fragment_bundle, std::filesystem::path path,
    watch::AssetKey source, std::string source_name, SurfacePass pass,
    std::vector<std::string> defines,
    std::optional<MaterialOutputSchema>
        material_output_schema,
    std::optional<SurfaceFormatDocument>
        embedded_surface) {
    auto dependencies = logicalDependencies(vertex_bundle, source);
    auto fragment_dependencies = logicalDependencies(fragment_bundle, source);
    dependencies.insert(dependencies.end(), fragment_dependencies.begin(),
                        fragment_dependencies.end());
    std::ranges::sort(dependencies);
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()),
                       dependencies.end());
    ReloadUnit unit{
        SurfaceReloadRecipe{std::move(path), source, std::move(source_name), pass,
                            std::move(defines),
                            std::move(material_output_schema),
                            std::move(embedded_surface)},
        {ids.vertex, ids.fragment},
        source,
        std::move(dependencies),
    };
    const auto index = reload_units.size();
    reload_units.push_back(std::move(unit));
    unit_by_bundle.emplace(ids.vertex, index);
    unit_by_bundle.emplace(ids.fragment, index);
    rebuildReverseDependencies();
}

void ShaderLibrary::rebuildReverseDependencies() {
    units_by_dependency.clear();
    for (std::size_t index = 0; index < reload_units.size(); ++index) {
        for (const auto &dependency : reload_units[index].dependencies) {
            auto &units = units_by_dependency[dependency];
            if (std::find(units.begin(), units.end(), index) == units.end()) {
                units.push_back(index);
            }
        }
    }
}

// VI-1: the pass compiler defines PELICAN_PASS_TEXTURE_INPUT_COUNT for every
// fullscreen / raster shader, so pelican_view.glsl's variadic count follows
// the pass. A prebuilt SPIR-V module was compiled without it and cannot
// read it; that define alone is no reason to refuse the module.
static bool definesNeedGlslSource(const std::vector<std::string> &defines) {
    const auto engine_count = std::string{passTextureInputCountDefine} + "=";
    return std::any_of(defines.begin(), defines.end(),
                       [&](const std::string &define) {
                           return !define.starts_with(engine_count);
                       });
}

ShaderBundle ShaderLibrary::buildFromFile(const std::filesystem::path &path,
                                          ShaderStage expected_stage,
                                           uint64_t version,
                                           std::vector<std::string> defines,
                                           std::vector<std::pair<std::string, std::string>>
                                               virtual_includes,
                                           std::string diagnostic_uri) const {
    if (diagnostic_uri.empty()) {
        diagnostic_uri = path.generic_string();
    }
    if (lowerExtension(path) == ".spv") {
        if (definesNeedGlslSource(defines) || !virtual_includes.empty()) {
            throw std::runtime_error(
                "Shader defines and generated includes require GLSL source, not SPIR-V: " +
                path.string());
        }
        return buildFromSpirv(
            bytesToSpirv(readBinaryFile(path.string()), path),
            expected_stage, path, std::move(diagnostic_uri), version, "");
    }

#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompileOptions options;
    options.defines = defines;
    options.virtual_includes = virtual_includes;
    const auto result = compiler.compileFile(path, options);
    if (!result.ok) {
        throw std::runtime_error("Shader compile failed: " + path.string() + "\n" + result.log);
    }
    auto bundle = buildFromSpirv(
        result.spirv, expected_stage, path, std::move(diagnostic_uri),
        version, result.log, std::move(defines));
    bundle.virtual_includes =
        std::move(virtual_includes);
    bundle.cache_key = result.cache_key;
    bundle.cache_hit = result.cache_hit;
    bundle.dependency_paths = result.dependencies;
    return bundle;
#else
    (void)defines;
    (void)virtual_includes;
    throw std::runtime_error("Runtime shader compiler is disabled; only .spv shader files are accepted: " +
                             path.string());
#endif
}

ShaderBundle ShaderLibrary::buildFromSpirv(
    std::span<const uint32_t> spirv, ShaderStage expected_stage,
    std::filesystem::path source_path, std::string diagnostic_uri,
    uint64_t version, std::string log,
                                           std::vector<std::string> defines) const {
    ShaderBundle bundle;
    if (diagnostic_uri.empty()) {
        diagnostic_uri = source_path.empty()
                             ? "memory://unnamed"
                             : source_path.generic_string();
    }
    auto rt_module = inspectRayTracingSpirv(spirv, diagnostic_uri);
    const auto &selected = selectRayTracingEntryPoint(
        rt_module, expected_stage, "main", diagnostic_uri);
    bundle.spirv_sha256 = sha256Hex(std::string_view{
        reinterpret_cast<const char *>(spirv.data()), spirv.size_bytes()});
    bundle.reflection = reflect(spirv);
    bundle.reflection.selected_entry_point = selected.key;
    bundle.reflection.ray_tracing_module = std::move(rt_module);
    // All policy inspection and entry selection happens before the Vulkan
    // shader module exists.
    bundle.module = createShaderModule(spirv);
    bundle.source_path = std::move(source_path);
    bundle.diagnostic_uri = std::move(diagnostic_uri);
    bundle.expected_stage = expected_stage;
    bundle.defines = std::move(defines);
    bundle.version = version;
    bundle.log = std::move(log);
    appendPathUnique(bundle.dependency_paths, bundle.source_path);
    return bundle;
}

ShaderBundle ShaderLibrary::buildFromEngineSource(std::string_view source, ShaderStage stage,
                                                  std::string_view name, uint64_t version,
                                                  std::vector<std::string> defines,
                                                  std::vector<std::pair<std::string, std::string>>
                                                      virtual_includes) const {
#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompileOptions options;
    options.defines = defines;
    options.virtual_includes = virtual_includes;
    const auto result = compiler.compileSource(source, toVkStage(stage), name, options);
    if (!result.ok) {
        throw std::runtime_error("Shader compile failed: " + std::string{name} + "\n" + result.log);
    }
    auto bundle = buildFromSpirv(
        result.spirv, stage, {}, memoryDiagnosticUri(name), version,
        result.log, std::move(defines));
    bundle.virtual_includes =
        std::move(virtual_includes);
    bundle.cache_key = result.cache_key;
    bundle.cache_hit = result.cache_hit;
    bundle.dependency_paths = result.dependencies;
    return bundle;
#else
    (void)source;
    (void)stage;
    (void)version;
    (void)defines;
    (void)virtual_includes;
    throw std::runtime_error("Runtime shader compiler is disabled; only .spv shader files are accepted: " +
                             std::string{name});
#endif
}

vk::UniqueShaderModule ShaderLibrary::createShaderModule(std::span<const uint32_t> spirv) const {
    if (spirv.empty()) {
        throw std::runtime_error("Shader module data must not be empty");
    }

    if (module_mode == ShaderLibraryModuleMode::reflection_only) {
        return {};
    }

    vk::ShaderModuleCreateInfo create_info;
    create_info.codeSize = spirv.size_bytes();
    create_info.pCode = spirv.data();
    return GET_MODULE(VulkanManageCore).getDevice().createShaderModuleUnique(create_info);
}

void ShaderLibrary::markDirty(ShaderBundleId id) {
    if (std::find(dirty_bundles.begin(), dirty_bundles.end(), id) == dirty_bundles.end()) {
        dirty_bundles.push_back(id);
    }
}

ShaderBundleId ShaderLibrary::loadFromFileForStage(
    const std::filesystem::path &path,
    ShaderStage expected_stage,
    std::vector<std::string> defines,
    std::vector<std::pair<std::string, std::string>>
        virtual_includes,
    std::string diagnostic_uri) {
    auto bundle =
        buildFromFile(path, expected_stage, 1, defines, virtual_includes,
                      std::move(diagnostic_uri));
    bundle_ids.reserve(bundle_ids.size() + 1);
    const auto id = bundles.reg(std::move(bundle));
    bundle_ids.push_back(id);
    registerFileReloadUnit(
        id, bundles.get(id), std::move(defines),
        std::move(virtual_includes));
    return id;
}

ShaderBundleId ShaderLibrary::loadFromFile(
    const std::filesystem::path &path,
    std::vector<std::string> defines,
    std::vector<std::pair<std::string, std::string>>
        virtual_includes) {
    return loadFromFileForStage(
        path, shaderStageFromPath(path), std::move(defines),
        std::move(virtual_includes), path.generic_string());
}

ShaderBundleId ShaderLibrary::loadGeneratedSource(
    std::string_view source, ShaderStage stage,
    std::string_view name, std::vector<std::string> defines,
    std::vector<std::pair<std::string, std::string>> virtual_includes) {
    auto bundle = buildFromEngineSource(
        source, stage, name, 1, std::move(defines),
        std::move(virtual_includes));
    bundle.engine_generated = true;
    bundle_ids.reserve(bundle_ids.size() + 1);
    const auto id = bundles.reg(std::move(bundle));
    bundle_ids.push_back(id);
    return id;
}

ShaderBundleId ShaderLibrary::loadResolvedReference(const ResolvedRef &resolved,
                                                    const ShaderReference &reference,
                                                    std::string_view display_name,
                                                    const std::vector<std::string> &defines,
                                                    const std::vector<std::pair<std::string, std::string>>
                                                        &virtual_includes) {
    if (const auto path = std::get_if<std::filesystem::path>(&resolved)) {
        return loadFromFileForStage(
            *path, reference.stage, defines, virtual_includes,
            std::string{display_name});
    }
    if (const auto fragment = std::get_if<ResolvedPathFragment>(&resolved)) {
        throwUnsupportedFragment(fragment->fragment);
    }
    if (const auto fragment = std::get_if<ResolvedEngineFragment>(&resolved)) {
        throwUnsupportedFragment(fragment->fragment);
    }

    const auto &engine_id = std::get<EngineResourceId>(resolved).id;
    const auto resource = engineResourceOrThrow(engine_id);
    ShaderBundle bundle;
    if (lowerExtension(engine_id) == ".spv") {
        if (definesNeedGlslSource(defines) || !virtual_includes.empty()) {
            throw std::runtime_error(
                "Shader defines and generated includes require GLSL source, not SPIR-V: engine://" +
                engine_id);
        }
        bundle = buildFromSpirv(
            bytesToSpirv(resource, engine_id), reference.stage, {},
            "engine://" + engine_id, 1, std::string{display_name});
    } else {
        bundle = buildFromEngineSource(
            resource, reference.stage, display_name, 1,
            defines, virtual_includes);
    }
    bundle_ids.reserve(bundle_ids.size() + 1);
    const auto id = bundles.reg(std::move(bundle));
    bundle_ids.push_back(id);
    return id;
}

std::string shaderStemCandidateReference(std::string_view stem, ShaderStage stage, bool spirv) {
    return shaderSourceCandidateReference(stem, sourceStage(stage), spirv);
}

std::optional<ResolvedRef> existingShaderStemCandidate(ResolvedRef resolved, std::string_view candidate_ref,
                                                       std::vector<std::string> *tried) {
    if (const auto path = std::get_if<std::filesystem::path>(&resolved)) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(*path, ec) || ec) {
            if (tried) tried->push_back(std::string{candidate_ref} + " (" + path->string() + ")");
            return std::nullopt;
        }
        return resolved;
    }

    const auto *engine_resource = std::get_if<EngineResourceId>(&resolved);
    if (engine_resource == nullptr) {
        if (const auto fragment = std::get_if<ResolvedPathFragment>(&resolved)) {
            throwUnsupportedFragment(fragment->fragment);
        }
        if (const auto fragment = std::get_if<ResolvedEngineFragment>(&resolved)) {
            throwUnsupportedFragment(fragment->fragment);
        }
        throw std::runtime_error("Unsupported resolved shader reference");
    }
    if (!engineResource(engine_resource->id)) {
        if (tried) tried->push_back(std::string{candidate_ref} + " (engine id not registered)");
        return std::nullopt;
    }
    return resolved;
}

ShaderBundleId ShaderLibrary::loadFromStemReference(const ShaderReference &reference,
                                                    const PathResolver &resolver,
                                                    const std::vector<std::string> &defines,
                                                    const std::vector<std::pair<std::string, std::string>>
                                                        &virtual_includes) {
    std::vector<std::string> candidate_refs;
#if PELICAN_RUNTIME_SHADER_COMPILER
    candidate_refs.push_back(shaderSourceCandidateReference(
        reference.ref, sourceStage(reference.stage), false));
#endif
    candidate_refs.push_back(shaderSourceCandidateReference(
        reference.ref, sourceStage(reference.stage), true));

    std::vector<std::string> tried;
    for (const auto &candidate_ref : candidate_refs) {
        const auto resolved = existingShaderStemCandidate(
            resolver.resolveProjectRef(candidate_ref), candidate_ref, &tried);
        if (!resolved) continue;
        return loadResolvedReference(
            *resolved, reference, candidate_ref, defines,
            virtual_includes);
    }

    throw std::runtime_error("Shader stem could not be resolved: " + reference.ref +
                             " (" + shaderStageName(reference.stage) + "). Tried: " +
                             joinTriedCandidates(tried));
}

ShaderBundleId ShaderLibrary::loadFromReference(const ShaderReference &reference,
                                                const PathResolver &resolver,
                                                bool project_context,
                                                std::vector<std::string> defines,
                                                std::vector<std::pair<std::string, std::string>>
                                                    virtual_includes) {
    if (reference.kind == ShaderReferenceKind::stem) {
        return loadFromStemReference(
            reference, resolver, defines,
            virtual_includes);
    }
    if (!project_context && !hasScheme(reference.ref)) {
        return loadFromFileForStage(
            reference.ref, reference.stage, std::move(defines),
            std::move(virtual_includes), reference.ref);
    }
    return loadResolvedReference(
        resolver.resolveProjectRef(reference.ref), reference,
        reference.ref, defines, virtual_includes);
}

ShaderBundleId ShaderLibrary::loadFromBytes(
    size_t len, const char *data, ShaderStage expected_stage,
    std::string_view name) {
    if (data == nullptr || len == 0) {
        throw std::runtime_error("Shader module data must not be empty");
    }
    if (len % sizeof(uint32_t) != 0) {
        throw std::runtime_error("Shader module data size must be a multiple of 4 bytes");
    }

    std::vector<uint32_t> spirv(len / sizeof(uint32_t));
    std::memcpy(spirv.data(), data, len);
    return loadFromSpirv(spirv, expected_stage, name);
}

ShaderBundleId ShaderLibrary::loadFromSpirv(
    std::span<const uint32_t> spirv, ShaderStage expected_stage,
    std::string_view name) {
    bundle_ids.reserve(bundle_ids.size() + 1);
    const auto id = bundles.reg(buildFromSpirv(
        spirv, expected_stage, {}, memoryDiagnosticUri(name), 1,
        std::string{name}));
    bundle_ids.push_back(id);
    return id;
}

SurfaceShaderBundleIds ShaderLibrary::loadFromSurface(const SurfaceFormatDocument &surface,
                                                      std::string_view source_name,
                                                      SurfacePass pass,
                                                      std::vector<std::string> defines,
                                                      std::optional<MaterialOutputSchema>
                                                          material_output_schema) {
#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto requested_defines = defines;
    const auto requested_output_schema =
        material_output_schema;
    const auto reloadable = resolveReloadableSurface(source_name);
    const auto source_path = reloadable ? reloadable->first : std::filesystem::path{};
    const auto composition = composeSurfaceShaders(
        surface, source_name, pass, defines,
        material_output_schema);
    const auto result = compileSurfaceShaders(
        compiler, surface, source_name, pass, defines,
        material_output_schema);
    if (!result.vertex.ok || !result.fragment.ok) {
        std::ostringstream message;
        message << "Surface shader compile failed: " << source_name << " ("
                << surfacePassName(pass) << ")";
        if (!result.vertex.ok) message << "\nvertex:\n" << result.vertex.log;
        if (!result.fragment.ok) message << "\nfragment:\n" << result.fragment.log;
        throw std::runtime_error(message.str());
    }

    const auto vertex_name = std::string{source_name} + "#" +
                             std::string{surfacePassName(pass)} + ".vert";
    const auto fragment_name = std::string{source_name} + "#" +
                               std::string{surfacePassName(pass)} + ".frag";
    auto vertex_bundle = buildFromSpirv(
        result.vertex.spirv, ShaderStage::vertex, source_path,
        memoryDiagnosticUri(vertex_name), 1, vertex_name,
        composition.defines);
    vertex_bundle.binding_table = result.vertex_bindings;
    vertex_bundle.cache_key = result.vertex_cache_key.empty()
                                  ? result.vertex.cache_key : result.vertex_cache_key;
    vertex_bundle.cache_hit = result.vertex.cache_hit;
    vertex_bundle.dependency_paths = result.vertex.dependencies;
    appendPathUnique(vertex_bundle.dependency_paths, source_path);
    auto fragment_bundle = buildFromSpirv(
        result.fragment.spirv, ShaderStage::fragment, source_path,
        memoryDiagnosticUri(fragment_name), 1, fragment_name,
        composition.defines);
    fragment_bundle.compiler_resource_interface =
        compilerOwnedSurfaceResourceInterface(
            composition, surface);
    fragment_bundle.surface_resource_interface =
        composition.resource_interface;
    if (composition.material_output_schema) {
        validateFragmentOutputSchema(
            fragment_bundle.reflection,
            *composition.material_output_schema,
            fragment_name);
        fragment_bundle.material_output_schema =
            composition.material_output_schema;
    }
    fragment_bundle.binding_table = result.fragment_bindings;
    fragment_bundle.cache_key = result.fragment_cache_key.empty()
                                    ? result.fragment.cache_key : result.fragment_cache_key;
    fragment_bundle.cache_hit = result.fragment.cache_hit;
    fragment_bundle.dependency_paths = result.fragment.dependencies;
    appendPathUnique(fragment_bundle.dependency_paths, source_path);
    bundle_ids.reserve(bundle_ids.size() + 2);
    const auto vertex = bundles.reg(std::move(vertex_bundle));
    bundle_ids.push_back(vertex);
    const auto fragment = bundles.reg(std::move(fragment_bundle));
    bundle_ids.push_back(fragment);
    const SurfaceShaderBundleIds ids{vertex, fragment};
    if (reloadable) {
        registerSurfaceReloadUnit(ids, bundles.get(vertex), bundles.get(fragment),
                                  reloadable->first, reloadable->second,
                                  std::string{source_name}, pass,
                                  requested_defines,
                                  requested_output_schema);
    } else {
        // A generated surface cannot be file-triggered, but it remains a
        // compiler input and must participate when the graph changes the
        // strategy-private material ABI.
        registerSurfaceReloadUnit(
            ids, bundles.get(vertex),
            bundles.get(fragment), {},
            watch::AssetKey{
                .path = std::string{source_name}},
            std::string{source_name}, pass,
            requested_defines,
            requested_output_schema, surface);
    }
    return ids;
#else
    (void)surface;
    (void)pass;
    (void)defines;
    (void)material_output_schema;
    throw std::runtime_error("Runtime shader compiler is disabled; B-layer surface source is unavailable: " +
                             std::string{source_name});
#endif
}

const ShaderBundle &ShaderLibrary::get(ShaderBundleId id) const { return bundles.get(id); }

bool ShaderLibrary::runtimeCompilerAvailable() noexcept {
#if PELICAN_RUNTIME_SHADER_COMPILER
    return true;
#else
    return false;
#endif
}

SurfaceShaderBundleIds ShaderLibrary::compileWithAdditionalDefines(
    ShaderBundleId vertex, ShaderBundleId fragment,
    std::span<const std::string> additional_defines) {
#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto appended = [&](std::vector<std::string> defines) {
        for (const auto &define : additional_defines) {
            if (std::find(defines.begin(), defines.end(), define) ==
                defines.end()) {
                defines.push_back(define);
            }
        }
        return defines;
    };
    if (const auto unit = unit_by_bundle.find(fragment);
        unit != unit_by_bundle.end()) {
        const auto &reload = reload_units.at(unit->second);
        if (const auto *surface =
                std::get_if<SurfaceReloadRecipe>(&reload.recipe);
            surface != nullptr && reload.bundle_ids.size() == 2 &&
            reload.bundle_ids[0] == vertex &&
            reload.bundle_ids[1] == fragment) {
            const auto document =
                surface->embedded_surface
                    ? *surface->embedded_surface
                    : parseSurfaceFormat(
                          readBinaryFile(surface->path.string()),
                          surface->source_name);
            return loadFromSurface(
                document, surface->source_name, surface->pass,
                appended(surface->defines),
                surface->material_output_schema);
        }
    }
    const auto compile_file = [&](ShaderBundleId id) {
        const auto &bundle = bundles.get(id);
        if (bundle.source_path.empty() ||
            lowerExtension(bundle.source_path) == ".spv") {
            throw std::runtime_error(
                "shader '" + bundle.diagnostic_uri +
                "' has no GLSL source to compile defines into");
        }
        auto defines = bundle.defines;
        auto virtual_includes = bundle.virtual_includes;
        auto diagnostic_uri = bundle.diagnostic_uri;
        if (const auto unit = unit_by_bundle.find(id);
            unit != unit_by_bundle.end()) {
            if (const auto *file = std::get_if<FileReloadRecipe>(
                    &reload_units.at(unit->second).recipe)) {
                defines = file->defines;
                virtual_includes = file->virtual_includes;
                diagnostic_uri = file->diagnostic_uri;
            }
        }
        return loadFromFileForStage(
            bundle.source_path, bundle.expected_stage,
            appended(std::move(defines)),
            std::move(virtual_includes), std::move(diagnostic_uri));
    };
    const auto compiled_vertex = compile_file(vertex);
    try {
        return {compiled_vertex, compile_file(fragment)};
    } catch (...) {
        retireRegistrations({compiled_vertex});
        throw;
    }
#else
    (void)vertex;
    (void)fragment;
    (void)additional_defines;
    throw std::runtime_error(
        "Runtime shader compiler is disabled; material defines cannot "
        "be compiled");
#endif
}

PreparedShaderReload
ShaderLibrary::prepareUnits(const std::set<std::size_t> &units,
                            std::vector<watch::AssetKey> changed_keys,
                            std::span<const SurfaceShaderReloadOverride>
                                surface_overrides) const {
    PreparedShaderReload prepared;
    prepared.changed_keys = std::move(changed_keys);
    std::ranges::sort(prepared.changed_keys);
    prepared.changed_keys.erase(
        std::unique(prepared.changed_keys.begin(), prepared.changed_keys.end()),
        prepared.changed_keys.end());

    const auto add_candidate = [&](ShaderBundleId id, std::size_t unit_index,
                                   ShaderBundle replacement) {
        if (replacement.cache_hit) ++prepared.cache_hits;
        else ++prepared.cache_misses;
        prepared.candidates.push_back(
            PreparedShaderBundle{id, std::move(replacement), unit_index});
    };

    for (const auto unit_index : units) {
        if (unit_index >= reload_units.size()) {
            throw std::runtime_error("shader reload unit is stale");
        }
        const auto &unit = reload_units[unit_index];
        if (const auto *file = std::get_if<FileReloadRecipe>(&unit.recipe)) {
            if (unit.bundle_ids.size() != 1) {
                throw std::runtime_error("file shader reload unit is malformed");
            }
            const auto id = unit.bundle_ids.front();
            add_candidate(id, unit_index,
                          buildFromFile(file->path, file->expected_stage,
                                        bundles.get(id).version + 1,
                                        file->defines,
                                        file->virtual_includes,
                                        file->diagnostic_uri));
            continue;
        }

        const auto &surface_recipe = std::get<SurfaceReloadRecipe>(unit.recipe);
        if (unit.bundle_ids.size() != 2) {
            throw std::runtime_error("surface shader reload unit is malformed");
        }
        const auto fragment_id = unit.bundle_ids[1];
        const auto override = std::find_if(
            surface_overrides.begin(),
            surface_overrides.end(),
            [fragment_id](const auto &candidate) {
                return candidate.fragment == fragment_id;
            });
        auto effective_defines =
            surface_recipe.defines;
        auto effective_output_schema =
            surface_recipe.material_output_schema;
        if (override != surface_overrides.end()) {
            std::erase_if(
                effective_defines,
                [](const auto &define) {
                    return isCompilerOwnedSurfaceDefine(
                        define);
                });
            for (const auto &define :
                 override->physical_defines) {
                if (std::find(
                        effective_defines.begin(),
                        effective_defines.end(),
                        define) ==
                    effective_defines.end()) {
                    effective_defines.push_back(
                        define);
                }
            }
            effective_output_schema =
                override->material_output_schema;
            prepared.surface_recipe_updates.push_back({
                .reload_unit = unit_index,
                .defines = effective_defines,
                .material_output_schema =
                    effective_output_schema,
            });
        }
        auto surface = prepared.surface_documents.find(surface_recipe.source);
        if (surface == prepared.surface_documents.end()) {
            if (surface_recipe.embedded_surface) {
                surface =
                    prepared.surface_documents
                        .emplace(
                            surface_recipe.source,
                            *surface_recipe
                                 .embedded_surface)
                        .first;
            } else {
                const auto source =
                    readBinaryFile(
                        surface_recipe.path.string());
                surface =
                    prepared.surface_documents
                        .emplace(
                            surface_recipe.source,
                            parseSurfaceFormat(
                                source,
                                surface_recipe
                                    .source_name))
                        .first;
            }
        }
        const auto composition = composeSurfaceShaders(
            surface->second, surface_recipe.source_name, surface_recipe.pass,
            effective_defines,
            effective_output_schema);
        const auto compiled = compileSurfaceShaders(
            compiler, surface->second, surface_recipe.source_name,
            surface_recipe.pass, effective_defines,
            effective_output_schema);
        if (!compiled.vertex.ok || !compiled.fragment.ok) {
            std::ostringstream message;
            message << "Surface shader compile failed: " << surface_recipe.source_name
                    << " (" << surfacePassName(surface_recipe.pass) << ")";
            if (!compiled.vertex.ok) message << "\nvertex:\n" << compiled.vertex.log;
            if (!compiled.fragment.ok) message << "\nfragment:\n" << compiled.fragment.log;
            throw std::runtime_error(message.str());
        }

        const auto vertex_id = unit.bundle_ids[0];
        auto vertex = buildFromSpirv(
            compiled.vertex.spirv, ShaderStage::vertex,
            surface_recipe.path,
            memoryDiagnosticUri(surface_recipe.source_name + "#" +
                                std::string{surfacePassName(
                                    surface_recipe.pass)} + ".vert"),
            bundles.get(vertex_id).version + 1,
            surface_recipe.source_name + "#" +
                std::string{surfacePassName(surface_recipe.pass)} + ".vert",
            composition.defines);
        vertex.binding_table = compiled.vertex_bindings;
        vertex.cache_key = compiled.vertex_cache_key.empty()
                               ? compiled.vertex.cache_key : compiled.vertex_cache_key;
        vertex.cache_hit = compiled.vertex.cache_hit;
        vertex.dependency_paths = compiled.vertex.dependencies;
        appendPathUnique(vertex.dependency_paths, surface_recipe.path);

        auto fragment = buildFromSpirv(
            compiled.fragment.spirv, ShaderStage::fragment,
            surface_recipe.path,
            memoryDiagnosticUri(surface_recipe.source_name + "#" +
                                std::string{surfacePassName(
                                    surface_recipe.pass)} + ".frag"),
            bundles.get(fragment_id).version + 1,
            surface_recipe.source_name + "#" +
                std::string{surfacePassName(surface_recipe.pass)} + ".frag",
            composition.defines);
        fragment.compiler_resource_interface =
            compilerOwnedSurfaceResourceInterface(
                composition, surface->second);
        fragment.surface_resource_interface =
            composition.resource_interface;
        if (effective_output_schema) {
            validateFragmentOutputSchema(
                fragment.reflection,
                *effective_output_schema,
                surface_recipe.source_name +
                    "#reload.frag");
            fragment.material_output_schema =
                effective_output_schema;
        }
        fragment.binding_table = compiled.fragment_bindings;
        fragment.cache_key = compiled.fragment_cache_key.empty()
                                 ? compiled.fragment.cache_key : compiled.fragment_cache_key;
        fragment.cache_hit = compiled.fragment.cache_hit;
        fragment.dependency_paths = compiled.fragment.dependencies;
        appendPathUnique(fragment.dependency_paths, surface_recipe.path);

        add_candidate(vertex_id, unit_index, std::move(vertex));
        add_candidate(fragment_id, unit_index, std::move(fragment));
    }
    return prepared;
}

SurfaceShaderBundleIds ShaderLibrary::loadFromSurfaceForMaterial(
    const SurfaceFormatDocument &surface, std::string_view source_name,
    const LoweredMaterial &material,
    std::vector<std::string> additional_defines) {
    auto defines = material.defines;
    for (auto &define : additional_defines) {
        if (std::find(defines.begin(), defines.end(), define) == defines.end()) {
            defines.push_back(std::move(define));
        }
    }
    if (std::any_of(
            defines.begin(), defines.end(),
            isCompilerOwnedSurfaceDefine)) {
        throw std::runtime_error(
            "material defines cannot override the compiler-owned "
            "physical shader ABI");
    }
    std::optional<MaterialOutputSchema> output_schema;
    if (const auto *passes =
            FastModuleContainer::tryGet<
                RenderingPassContainer>()) {
        output_schema = passes->materialOutputSchema(
            material.route, material.exact_pass);
        std::vector<MaterialPassShaderInputRequest>
            requested_inputs;
        requested_inputs.reserve(
            material.screen_inputs.size() +
            material.resource_ports.size());
        for (const auto &input :
             material.screen_inputs) {
            requested_inputs.push_back({
                .kind =
                    MaterialPassShaderInputKind::
                        screen_input,
                .name = input,
            });
        }
        std::vector<std::size_t>
            image_resource_indices;
        for (std::size_t index = 0;
             index < material.resource_ports.size();
             ++index) {
            if (material.resource_ports[index].kind !=
                SurfaceResourcePortKind::image) {
                continue;
            }
            requested_inputs.push_back({
                .kind =
                    MaterialPassShaderInputKind::
                        material_resource,
                .name =
                    material.resource_ports[index].name,
            });
            image_resource_indices.push_back(index);
        }
        const auto physical_inputs =
            passes->materialPassShaderInputBindings(
                material.route,
                materialShaderContractForRoute(
                    material.route),
                requested_inputs,
                material.exact_pass);
        for (std::size_t index = 0;
             index < material.screen_inputs.size();
             ++index) {
            if (physical_inputs[index]
                    .input_attachment_index) {
                defines.push_back(
                    makeSurfaceScreenInputLocalReadDefine(
                        index,
                        *physical_inputs[index]
                             .input_attachment_index));
            }
        }
        for (std::size_t index = 0;
             index < image_resource_indices.size();
             ++index) {
            const auto physical_index =
                material.screen_inputs.size() + index;
            if (physical_inputs[physical_index]
                    .input_attachment_index) {
                const auto &physical =
                    physical_inputs[physical_index];
                if (!physical.input_attachment_extent) {
                    throw std::runtime_error(
                        "material resource port '" +
                        physical.input.name +
                        "' input attachment has no resolved extent");
                }
                const auto local_defines =
                    makeSurfaceResourceLocalReadDefines(
                        image_resource_indices[index],
                        *physical.input_attachment_index,
                        *physical.input_attachment_extent);
                defines.insert(
                    defines.end(),
                    local_defines.begin(),
                    local_defines.end());
            } else if (
                physical_inputs[physical_index]
                        .descriptor_dimension ==
                    ImageSubresourceViewDimension::
                        cube) {
                defines.push_back(
                    makeSurfaceResourceCubeDefine(
                        image_resource_indices[index]));
            } else if (
                physical_inputs[physical_index]
                        .descriptor_dimension ==
                    ImageSubresourceViewDimension::
                        two_d_array) {
                defines.push_back(
                    makeSurfaceResourceLayeredDefine(
                        image_resource_indices[index]));
            }
        }
    }
    return loadFromSurface(
        surface, source_name,
        surfacePassForMaterialRoute(material.route),
        std::move(defines), std::move(output_schema));
}

bool ShaderLibrary::handlesReload(const watch::AssetKey &key) const {
    return units_by_dependency.contains(key);
}

PreparedShaderReload
ShaderLibrary::prepareReload(std::span<const watch::AssetKey> changed_keys) const {
    return prepareReload(changed_keys, {});
}

PreparedShaderReload ShaderLibrary::prepareReload(
    std::span<const watch::AssetKey> changed_keys,
    std::span<const SurfaceShaderReloadOverride>
        surface_overrides) const {
    std::set<std::size_t> units;
    for (const auto &key : changed_keys) {
        const auto found = units_by_dependency.find(key);
        if (found == units_by_dependency.end()) continue;
        units.insert(found->second.begin(), found->second.end());
    }
    std::vector<watch::AssetKey> effective_changed{
        changed_keys.begin(), changed_keys.end()};
    std::set<ShaderBundleId> override_fragments;
    for (const auto &override : surface_overrides) {
        if (!override_fragments.insert(
                override.fragment).second) {
            throw std::runtime_error(
                "surface shader reload override is duplicated");
        }
        const auto found =
            unit_by_bundle.find(override.fragment);
        if (found == unit_by_bundle.end()) {
            throw std::runtime_error(
                "material shader requires a graph-coordinated "
                "recompile but has no retained surface compiler "
                "recipe");
        }
        const auto unit_index = found->second;
        const auto &unit = reload_units.at(unit_index);
        if (unit.bundle_ids.size() != 2 ||
            unit.bundle_ids[1] !=
                override.fragment ||
            !std::holds_alternative<
                SurfaceReloadRecipe>(unit.recipe)) {
            throw std::runtime_error(
                "surface shader reload override does not "
                "identify a fragment reload unit");
        }
        units.insert(unit_index);
        effective_changed.push_back(
            unit.primary_source);
    }
    return prepareUnits(
        units, std::move(effective_changed),
        surface_overrides);
}

PreparedShaderReload ShaderLibrary::prepareReloadAll() const {
    std::set<std::size_t> units;
    std::vector<watch::AssetKey> changed;
    for (std::size_t index = 0; index < reload_units.size(); ++index) {
        units.insert(index);
        changed.push_back(reload_units[index].primary_source);
    }
    return prepareUnits(units, std::move(changed));
}

ShaderLibrary::ChangedShaderReload ShaderLibrary::prepareReloadChanged() const {
    ChangedShaderReload result;
    auto &merged = result.prepared;
    for (std::size_t index = 0; index < reload_units.size(); ++index) {
        const auto &source = reload_units[index].primary_source;
        PreparedShaderReload prepared;
        try {
            prepared = prepareUnits({index}, {source});
        } catch (const std::exception &error) {
            if (!result.error.empty()) result.error += "; ";
            result.error += error.what();
            merged.changed_keys.push_back(source);
            continue;
        }
        const bool unchanged =
            !prepared.candidates.empty() &&
            std::ranges::all_of(prepared.candidates, [&](const auto &candidate) {
                const auto &published = bundles.get(candidate.id).spirv_sha256;
                return !published.empty() &&
                       published == candidate.replacement.spirv_sha256;
            });
        // A unit whose last reload failed is republished once it compiles,
        // even to what is already published: that is what clears its error.
        if (unchanged && !units_failed_since_publish.contains(index)) continue;
        merged.cache_hits += prepared.cache_hits;
        merged.cache_misses += prepared.cache_misses;
        for (auto &candidate : prepared.candidates) {
            merged.candidates.push_back(std::move(candidate));
        }
        for (auto &[key, document] : prepared.surface_documents) {
            merged.surface_documents.emplace(key, std::move(document));
        }
        for (auto &update : prepared.surface_recipe_updates) {
            merged.surface_recipe_updates.push_back(std::move(update));
        }
        merged.changed_keys.push_back(source);
    }
    std::ranges::sort(merged.changed_keys);
    merged.changed_keys.erase(
        std::unique(merged.changed_keys.begin(), merged.changed_keys.end()),
        merged.changed_keys.end());
    return result;
}

void ShaderLibrary::activatePrepared(PreparedShaderReload &prepared) {
    for (auto &candidate : prepared.candidates) {
        using std::swap;
        swap(bundles.get(candidate.id), candidate.replacement);
    }
}

void ShaderLibrary::finalizePrepared(const PreparedShaderReload &prepared) {
    for (const auto &update :
         prepared.surface_recipe_updates) {
        if (update.reload_unit >=
            reload_units.size()) {
            throw std::runtime_error(
                "prepared surface shader recipe is stale");
        }
        auto *recipe = std::get_if<
            SurfaceReloadRecipe>(
            &reload_units[update.reload_unit]
                 .recipe);
        if (recipe == nullptr) {
            throw std::runtime_error(
                "prepared surface shader recipe changed kind");
        }
        recipe->defines = update.defines;
        recipe->material_output_schema =
            update.material_output_schema;
    }
    std::set<std::size_t> affected_units;
    for (const auto &candidate : prepared.candidates) {
        affected_units.insert(candidate.reload_unit);
    }
    for (const auto unit_index : affected_units) {
        units_failed_since_publish.erase(unit_index);
        auto &unit = reload_units.at(unit_index);
        std::vector<watch::AssetKey> dependencies{unit.primary_source};
        for (const auto id : unit.bundle_ids) {
            auto bundle_dependencies = logicalDependencies(bundles.get(id));
            dependencies.insert(dependencies.end(), bundle_dependencies.begin(),
                                bundle_dependencies.end());
        }
        std::ranges::sort(dependencies);
        dependencies.erase(std::unique(dependencies.begin(), dependencies.end()),
                           dependencies.end());
        unit.dependencies = std::move(dependencies);
    }
    rebuildReverseDependencies();
}

void ShaderLibrary::recordReloadFailure(
    std::span<const watch::AssetKey> changed_keys, std::string_view error) {
    std::set<std::size_t> units;
    if (changed_keys.empty()) {
        for (std::size_t index = 0; index < reload_units.size(); ++index) units.insert(index);
    } else {
        for (const auto &key : changed_keys) {
            const auto found = units_by_dependency.find(key);
            if (found != units_by_dependency.end()) {
                units.insert(found->second.begin(), found->second.end());
            }
        }
    }
    for (const auto unit_index : units) {
        for (const auto id : reload_units[unit_index].bundle_ids) {
            bundles.get(id).log = std::string{error};
        }
    }
    // F-5: the units the failure is about are those whose own source was
    // named; the dependency closure also takes in every unit whose include
    // roots hold the file. With no unit named, it is the closure.
    std::set<std::size_t> named;
    for (const auto unit_index : units) {
        const auto &source = reload_units[unit_index].primary_source;
        if (std::ranges::find(changed_keys, source) != changed_keys.end())
            named.insert(unit_index);
    }
    const auto &failed = named.empty() ? units : named;
    units_failed_since_publish.insert(failed.begin(), failed.end());
}

ShaderReloadTrackingStatus ShaderLibrary::reloadTrackingStatus() const noexcept {
    return {reload_units.size(), unit_by_bundle.size(), units_by_dependency.size()};
}

std::vector<watch::AssetKey> ShaderLibrary::failedReloadSources() const {
    std::vector<watch::AssetKey> sources;
    for (const auto unit : units_failed_since_publish)
        if (unit < reload_units.size()) sources.push_back(reload_units[unit].primary_source);
    std::ranges::sort(sources);
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
    return sources;
}

bool ShaderLibrary::reload(ShaderBundleId id) {
    auto &current = bundles.get(id);
    if (current.source_path.empty()) {
        current.log = "Shader bundle has no source path";
        return false;
    }

    try {
        if (const auto found = unit_by_bundle.find(id); found != unit_by_bundle.end()) {
            const auto source = reload_units[found->second].primary_source;
            auto prepared = prepareReload(std::span{&source, std::size_t{1}});
            const auto affected = prepared.affectedBundleIds();
            activatePrepared(prepared);
            finalizePrepared(prepared);
            for (const auto affected_id : affected) markDirty(affected_id);
        } else {
            auto replacement = buildFromFile(current.source_path,
                                             current.expected_stage,
                                             current.version + 1,
                                             current.defines,
                                             current.virtual_includes,
                                             current.diagnostic_uri);
            current = std::move(replacement);
            markDirty(id);
        }
        return true;
    } catch (const std::exception &ex) {
        if (const auto found = unit_by_bundle.find(id); found != unit_by_bundle.end()) {
            const auto source = reload_units[found->second].primary_source;
            recordReloadFailure(std::span{&source, std::size_t{1}}, ex.what());
        } else {
            current.log = ex.what();
        }
        return false;
    }
}

std::vector<ShaderBundleId> ShaderLibrary::takeDirtyBundles() {
    auto dirty = std::move(dirty_bundles);
    dirty_bundles.clear();
    return dirty;
}

ShaderLibrary::RegistrationCheckpoint
ShaderLibrary::checkpointRegistrations() const {
    return RegistrationCheckpoint{
        .bundle_count = bundle_ids.size(),
        .reload_unit_count = reload_units.size(),
        .unit_by_bundle = unit_by_bundle,
        .units_by_dependency = units_by_dependency,
        .dirty_bundles = dirty_bundles,
    };
}

void ShaderLibrary::rollbackRegistrations(
    RegistrationCheckpoint &checkpoint) {
    if (checkpoint.bundle_count > bundle_ids.size() ||
        checkpoint.reload_unit_count > reload_units.size()) {
        throw std::runtime_error(
            "Shader registration checkpoint is invalid");
    }
    while (bundle_ids.size() > checkpoint.bundle_count) {
        const auto id = bundle_ids.back();
        (void)bundles.extract(id, false);
        bundle_ids.pop_back();
    }
    reload_units.resize(checkpoint.reload_unit_count);
    std::erase_if(units_failed_since_publish, [&](std::size_t unit) {
        return unit >= reload_units.size();
    });
    unit_by_bundle.swap(checkpoint.unit_by_bundle);
    units_by_dependency.swap(
        checkpoint.units_by_dependency);
    dirty_bundles.swap(checkpoint.dirty_bundles);
}

std::vector<ShaderBundleId>
ShaderLibrary::registrationsSince(
    const RegistrationCheckpoint &checkpoint) const {
    if (checkpoint.bundle_count > bundle_ids.size()) {
        throw std::runtime_error(
            "Shader registration checkpoint is invalid");
    }
    return {
        bundle_ids.begin() +
            static_cast<std::ptrdiff_t>(
                checkpoint.bundle_count),
        bundle_ids.end()};
}

void ShaderLibrary::retireRegistrations(
    const std::vector<ShaderBundleId> &ids) noexcept {
    if (ids.empty()) return;
    const std::unordered_set<ShaderBundleId,
                             ShaderBundleId::Hash>
        retiring{ids.begin(), ids.end()};

    std::vector<ShaderBundleId> next_bundle_ids;
    std::vector<ShaderBundleId> next_dirty;
    std::vector<ReloadUnit> next_units;
    std::set<std::size_t> next_failed_units;
    std::unordered_map<ShaderBundleId, std::size_t,
                       ShaderBundleId::Hash>
        next_unit_by_bundle;
    std::map<watch::AssetKey, std::vector<std::size_t>>
        next_units_by_dependency;
    try {
        next_bundle_ids.reserve(bundle_ids.size());
        for (const auto id : bundle_ids) {
            if (!retiring.contains(id)) {
                next_bundle_ids.push_back(id);
            }
        }
        next_dirty.reserve(dirty_bundles.size());
        for (const auto id : dirty_bundles) {
            if (!retiring.contains(id)) {
                next_dirty.push_back(id);
            }
        }
        next_units.reserve(reload_units.size());
        for (std::size_t old_index = 0; old_index < reload_units.size(); ++old_index) {
            const auto &unit = reload_units[old_index];
            const auto retiring_count =
                std::count_if(
                    unit.bundle_ids.begin(),
                    unit.bundle_ids.end(),
                    [&retiring](ShaderBundleId id) {
                        return retiring.contains(id);
                    });
            if (retiring_count == 0) {
                if (units_failed_since_publish.contains(old_index))
                    next_failed_units.insert(next_units.size());
                next_units.push_back(unit);
            } else if (retiring_count !=
                       unit.bundle_ids.size()) {
                // A reload unit is an ownership atom (notably a
                // vertex/fragment .surface pair). Keep the registry
                // intact if a malformed scope tries to split it.
                return;
            }
        }
        for (std::size_t index = 0;
             index < next_units.size(); ++index) {
            for (const auto id :
                 next_units[index].bundle_ids) {
                next_unit_by_bundle.emplace(id, index);
            }
            for (const auto &dependency :
                 next_units[index].dependencies) {
                next_units_by_dependency[dependency]
                    .push_back(index);
            }
        }
    } catch (...) {
        // Allocation failure during retirement must not leave a
        // half-updated reload index. The registry will be reclaimed
        // during engine teardown instead.
        return;
    }

    bundle_ids.swap(next_bundle_ids);
    dirty_bundles.swap(next_dirty);
    reload_units.swap(next_units);
    units_failed_since_publish.swap(next_failed_units);
    unit_by_bundle.swap(next_unit_by_bundle);
    units_by_dependency.swap(
        next_units_by_dependency);

    // A replaced generation can retire a failed shader without another
    // shader reload. Its diagnostic must leave with its registration;
    // failures of surviving shaders retain their original detail.
    try {
        std::vector<std::string> failed;
        for (const auto &key : failedReloadSources()) failed.push_back(watch::assetKeyString(key));
        syncShaderProblems(problemSourceHotReload, failed, {});
    } catch (...) {
    }

    for (const auto id : ids) {
        auto retired = bundles.extract(id, false);
        if (!retired) continue;
        try {
            auto *queue =
                FastModuleContainer::tryGet<DeletionQueue>();
            if (queue != nullptr &&
                queue->acceptingResources()) {
                queue->defer(std::move(*retired));
            }
        } catch (...) {
        }
    }
}

} // namespace Pelican
