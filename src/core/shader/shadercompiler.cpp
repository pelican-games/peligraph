#include "shadercompiler.hpp"
#include "../loader/engineresources.hpp"
#include "../loader/pathresolver.hpp"
#include "../startup.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <picosha2.h>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if PELICAN_RUNTIME_SHADER_COMPILER
#include <shaderc/shaderc.hpp>
#endif

#ifndef PELICAN_SHADERC_VERSION
#define PELICAN_SHADERC_VERSION "unknown"
#endif

namespace Pelican {

namespace {

std::string readTextFile(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open shader source: " + path.string());
    }

    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

std::filesystem::path normalizedPath(const std::filesystem::path &path) {
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return normalized;
    }
    return std::filesystem::absolute(path, ec);
}

#if PELICAN_RUNTIME_SHADER_COMPILER
constexpr std::string_view shaderCacheFormat = "pelican-shader-cache-v1";
constexpr std::string_view shaderContractSalt = "pelican-shader-contract-v1-wp82-20260712";
constexpr std::string_view shaderTargetEnvironment = "vulkan-1.2";

shaderc_shader_kind toShadercKind(vk::ShaderStageFlagBits stage) {
    switch (stage) {
    case vk::ShaderStageFlagBits::eVertex:
        return shaderc_vertex_shader;
    case vk::ShaderStageFlagBits::eFragment:
        return shaderc_fragment_shader;
    case vk::ShaderStageFlagBits::eCompute:
        return shaderc_compute_shader;
    case vk::ShaderStageFlagBits::eRaygenKHR:
        return shaderc_raygen_shader;
    case vk::ShaderStageFlagBits::eMissKHR:
        return shaderc_miss_shader;
    case vk::ShaderStageFlagBits::eClosestHitKHR:
        return shaderc_closesthit_shader;
    default:
        throw std::runtime_error("Unsupported shader stage for shaderc");
    }
}

struct IncludeResultStorage {
    shaderc_include_result result{};
    std::string source_name;
    std::string content;
};

using VirtualIncludes = std::unordered_map<std::string, std::string>;

std::optional<std::pair<std::string, std::string>> resolveInclude(
    std::string_view requested_source, shaderc_include_type type, std::string_view requesting_source,
    const std::vector<std::filesystem::path> &include_dirs, const VirtualIncludes &virtual_includes) {
    if (const auto found = virtual_includes.find(std::string{requested_source});
        found != virtual_includes.end()) {
        return std::pair{found->first, found->second};
    }

    std::vector<std::filesystem::path> roots;
    if (type == shaderc_include_type_relative && !requesting_source.empty()) {
        const std::filesystem::path requesting_path{requesting_source};
        if (requesting_path.has_parent_path()) {
            roots.push_back(requesting_path.parent_path());
        }
    }
    roots.insert(roots.end(), include_dirs.begin(), include_dirs.end());

    const std::filesystem::path requested_path{requested_source};
    std::vector<std::filesystem::path> candidates;
    if (requested_path.is_absolute()) {
        candidates.push_back(requested_path);
    } else {
        for (const auto &root : roots) {
            candidates.push_back(root / requested_path);
        }
    }
    for (const auto &candidate : candidates) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(candidate, ec) || ec) {
            continue;
        }
        return std::pair{normalizedPath(candidate).string(), readTextFile(candidate)};
    }

    const std::string requested{requested_source};
    for (const auto &engine_id : {requested, "shaders/include/" + requested}) {
        if (const auto resource = engineResource(engine_id)) {
            return std::pair{"engine://" + engine_id, std::string{*resource}};
        }
    }
    return std::nullopt;
}

class FileIncluder final : public shaderc::CompileOptions::IncluderInterface {
    std::vector<std::filesystem::path> include_dirs;
    VirtualIncludes virtual_includes;

