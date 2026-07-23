#include "shaderlibrary.hpp"
#include "shadercompiler.hpp"
#include "../loader/engineresources.hpp"
#include "../loader/fileio.hpp"
#include "../loader/pathresolver.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"
#include "../../project/materiallowering.hpp"
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
    }
    throw std::runtime_error("unknown shader stage");
}

std::string appendShaderExtension(std::string_view ref, ShaderStage stage, bool spirv) {
    std::string candidate{ref};
    candidate += shaderStageSourceExtension(stage);
    if (spirv) {
        candidate += ".spv";
    }
    return candidate;
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
                                           std::vector<std::string> defines) {
    const auto source = logicalKeyForPath(bundle.source_path);
    if (!source) return;
    ReloadUnit unit{
        FileReloadRecipe{bundle.source_path, std::move(defines)},
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
    std::vector<std::string> defines) {
    auto dependencies = logicalDependencies(vertex_bundle, source);
    auto fragment_dependencies = logicalDependencies(fragment_bundle, source);
    dependencies.insert(dependencies.end(), fragment_dependencies.begin(),
                        fragment_dependencies.end());
    std::ranges::sort(dependencies);
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()),
                       dependencies.end());
    ReloadUnit unit{
        SurfaceReloadRecipe{std::move(path), source, std::move(source_name), pass,
                            std::move(defines)},
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

ShaderBundle ShaderLibrary::buildFromFile(const std::filesystem::path &path, uint64_t version,
                                          std::vector<std::string> defines) const {
    if (lowerExtension(path) == ".spv") {
        if (!defines.empty()) {
            throw std::runtime_error("Shader defines require GLSL source, not SPIR-V: " + path.string());
        }
        return buildFromSpirv(bytesToSpirv(readBinaryFile(path.string()), path), path, version, "");
    }

#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompileOptions options;
    options.defines = defines;
    const auto result = compiler.compileFile(path, options);
    if (!result.ok) {
        throw std::runtime_error("Shader compile failed: " + path.string() + "\n" + result.log);
    }
    auto bundle = buildFromSpirv(result.spirv, path, version, result.log, std::move(defines));
    bundle.cache_key = result.cache_key;
    bundle.cache_hit = result.cache_hit;
    bundle.dependency_paths = result.dependencies;
    return bundle;
#else
    (void)defines;
    throw std::runtime_error("Runtime shader compiler is disabled; only .spv shader files are accepted: " +
                             path.string());
#endif
}

ShaderBundle ShaderLibrary::buildFromSpirv(std::span<const uint32_t> spirv, std::filesystem::path source_path,
                                           uint64_t version, std::string log,
                                           std::vector<std::string> defines) const {
    ShaderBundle bundle;
    bundle.module = createShaderModule(spirv);
    bundle.reflection = reflect(spirv);
    bundle.source_path = std::move(source_path);
    bundle.defines = std::move(defines);
    bundle.version = version;
    bundle.log = std::move(log);
    appendPathUnique(bundle.dependency_paths, bundle.source_path);
    return bundle;
}

ShaderBundle ShaderLibrary::buildFromEngineSource(std::string_view source, ShaderStage stage,
                                                  std::string_view name, uint64_t version,
                                                  std::vector<std::string> defines) const {
#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompileOptions options;
    options.defines = defines;
    const auto result = compiler.compileSource(source, toVkStage(stage), name, options);
    if (!result.ok) {
        throw std::runtime_error("Shader compile failed: " + std::string{name} + "\n" + result.log);
    }
    auto bundle = buildFromSpirv(result.spirv, {}, version, result.log, std::move(defines));
    bundle.cache_key = result.cache_key;
    bundle.cache_hit = result.cache_hit;
    bundle.dependency_paths = result.dependencies;
    return bundle;
#else
    (void)source;
    (void)stage;
    (void)version;
    (void)defines;
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

ShaderBundleId ShaderLibrary::loadFromFile(const std::filesystem::path &path, std::vector<std::string> defines) {
    auto bundle = buildFromFile(path, 1, defines);
    bundle_ids.reserve(bundle_ids.size() + 1);
    const auto id = bundles.reg(std::move(bundle));
    bundle_ids.push_back(id);
    registerFileReloadUnit(id, bundles.get(id), std::move(defines));
    return id;
}

ShaderBundleId ShaderLibrary::loadResolvedReference(const ResolvedRef &resolved,
                                                    const ShaderReference &reference,
                                                    std::string_view display_name,
                                                    const std::vector<std::string> &defines) {
    if (const auto path = std::get_if<std::filesystem::path>(&resolved)) {
        return loadFromFile(*path, defines);
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
        if (!defines.empty()) {
            throw std::runtime_error("Shader defines require GLSL source, not SPIR-V: engine://" +
                                     engine_id);
        }
        bundle = buildFromSpirv(bytesToSpirv(resource, engine_id), {}, 1, std::string{display_name});
    } else {
        bundle = buildFromEngineSource(resource, reference.stage, display_name, 1, defines);
    }
    bundle_ids.reserve(bundle_ids.size() + 1);
    const auto id = bundles.reg(std::move(bundle));
    bundle_ids.push_back(id);
    return id;
}

ShaderBundleId ShaderLibrary::loadFromStemReference(const ShaderReference &reference,
                                                    const PathResolver &resolver,
                                                    const std::vector<std::string> &defines) {
    std::vector<std::string> candidate_refs;
#if PELICAN_RUNTIME_SHADER_COMPILER
    candidate_refs.push_back(appendShaderExtension(reference.ref, reference.stage, false));
#endif
    candidate_refs.push_back(appendShaderExtension(reference.ref, reference.stage, true));

    std::vector<std::string> tried;
    for (const auto &candidate_ref : candidate_refs) {
        const auto resolved = resolver.resolveProjectRef(candidate_ref);
        if (const auto path = std::get_if<std::filesystem::path>(&resolved)) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(*path, ec) || ec) {
                tried.push_back(candidate_ref + " (" + path->string() + ")");
                continue;
            }
            return loadResolvedReference(resolved, reference, candidate_ref, defines);
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
        const auto &engine_id = engine_resource->id;
        if (!engineResource(engine_id)) {
            tried.push_back(candidate_ref + " (engine id not registered)");
            continue;
        }
        return loadResolvedReference(resolved, reference, candidate_ref, defines);
    }

    throw std::runtime_error("Shader stem could not be resolved: " + reference.ref +
                             " (" + shaderStageName(reference.stage) + "). Tried: " +
                             joinTriedCandidates(tried));
}

ShaderBundleId ShaderLibrary::loadFromReference(const ShaderReference &reference,
                                                const PathResolver &resolver,
                                                bool project_context,
                                                std::vector<std::string> defines) {
    if (reference.kind == ShaderReferenceKind::stem) {
        return loadFromStemReference(reference, resolver, defines);
    }
    if (!project_context && !hasScheme(reference.ref)) {
        return loadFromFile(reference.ref, std::move(defines));
    }
    return loadResolvedReference(resolver.resolveProjectRef(reference.ref), reference, reference.ref, defines);
}

ShaderBundleId ShaderLibrary::loadFromBytes(size_t len, const char *data, std::string_view name) {
    if (data == nullptr || len == 0) {
        throw std::runtime_error("Shader module data must not be empty");
    }
    if (len % sizeof(uint32_t) != 0) {
        throw std::runtime_error("Shader module data size must be a multiple of 4 bytes");
    }

    std::vector<uint32_t> spirv(len / sizeof(uint32_t));
    std::memcpy(spirv.data(), data, len);
    return loadFromSpirv(spirv, name);
}

ShaderBundleId ShaderLibrary::loadFromSpirv(std::span<const uint32_t> spirv, std::string_view name) {
    bundle_ids.reserve(bundle_ids.size() + 1);
    const auto id = bundles.reg(buildFromSpirv(spirv, {}, 1, std::string{name}));
    bundle_ids.push_back(id);
    return id;
}

SurfaceShaderBundleIds ShaderLibrary::loadFromSurface(const SurfaceFormatDocument &surface,
                                                      std::string_view source_name,
                                                      SurfacePass pass,
                                                      std::vector<std::string> defines) {
#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto requested_defines = defines;
    const auto reloadable = resolveReloadableSurface(source_name);
    const auto source_path = reloadable ? reloadable->first : std::filesystem::path{};
    const auto composition = composeSurfaceShaders(surface, source_name, pass, defines);
    const auto result = compileSurfaceShaders(compiler, surface, source_name, pass, defines);
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
    auto vertex_bundle = buildFromSpirv(result.vertex.spirv, source_path, 1, vertex_name,
                                        composition.defines);
    vertex_bundle.binding_table = result.vertex_bindings;
    vertex_bundle.cache_key = result.vertex_cache_key.empty()
                                  ? result.vertex.cache_key : result.vertex_cache_key;
    vertex_bundle.cache_hit = result.vertex.cache_hit;
    vertex_bundle.dependency_paths = result.vertex.dependencies;
    appendPathUnique(vertex_bundle.dependency_paths, source_path);
    auto fragment_bundle = buildFromSpirv(result.fragment.spirv, source_path, 1, fragment_name,
                                          composition.defines);
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
                                  std::string{source_name}, pass, requested_defines);
    }
    return ids;
