#pragma once

#include "../container.hpp"
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct ShaderCompileResult {
    std::vector<uint32_t> spirv;
    std::string log;
    bool ok = false;
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

  public:
    ShaderCompileResult compileFile(const std::filesystem::path &path, const ShaderCompileOptions &opts = {});
    ShaderCompileResult compileSource(std::string_view source, vk::ShaderStageFlagBits stage,
                                      std::string_view name, const ShaderCompileOptions &opts = {});
    void addIncludeDir(const std::filesystem::path &dir);
};

} // namespace Pelican