    static shaderc_include_result *makeResult(std::string source_name, std::string content) {
        auto storage = std::make_unique<IncludeResultStorage>();
        storage->source_name = std::move(source_name);
        storage->content = std::move(content);
        storage->result.source_name = storage->source_name.c_str();
        storage->result.source_name_length = storage->source_name.size();
        storage->result.content = storage->content.c_str();
        storage->result.content_length = storage->content.size();
        storage->result.user_data = storage.get();
        return &storage.release()->result;
    }

    static shaderc_include_result *makeError(std::string message) { return makeResult("", std::move(message)); }

  public:
    FileIncluder(std::vector<std::filesystem::path> dirs,
                 const std::vector<std::pair<std::string, std::string>> &sources)
        : include_dirs{std::move(dirs)} {
        for (const auto &[name, content] : sources) {
            if (!virtual_includes.emplace(name, content).second) {
                throw std::runtime_error("Duplicate virtual shader include: " + name);
            }
        }
    }

    shaderc_include_result *GetInclude(const char *requested_source, shaderc_include_type type,
                                       const char *requesting_source, size_t) override {
        try {
            const auto resolved = resolveInclude(requested_source, type,
                                                 requesting_source == nullptr ? "" : requesting_source,
                                                 include_dirs, virtual_includes);
            if (resolved) {
                return makeResult(resolved->first, resolved->second);
            }
        } catch (const std::exception &ex) {
            return makeError(ex.what());
        }
        return makeError("Shader include not found: " + std::string{requested_source});
    }

    void ReleaseInclude(shaderc_include_result *data) override {
        if (data == nullptr) {
            return;
        }
        delete static_cast<IncludeResultStorage *>(data->user_data);
    }
};

void appendKeyPart(std::string &key, std::string_view label, std::string_view value) {
    key += std::string{label} + "=" + std::to_string(value.size()) + ":" + std::string{value} + ";";
}

const std::string &engineShaderUniverseSha256() {
    static const std::string result = [] {
        std::string universe;
        for (const auto id : registeredEngineResourceIds()) {
            const auto extension = std::filesystem::path{id}.extension().string();
            if (extension != ".glsl" && extension != ".vert" && extension != ".frag" &&
                extension != ".comp" && extension != ".rgen" && extension != ".rmiss" &&
                extension != ".rchit") {
                continue;
            }
            if (const auto resource = engineResource(id)) {
                appendKeyPart(universe, "potential-engine-name", id);
                appendKeyPart(universe, "potential-engine-source", *resource);
            }
        }
        return picosha2::hash256_hex_string(universe);
    }();
    return result;
}

