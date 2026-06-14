#pragma once

#include "../container.hpp"
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
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
};

std::optional<vk::ShaderStageFlagBits> inferShaderStageFromPath(const std::filesystem::path &path);

DECLARE_MODULE(ShaderCompiler) {
    std::vector<std::filesystem::path> include_dirs;

  public:
    ShaderCompileResult compileFile(const std::filesystem::path &path, const ShaderCompileOptions &opts = {});
    ShaderCompileResult compileSource(std::string_view source, vk::ShaderStageFlagBits stage, std::string_view name);
    void addIncludeDir(const std::filesystem::path &dir);
};

} // namespace Pelican
