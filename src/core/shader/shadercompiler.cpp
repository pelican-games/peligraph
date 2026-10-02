#include "shadercompiler.hpp"
#include "../loader/engineresources.hpp"
#include "../loader/pathresolver.hpp"
#include "../startup.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
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
constexpr std::string_view shaderTargetEnvironment = PELICAN_SHADER_TARGET_CACHE_ENV_NAME;
#define PELICAN_SHADER_CACHE_STRINGIFY_IMPL(value) #value
#define PELICAN_SHADER_CACHE_STRINGIFY(value) PELICAN_SHADER_CACHE_STRINGIFY_IMPL(value)
// The options compileGlslUncached gives shaderc, as part of the key: engine
// builds with other options must not answer from one shared cache directory.
// Change it together with those options.
constexpr std::string_view shaderCompileOptions =
    "target-env-version=" PELICAN_SHADER_CACHE_STRINGIFY(PELICAN_SHADERC_TARGET_ENV_VERSION)
    ";optimization=none;debug-info=none";
std::atomic_bool shaderCacheWarningEmitted = false;

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
    case vk::ShaderStageFlagBits::eAnyHitKHR:
        return shaderc_anyhit_shader;
    case vk::ShaderStageFlagBits::eIntersectionKHR:
        return shaderc_intersection_shader;
    case vk::ShaderStageFlagBits::eCallableKHR:
        return shaderc_callable_shader;
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

using FileReader = std::function<std::string(const std::filesystem::path &)>;

std::optional<std::pair<std::string, std::string>> resolveInclude(
    std::string_view requested_source, shaderc_include_type type, std::string_view requesting_source,
    const std::vector<std::filesystem::path> &include_dirs, const VirtualIncludes &virtual_includes,
    const FileReader &read_file = readTextFile) {
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
        return std::pair{normalizedPath(candidate).string(), read_file(candidate)};
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
                extension != ".rchit" && extension != ".rahit") {
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

// TS-1: a graph without include roots is a function of its root, its virtual
// sources and the embedded engine sources, which are fixed for the process.
// Every compiler of the process (each engine, each test project) reuses it.
std::mutex &processSourceGraphMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, std::string> &processSourceGraphs() {
    static std::unordered_map<std::string, std::string> graphs;
    return graphs;
}

// D-1 (TS-1): a file the key reads is read again only when its size or write
// time changed. A file written in the last two seconds is not kept: a second
// write of the same size inside one write-time tick would look unchanged.
std::string readKeyFile(const std::filesystem::path &path, ShaderKeyMemory &memory) {
    const auto id = path.lexically_normal().string();
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    std::filesystem::file_time_type write_time{};
    if (!ec) write_time = std::filesystem::last_write_time(path, ec);
    if (!ec) {
        if (const auto found = memory.files.find(id); found != memory.files.end() &&
            found->second.size == size && found->second.write_time == write_time) {
            return found->second.content;
        }
    }
    auto content = readTextFile(path);
    ++memory.file_reads;
    if (!ec && content.size() == size &&
        write_time < std::filesystem::file_time_type::clock::now() - std::chrono::seconds{2}) {
        memory.files.insert_or_assign(id, ShaderKeyFileMemo{size, write_time, content});
    } else {
        memory.files.erase(id);
    }
    return content;
}

// B-2 (TS-1): the key names a physical file by the first include root that
// holds it ("<root index>/<path below the root>"), so one project's shader
// and the same bytes at the same place in another project share a key. A
// file outside every root keeps its absolute path.
std::string keyFileName(const std::filesystem::path &file,
                        const std::vector<std::filesystem::path> &include_dirs) {
    const auto normal = file.lexically_normal();
    for (std::size_t index = 0; index < include_dirs.size(); ++index) {
        const auto relative = normal.lexically_relative(include_dirs[index].lexically_normal());
        if (relative.empty() || relative.is_absolute() ||
            (relative.begin() != relative.end() && *relative.begin() == "..")) {
            continue;
        }
        return std::to_string(index) + "/" + relative.generic_string();
    }
    return normal.string();
}

std::string sourceGraphSha256(std::string_view source, std::string_view name,
                              const std::vector<std::filesystem::path> &include_dirs,
                              const std::vector<std::pair<std::string, std::string>> &virtual_sources,
                              ShaderKeyMemory &key_memory,
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
        std::lock_guard lock{processSourceGraphMutex()};
        if (const auto found = processSourceGraphs().find(stable_input);
            found != processSourceGraphs().end()) {
            return found->second;
        }
    }
    const FileReader read_key_file = [&](const std::filesystem::path &path) {
        return readKeyFile(path, key_memory);
    };
    // A name the graph records: physical files by their place below the
    // roots, virtual and engine sources by their own names.
    const auto graph_name = [&](const std::string &source_name) {
        if (include_dirs.empty() || source_name.starts_with("engine://")) return source_name;
        const std::filesystem::path path{source_name};
        return path.is_absolute() ? keyFileName(path, include_dirs) : source_name;
    };
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
        appendKeyPart(graph, "source-name",
                      virtual_includes.contains(source_name) ? source_name : graph_name(source_name));
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
                                                 include_dirs, virtual_includes, read_key_file);
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
    // Each root lists its own files by their path below it, in root order, so
    // the key holds what a search of the roots in that order can find.
    appendKeyPart(graph, "include-root-count", std::to_string(include_dirs.size()));
    for (std::size_t index = 0; index < include_dirs.size(); ++index) {
        const auto &root = include_dirs[index];
        std::vector<std::pair<std::string, std::filesystem::path>> potential_files;
        std::error_code ec;
        if (std::filesystem::is_regular_file(root, ec) && !ec) {
            potential_files.emplace_back(".", normalizedPath(root));
        } else {
            ec.clear();
            for (std::filesystem::recursive_directory_iterator it{
                     root, std::filesystem::directory_options::skip_permission_denied, ec}, end;
                 !ec && it != end; it.increment(ec)) {
                if (it->is_regular_file(ec) && !ec) {
                    potential_files.emplace_back(
                        it->path().lexically_relative(root).generic_string(),
                        normalizedPath(it->path()));
                }
                ec.clear();
            }
        }
        std::sort(potential_files.begin(), potential_files.end());
        for (const auto &[relative, path] : potential_files) {
            appendKeyPart(graph, "potential-file-name", std::to_string(index) + "/" + relative);
            appendKeyPart(graph, "potential-file-source", read_key_file(path));
            if (dependencies != nullptr) dependencies->push_back(path);
        }
    }
    appendKeyPart(graph, "potential-engine-universe-sha256", engineShaderUniverseSha256());
    if (dependencies != nullptr) {
        std::sort(dependencies->begin(), dependencies->end());
        dependencies->erase(std::unique(dependencies->begin(), dependencies->end()),
                            dependencies->end());
    }
    auto result = picosha2::hash256_hex_string(graph);
    if (!stable_input.empty() && !has_file_dependency) {
        std::lock_guard lock{processSourceGraphMutex()};
        processSourceGraphs().emplace(std::move(stable_input), result);
    }
    return result;
}