std::string sourceGraphSha256(std::string_view source, std::string_view name,
                              const std::vector<std::filesystem::path> &include_dirs,
                              const std::vector<std::pair<std::string, std::string>> &virtual_sources,
                              std::unordered_map<std::string, std::string> &source_graph_memory,
                              std::vector<std::filesystem::path> *dependencies) {
    // With no physical include roots the graph cannot acquire file-backed
    // dependencies, so the prior digest-only fast path remains valid. Graphs
    // that may touch files are always rediscovered for hot-reload reverse edges.
    std::string stable_input;
    if (include_dirs.empty()) {
        appendKeyPart(stable_input, "root-name", name);
        appendKeyPart(stable_input, "root-source", source);
        for (const auto &[virtual_name, content] : virtual_sources) {
            appendKeyPart(stable_input, "virtual-name", virtual_name);
            appendKeyPart(stable_input, "virtual-source", content);
        }
        stable_input = picosha2::hash256_hex_string(stable_input);
        if (const auto found = source_graph_memory.find(stable_input);
            found != source_graph_memory.end()) {
            return found->second;
        }
    }
    VirtualIncludes virtual_includes;
    for (const auto &[virtual_name, content] : virtual_sources) {
        virtual_includes.emplace(virtual_name, content);
    }

    std::string graph;
    std::unordered_set<std::string> visited;
    bool has_file_dependency = false;
    const std::regex include_directive{R"(^\s*#\s*include\b)"};
    const std::regex include_pattern{R"(^\s*#\s*include\s*([<"])([^>"]+)[>"])"};
    const auto visit = [&](auto &&self, std::string source_name, std::string content) -> void {
        if (!visited.insert(source_name).second) {
            return;
        }
        appendKeyPart(graph, "source-name", source_name);
        appendKeyPart(graph, "source-bytes", content);
        std::istringstream lines{content};
        std::string line;
        std::smatch match;
        while (std::getline(lines, line)) {
            if (!std::regex_search(line, match, include_pattern)) {
                if (std::regex_search(line, include_directive)) {
                    throw std::runtime_error(
                        "non-literal shader include cannot be cached safely");
                }
                continue;
            }
            const auto type = match[1].str() == "\"" ? shaderc_include_type_relative
                                                        : shaderc_include_type_standard;
            const auto included = resolveInclude(match[2].str(), type, source_name,
                                                 include_dirs, virtual_includes);
            if (included) {
                if (!included->first.starts_with("engine://") &&
                    !virtual_includes.contains(included->first)) {
                    has_file_dependency = true;
                    if (dependencies != nullptr) {
                        dependencies->push_back(normalizedPath(included->first));
                    }
                }
                self(self, included->first, included->second);
            }
        }
    };
    visit(visit, std::string{name}, std::string{source});

    // shaderc also accepts macro-expanded include operands. A text-only
    // dependency scanner cannot prove which file such an operand selects, so
    // the key conservatively covers every virtual source, every regular file
    // below the configured include roots, and every embedded engine shader
    // source. This may create a harmless extra miss, but cannot create a stale
    // hit when an include spelling evades the scanner.
    for (const auto &[virtual_name, content] : virtual_sources) {
        appendKeyPart(graph, "potential-virtual-name", virtual_name);
        appendKeyPart(graph, "potential-virtual-source", content);
    }
    std::vector<std::filesystem::path> potential_files;
    for (const auto &root : include_dirs) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(root, ec) && !ec) {
            potential_files.push_back(normalizedPath(root));
            continue;
        }
        ec.clear();
        for (std::filesystem::recursive_directory_iterator it{
                 root, std::filesystem::directory_options::skip_permission_denied, ec}, end;
             !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && !ec) {
                potential_files.push_back(normalizedPath(it->path()));
            }
            ec.clear();
        }
    }
    std::sort(potential_files.begin(), potential_files.end());
    potential_files.erase(std::unique(potential_files.begin(), potential_files.end()),
                          potential_files.end());
    for (const auto &path : potential_files) {
        appendKeyPart(graph, "potential-file-name", path.string());
        appendKeyPart(graph, "potential-file-source", readTextFile(path));
        if (dependencies != nullptr) dependencies->push_back(path);
    }
    appendKeyPart(graph, "potential-engine-universe-sha256", engineShaderUniverseSha256());
    if (dependencies != nullptr) {
        std::sort(dependencies->begin(), dependencies->end());
        dependencies->erase(std::unique(dependencies->begin(), dependencies->end()),
                            dependencies->end());
    }
    auto result = picosha2::hash256_hex_string(graph);
    if (!stable_input.empty() && !has_file_dependency) {
        source_graph_memory.emplace(std::move(stable_input), result);
    }
    return result;
}