#else
    (void)surface;
    (void)pass;
    (void)defines;
    throw std::runtime_error("Runtime shader compiler is disabled; B-layer surface source is unavailable: " +
                             std::string{source_name});
#endif
}

const ShaderBundle &ShaderLibrary::get(ShaderBundleId id) const { return bundles.get(id); }

PreparedShaderReload
ShaderLibrary::prepareUnits(const std::set<std::size_t> &units,
                            std::vector<watch::AssetKey> changed_keys) const {
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
                          buildFromFile(file->path, bundles.get(id).version + 1,
                                        file->defines));
            continue;
        }

        const auto &surface_recipe = std::get<SurfaceReloadRecipe>(unit.recipe);
        if (unit.bundle_ids.size() != 2) {
            throw std::runtime_error("surface shader reload unit is malformed");
        }
        auto surface = prepared.surface_documents.find(surface_recipe.source);
        if (surface == prepared.surface_documents.end()) {
            const auto source = readBinaryFile(surface_recipe.path.string());
            surface = prepared.surface_documents
                          .emplace(surface_recipe.source,
                                   parseSurfaceFormat(source, surface_recipe.source_name))
                          .first;
        }
        const auto composition = composeSurfaceShaders(
            surface->second, surface_recipe.source_name, surface_recipe.pass,
            surface_recipe.defines);
        const auto compiled = compileSurfaceShaders(
            compiler, surface->second, surface_recipe.source_name,
            surface_recipe.pass, surface_recipe.defines);
        if (!compiled.vertex.ok || !compiled.fragment.ok) {
            std::ostringstream message;
            message << "Surface shader compile failed: " << surface_recipe.source_name
                    << " (" << surfacePassName(surface_recipe.pass) << ")";
            if (!compiled.vertex.ok) message << "\nvertex:\n" << compiled.vertex.log;
            if (!compiled.fragment.ok) message << "\nfragment:\n" << compiled.fragment.log;
            throw std::runtime_error(message.str());
        }

        const auto vertex_id = unit.bundle_ids[0];
        const auto fragment_id = unit.bundle_ids[1];
        auto vertex = buildFromSpirv(
            compiled.vertex.spirv, surface_recipe.path,
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
            compiled.fragment.spirv, surface_recipe.path,
            bundles.get(fragment_id).version + 1,
            surface_recipe.source_name + "#" +
                std::string{surfacePassName(surface_recipe.pass)} + ".frag",
            composition.defines);
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
    return loadFromSurface(surface, source_name,
                           surfacePassForMaterialRoute(material.route),
                           std::move(defines));
}

