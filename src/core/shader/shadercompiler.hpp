#pragma once

#include "../container.hpp"
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct ShaderCompileResult {
    std::vector<uint32_t> spirv;
    std::string log;
    bool ok = false;
    bool cache_hit = false;
    std::string cache_key;
    // Canonical physical files that participated in the source graph. The
    // root file is included by compileFile(); embedded and virtual sources
    // deliberately stay out of this list.
    std::vector<std::filesystem::path> dependencies;
};

struct ShaderCompileOptions {
    std::optional<vk::ShaderStageFlagBits> stage;
    std::string entry_point = "main";
    std::vector<std::string> defines;
    // In-memory sources participate in the normal shaderc include path. This
    // is used by B-layer surface compilation so authored code is reverse
    // included by an engine-owned template without rewriting the document.
    std::vector<std::pair<std::string, std::string>> virtual_includes;
};

std::optional<vk::ShaderStageFlagBits> inferShaderStageFromPath(const std::filesystem::path &path);

DECLARE_MODULE(ShaderCompiler) {
    std::vector<std::filesystem::path> include_dirs;
    std::optional<std::filesystem::path> cache_directory;
    std::unordered_map<std::string, ShaderCompileResult> memory_cache;
    std::unordered_map<std::string, std::string> source_graph_memory;
    bool records_startup_metrics = true;

    struct DetachedTag {};
    explicit ShaderCompiler(DetachedTag);

  public:
    ShaderCompiler();
    // EL-4c: a compiler that reads no module and writes nothing outside
    // itself: it looks up no project shader cache (so it has none) and adds
    // nothing to StartupMetrics. A worker thread owns one while the engine
    // compiles on its own thread.
    static ShaderCompiler detached();
    ShaderCompileResult compileFile(const std::filesystem::path &path, const ShaderCompileOptions &opts = {});
    ShaderCompileResult compileSource(std::string_view source, vk::ShaderStageFlagBits stage,
                                      std::string_view name, const ShaderCompileOptions &opts = {});
    void addIncludeDir(const std::filesystem::path &dir);
    void setCacheDirectory(std::filesystem::path dir);
};

} // namespace Pelican