std::string shaderCacheKey(std::string_view source, vk::ShaderStageFlagBits stage,
                           std::string_view name, std::string_view entry_point,
                           const std::vector<std::filesystem::path> &include_dirs,
                           const std::vector<std::string> &defines,
                           const std::vector<std::pair<std::string, std::string>> &virtual_includes,
                           std::unordered_map<std::string, std::string> &source_graph_memory,
                           std::vector<std::filesystem::path> *dependencies) {
    unsigned int spirv_version = 0;
    unsigned int spirv_revision = 0;
    shaderc_get_spv_version(&spirv_version, &spirv_revision);

    std::string raw;
    appendKeyPart(raw, "source-sha256",
                  sourceGraphSha256(source, name, include_dirs, virtual_includes,
                                    source_graph_memory, dependencies));
    for (const auto &define : defines) {
        appendKeyPart(raw, "define", define);
    }
    appendKeyPart(raw, "shaderc-version", PELICAN_SHADERC_VERSION);
    appendKeyPart(raw, "shaderc-spirv-version", std::to_string(spirv_version));
    appendKeyPart(raw, "shaderc-spirv-revision", std::to_string(spirv_revision));
    appendKeyPart(raw, "target-env", shaderTargetEnvironment);
    appendKeyPart(raw, "stage", std::to_string(static_cast<std::uint32_t>(stage)));
    appendKeyPart(raw, "entry-point", entry_point);
    appendKeyPart(raw, "contract-salt", shaderContractSalt);
    return picosha2::hash256_hex_string(raw);
}

std::string spirvSha256(const std::vector<uint32_t> &spirv) {
    const auto *begin = reinterpret_cast<const unsigned char *>(spirv.data());
    return picosha2::hash256_hex_string(begin, begin + spirv.size() * sizeof(uint32_t));
}

struct CacheReadResult {
    std::optional<std::vector<uint32_t>> spirv;
    std::optional<std::string> error;
};

CacheReadResult readCache(const std::filesystem::path &path, std::string_view key) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return {};
    }
    try {
        std::ifstream file{path, std::ios::binary};
        if (!file) {
            return {{}, "entry cannot be opened"};
        }
        std::string format, stored_key, stored_hash, word_count_text;
        if (!std::getline(file, format) || !std::getline(file, stored_key) ||
            !std::getline(file, stored_hash) || !std::getline(file, word_count_text) ||
            format != shaderCacheFormat || stored_key != key) {
            return {{}, "entry header is invalid"};
        }
        const auto word_count = std::stoull(word_count_text);
        if (word_count == 0 || word_count > (1ull << 30)) {
            return {{}, "entry word count is invalid"};
        }
        std::vector<uint32_t> spirv(static_cast<std::size_t>(word_count));
        file.read(reinterpret_cast<char *>(spirv.data()), static_cast<std::streamsize>(spirv.size() * sizeof(uint32_t)));
        if (!file || file.peek() != std::char_traits<char>::eof() || spirv.front() != 0x07230203u ||
            spirvSha256(spirv) != stored_hash) {
            return {{}, "entry payload is corrupt"};
        }
        return {std::move(spirv), {}};
    } catch (const std::exception &ex) {
        return {{}, ex.what()};
    }
}

std::optional<std::string> writeCache(const std::filesystem::path &directory,
                                      const std::filesystem::path &path, std::string_view key,
                                      const std::vector<uint32_t> &spirv) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return "directory cannot be created: " + ec.message();
    }
    const auto nonce = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                       std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const auto temporary = path.string() + ".tmp-" + nonce;
    {
        std::ofstream file{temporary, std::ios::binary | std::ios::trunc};
        if (!file) {
            return "temporary entry cannot be opened";
        }
        file << shaderCacheFormat << '\n' << key << '\n' << spirvSha256(spirv) << '\n'
             << spirv.size() << '\n';
        file.write(reinterpret_cast<const char *>(spirv.data()),
                   static_cast<std::streamsize>(spirv.size() * sizeof(uint32_t)));
        if (!file) {
            std::filesystem::remove(temporary, ec);
            return "temporary entry cannot be written";
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        // Another process may have won the same-key race. Its complete entry is
        // equivalent; otherwise replace the stale/corrupt destination.
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temporary, path, ec);
    }
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return "entry cannot be committed: " + ec.message();
    }
    return std::nullopt;
}