std::string shaderCacheKey(std::string_view source, vk::ShaderStageFlagBits stage,
                           std::string_view name, std::string_view entry_point,
                           const std::vector<std::filesystem::path> &include_dirs,
                           const std::vector<std::string> &defines,
                           const std::vector<std::pair<std::string, std::string>> &virtual_includes,
                           ShaderKeyMemory &key_memory,
                           std::vector<std::filesystem::path> *dependencies) {
    unsigned int spirv_version = 0;
    unsigned int spirv_revision = 0;
    shaderc_get_spv_version(&spirv_version, &spirv_revision);

    std::string raw;
    appendKeyPart(raw, "source-sha256",
                  sourceGraphSha256(source, name, include_dirs, virtual_includes,
                                    key_memory, dependencies));
    for (const auto &define : defines) {
        appendKeyPart(raw, "define", define);
    }
    appendKeyPart(raw, "shaderc-version", PELICAN_SHADERC_VERSION);
    appendKeyPart(raw, "shaderc-spirv-version", std::to_string(spirv_version));
    appendKeyPart(raw, "shaderc-spirv-revision", std::to_string(spirv_revision));
    appendKeyPart(raw, "target-env", shaderTargetEnvironment);
    appendKeyPart(raw, "compile-options", shaderCompileOptions);
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
    // Keep the temporary entry beside the destination so rename remains atomic,
    // but do not append to its already-long SHA-256 filename. The shortened
    // sibling name is portable and remains shorter than the final cache entry,
    // avoiding the Windows MAX_PATH failure without platform-specific paths.
    const auto temporary = directory / (".tmp-" + std::string{key.substr(0, 8)} + "-" + nonce);
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

void warnCacheOnce(const std::filesystem::path &path, std::string_view reason) {
    if (logger == nullptr || shaderCacheWarningEmitted.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    LOG_WARNING(logger,
                "shader disk cache is unavailable; recompiling without it (path: {}, reason: {}). "
                "Further cache failures will not be reported in this process",
                path.string(), reason);
}

ShaderCompileResult compileGlslUncached(std::string_view source, vk::ShaderStageFlagBits stage,
                                       std::string_view name, std::string_view entry_point,
                                       const std::vector<std::filesystem::path> &include_dirs,
                                       const std::vector<std::string> &defines,
                                       const std::vector<std::pair<std::string, std::string>> &virtual_includes) {
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, PELICAN_SHADERC_TARGET_ENV_VERSION);
    options.SetIncluder(std::make_unique<FileIncluder>(include_dirs, virtual_includes));
    for (const auto &define : defines) {
        const auto separator = define.find('=');
        if (separator == std::string::npos) {
            options.AddMacroDefinition(define);
        } else {
            options.AddMacroDefinition(define.substr(0, separator), define.substr(separator + 1));
        }
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
                                ShaderKeyMemory &key_memory,
                                bool record_metrics) {
    const auto start = std::chrono::steady_clock::now();
    std::string key;
    std::filesystem::path cache_path;
    std::vector<std::filesystem::path> dependencies;
    if (cache_directory) {
        try {
            key = shaderCacheKey(source, stage, name, entry_point, include_dirs, defines, virtual_includes,
                                 key_memory, &dependencies);
            cache_path = *cache_directory / (key + ".spv-cache");
            if (const auto found = memory_cache.find(key); found != memory_cache.end()) {
                auto output = found->second;
                output.cache_hit = true;
                output.log = "shader in-memory cache hit";
                output.dependencies = dependencies;
                if (auto *metrics = record_metrics ? FastModuleContainer::tryGet<StartupMetrics>() : nullptr) {
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
                if (auto *metrics = record_metrics ? FastModuleContainer::tryGet<StartupMetrics>() : nullptr) {
                    metrics->addShader(std::chrono::duration<double, std::milli>{
                                           std::chrono::steady_clock::now() - start}.count(), true);
                }
                return output;
            }
            if (cached.error) {
                warnCacheOnce(cache_path, *cached.error);
            }
        } catch (const std::exception &ex) {
            warnCacheOnce(*cache_directory, ex.what());
            key.clear();
        }
    } else {
        (void)sourceGraphSha256(source, name, include_dirs, virtual_includes,
                               key_memory, &dependencies);
    }

    auto output = compileGlslUncached(source, stage, name, entry_point, include_dirs, defines, virtual_includes);
    output.cache_key = key;
    output.dependencies = std::move(dependencies);
    if (output.ok && cache_directory && !key.empty()) {
        if (const auto error = writeCache(*cache_directory, cache_path, key, output.spirv)) {
            warnCacheOnce(cache_path, *error);
        }
        memory_cache.insert_or_assign(key, output);
    }
    if (auto *metrics = record_metrics ? FastModuleContainer::tryGet<StartupMetrics>() : nullptr) {
        metrics->addShader(std::chrono::duration<double, std::milli>{
                               std::chrono::steady_clock::now() - start}.count(), false);
    }
    return output;
}
#endif

std::optional<std::filesystem::path> sharedShaderCacheDirectory() {
#ifdef _WIN32
    wchar_t *raw_value = nullptr;
    std::size_t value_size = 0;
    const auto error = _wdupenv_s(&raw_value, &value_size, L"PELICAN_SHADER_CACHE_DIR");
    std::unique_ptr<wchar_t, decltype(&std::free)> value{raw_value, &std::free};
    if (error != 0 || value == nullptr || value.get()[0] == L'\0') return std::nullopt;
    return std::filesystem::path{value.get()};
#else
    const auto *value = std::getenv(shaderCacheDirectoryEnvironment);
    if (value == nullptr || value[0] == '\0') return std::nullopt;
    return std::filesystem::path{value};
#endif
}

} // namespace

ShaderCompiler::ShaderCompiler(DetachedTag) : records_startup_metrics{false} {}

ShaderCompiler ShaderCompiler::detached() { return ShaderCompiler{DetachedTag{}}; }

ShaderCompiler::ShaderCompiler() {
    if (const auto *resolver = FastModuleContainer::tryGet<PathResolver>();
        resolver != nullptr && resolver->isSetup()) {
        cache_directory = resolver->projectRoot() / ".pelican" / "shader_cache";
        // TS-1: the variable moves the cache, it does not give one to a
        // compiler without a project.
        if (const auto shared = sharedShaderCacheDirectory()) {
            cache_directory = normalizedPath(*shared);
        }
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
    if (ext == ".rahit") {
        return vk::ShaderStageFlagBits::eAnyHitKHR;
    }
    if (ext == ".rcall") {
        return vk::ShaderStageFlagBits::eCallableKHR;
    }
    if (ext == ".rint") {
        return vk::ShaderStageFlagBits::eIntersectionKHR;
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
                              memory_cache, key_memory, records_startup_metrics);
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
                        opts.virtual_includes, cache_directory, memory_cache, key_memory,
                        records_startup_metrics);
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
