#pragma once

#include "shader.hpp"
#include "shaderreflection.hpp"
#include "../container.hpp"
#include "../resourcecontainer.hpp"
#include <chrono>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

PELICAN_DEFINE_HANDLE(ShaderBundleId, int);

struct ShaderBundle {
    vk::UniqueShaderModule module;
    ShaderReflection reflection;
    std::filesystem::path source_path;
    uint64_t version = 1;
    std::string log;
};

enum class ShaderLibraryModuleMode {
    create_modules,
    reflection_only,
};

DECLARE_MODULE(ShaderLibrary) {
    ResourceContainer<ShaderBundleId, ShaderBundle> bundles;
    std::vector<ShaderBundleId> bundle_ids;
    std::vector<ShaderBundleId> dirty_bundles;
    std::unordered_map<ShaderBundleId, std::filesystem::file_time_type, ShaderBundleId::Hash> source_write_times;
    std::chrono::steady_clock::time_point next_source_poll_time{};
    ShaderLibraryModuleMode module_mode = ShaderLibraryModuleMode::create_modules;

    ShaderBundle buildFromFile(const std::filesystem::path &path, uint64_t version) const;
    ShaderBundle buildFromSpirv(std::span<const uint32_t> spirv, std::filesystem::path source_path,
                                uint64_t version, std::string log) const;
    vk::UniqueShaderModule createShaderModule(std::span<const uint32_t> spirv) const;
    void markDirty(ShaderBundleId id);

  public:
    explicit ShaderLibrary(ShaderLibraryModuleMode mode = ShaderLibraryModuleMode::create_modules);

    ShaderBundleId loadFromFile(const std::filesystem::path &path);
    ShaderBundleId loadFromBytes(size_t len, const char *data, std::string_view name);
    ShaderBundleId loadFromSpirv(std::span<const uint32_t> spirv, std::string_view name);
    const ShaderBundle &get(ShaderBundleId id) const;

    bool reload(ShaderBundleId id);
    size_t reloadModifiedSources(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    std::vector<ShaderBundleId> takeDirtyBundles();
};

} // namespace Pelican