void warnCacheOnce(bool &warned, const std::filesystem::path &path, std::string_view reason) {
    if (warned) {
        return;
    }
    warned = true;
    if (logger != nullptr) {
        LOG_WARNING(logger, "shader cache {}: {}; recompiling", path.string(), reason);
    }
}

ShaderCompileResult compileGlslUncached(std::string_view source, vk::ShaderStageFlagBits stage,
                                       std::string_view name, std::string_view entry_point,
                                       const std::vector<std::filesystem::path> &include_dirs,
                                       const std::vector<std::string> &defines,
                                       const std::vector<std::pair<std::string, std::string>> &virtual_includes) {
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    options.SetIncluder(std::make_unique<FileIncluder>(include_dirs, virtual_includes));
    for (const auto &define : defines) {
        options.AddMacroDefinition(define);
    }

    const std::string source_text{source};
    const std::string source_name{name};
    const std::string entry{entry_point};
    auto result = compiler.CompileGlslToSpv(source_text, toShadercKind(stage), source_name.c_str(), entry.c_str(),
                                            options);

    ShaderCompileResult output;
    output.log = result.GetErrorMessage();
    output.ok = result.GetCompilationStatus() == shaderc_compilation_status_success;
    if (output.ok) {
        output.spirv.assign(result.cbegin(), result.cend());
    }
    return output;
}

ShaderCompileResult compileGlsl(std::string_view source, vk::ShaderStageFlagBits stage,
                                std::string_view name, std::string_view entry_point,
                                const std::vector<std::filesystem::path> &include_dirs,
                                const std::vector<std::string> &defines,
                                const std::vector<std::pair<std::string, std::string>> &virtual_includes,
                                const std::optional<std::filesystem::path> &cache_directory,
                                std::unordered_map<std::string, ShaderCompileResult> &memory_cache,
                                std::unordered_map<std::string, std::string> &source_graph_memory) {
    const auto start = std::chrono::steady_clock::now();
    bool warned = false;
    std::string key;
    std::filesystem::path cache_path;
    std::vector<std::filesystem::path> dependencies;
    if (cache_directory) {
        try {
            key = shaderCacheKey(source, stage, name, entry_point, include_dirs, defines, virtual_includes,
                                 source_graph_memory, &dependencies);
            cache_path = *cache_directory / (key + ".spv-cache");
            if (const auto found = memory_cache.find(key); found != memory_cache.end()) {
                auto output = found->second;
                output.cache_hit = true;
                output.log = "shader in-memory cache hit";
                output.dependencies = dependencies;
                if (auto *metrics = FastModuleContainer::tryGet<StartupMetrics>()) {
                    metrics->addShader(std::chrono::duration<double, std::milli>{
                                           std::chrono::steady_clock::now() - start}.count(), true);
                }
                return output;
            }
            const auto cached = readCache(cache_path, key);
            if (cached.spirv) {
                ShaderCompileResult output{.spirv = std::move(*cached.spirv),
                                           .log = "shader disk cache hit",
                                           .ok = true,
                                           .cache_hit = true,
                                           .cache_key = key,
                                           .dependencies = dependencies};
                memory_cache.emplace(key, output);
                if (auto *metrics = FastModuleContainer::tryGet<StartupMetrics>()) {
                    metrics->addShader(std::chrono::duration<double, std::milli>{
                                           std::chrono::steady_clock::now() - start}.count(), true);
                }
                return output;
            }
            if (cached.error) {
                warnCacheOnce(warned, cache_path, *cached.error);
            }
        } catch (const std::exception &ex) {
            warnCacheOnce(warned, *cache_directory, ex.what());
            key.clear();
        }
    } else {
        (void)sourceGraphSha256(source, name, include_dirs, virtual_includes,
                               source_graph_memory, &dependencies);
    }

    auto output = compileGlslUncached(source, stage, name, entry_point, include_dirs, defines, virtual_includes);
    output.cache_key = key;
    output.dependencies = std::move(dependencies);
    if (output.ok && cache_directory && !key.empty()) {
        if (const auto error = writeCache(*cache_directory, cache_path, key, output.spirv)) {
            warnCacheOnce(warned, cache_path, *error);
        }
        memory_cache.insert_or_assign(key, output);
    }
    if (auto *metrics = FastModuleContainer::tryGet<StartupMetrics>()) {
        metrics->addShader(std::chrono::duration<double, std::milli>{
                               std::chrono::steady_clock::now() - start}.count(), false);
    }
    return output;
}
#endif

} // namespace