bool ShaderLibrary::handlesReload(const watch::AssetKey &key) const {
    return units_by_dependency.contains(key);
}

PreparedShaderReload
ShaderLibrary::prepareReload(std::span<const watch::AssetKey> changed_keys) const {
    std::set<std::size_t> units;
    for (const auto &key : changed_keys) {
        const auto found = units_by_dependency.find(key);
        if (found == units_by_dependency.end()) continue;
        units.insert(found->second.begin(), found->second.end());
    }
    return prepareUnits(units, {changed_keys.begin(), changed_keys.end()});
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

void ShaderLibrary::activatePrepared(PreparedShaderReload &prepared) {
    for (auto &candidate : prepared.candidates) {
        using std::swap;
        swap(bundles.get(candidate.id), candidate.replacement);
    }
}

void ShaderLibrary::finalizePrepared(const PreparedShaderReload &prepared) {
    std::set<std::size_t> affected_units;
    for (const auto &candidate : prepared.candidates) {
        affected_units.insert(candidate.reload_unit);
    }
    for (const auto unit_index : affected_units) {
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
}

ShaderReloadTrackingStatus ShaderLibrary::reloadTrackingStatus() const noexcept {
    return {reload_units.size(), unit_by_bundle.size(), units_by_dependency.size()};
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
            auto replacement = buildFromFile(current.source_path, current.version + 1,
                                             current.defines);
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
        for (const auto &unit : reload_units) {
            const auto retiring_count =
                std::count_if(
                    unit.bundle_ids.begin(),
                    unit.bundle_ids.end(),
                    [&retiring](ShaderBundleId id) {
                        return retiring.contains(id);
                    });
            if (retiring_count == 0) {
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
    unit_by_bundle.swap(next_unit_by_bundle);
    units_by_dependency.swap(
        next_units_by_dependency);

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