ShaderCompiler::ShaderCompiler() {
    if (const auto *resolver = FastModuleContainer::tryGet<PathResolver>();
        resolver != nullptr && resolver->isSetup()) {
        cache_directory = resolver->projectRoot() / ".pelican" / "shader_cache";
    }
}

std::optional<vk::ShaderStageFlagBits> inferShaderStageFromPath(const std::filesystem::path &path) {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (ext == ".vert") {
        return vk::ShaderStageFlagBits::eVertex;
    }
    if (ext == ".frag") {
        return vk::ShaderStageFlagBits::eFragment;
    }
    if (ext == ".comp") {
        return vk::ShaderStageFlagBits::eCompute;
    }
    if (ext == ".rgen") {
        return vk::ShaderStageFlagBits::eRaygenKHR;
    }
    if (ext == ".rmiss") {
        return vk::ShaderStageFlagBits::eMissKHR;
    }
    if (ext == ".rchit") {
        return vk::ShaderStageFlagBits::eClosestHitKHR;
    }
    return std::nullopt;
}

ShaderCompileResult ShaderCompiler::compileFile(const std::filesystem::path &path, const ShaderCompileOptions &opts) {
    const auto stage = opts.stage.value_or(inferShaderStageFromPath(path).value_or(vk::ShaderStageFlagBits{}));
    if (stage == vk::ShaderStageFlagBits{}) {
        return {{}, "Unable to infer shader stage from path: " + path.string(), false};
    }

#if PELICAN_RUNTIME_SHADER_COMPILER
    auto compile_include_dirs = include_dirs;
    if (path.has_parent_path()) {
        const auto source_dir = normalizedPath(path.parent_path());
        compile_include_dirs.insert(compile_include_dirs.begin(), source_dir / "shaders" / "include");
        compile_include_dirs.insert(compile_include_dirs.begin(), source_dir);
    }
    auto result = compileGlsl(readTextFile(path), stage, normalizedPath(path).string(), opts.entry_point,
                              compile_include_dirs, opts.defines, opts.virtual_includes, cache_directory,
                              memory_cache, source_graph_memory);
    result.dependencies.push_back(normalizedPath(path));
    std::sort(result.dependencies.begin(), result.dependencies.end());
    result.dependencies.erase(std::unique(result.dependencies.begin(), result.dependencies.end()),
                              result.dependencies.end());
    return result;
#else
    (void)path;
    return {{}, "Runtime shader compiler is disabled", false};
#endif
}

ShaderCompileResult ShaderCompiler::compileSource(std::string_view source, vk::ShaderStageFlagBits stage,
                                                  std::string_view name, const ShaderCompileOptions &opts) {
#if PELICAN_RUNTIME_SHADER_COMPILER
    return compileGlsl(source, stage, name, opts.entry_point, include_dirs, opts.defines,
                        opts.virtual_includes, cache_directory, memory_cache, source_graph_memory);
#else
    (void)source;
    (void)stage;
    (void)name;
    (void)opts;
    return {{}, "Runtime shader compiler is disabled", false};
#endif
}

void ShaderCompiler::addIncludeDir(const std::filesystem::path &dir) { include_dirs.push_back(normalizedPath(dir)); }

void ShaderCompiler::setCacheDirectory(std::filesystem::path dir) {
    cache_directory = normalizedPath(dir);
}

} // namespace Pelican
