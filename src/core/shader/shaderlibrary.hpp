#pragma once

#include "shader.hpp"
#include "shadercompiler.hpp"
#include "shaderreference.hpp"
#include "shaderreflection.hpp"
#include "surfacecompiler.hpp"
#include "../container.hpp"
#include "../loader/pathresolver.hpp"
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
    std::vector<std::string> defines;
    uint64_t version = 1;
    std::string log;
    std::vector<SpvLinkBinding> binding_table;
    std::string cache_key;
};

struct SurfaceShaderBundleIds {
    ShaderBundleId vertex;
    ShaderBundleId fragment;
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
    mutable ShaderCompiler compiler;

    ShaderBundle buildFromFile(const std::filesystem::path &path, uint64_t version,
                               std::vector<std::string> defines = {}) const;
    ShaderBundle buildFromSpirv(std::span<const uint32_t> spirv, std::filesystem::path source_path,
                                uint64_t version, std::string log, std::vector<std::string> defines = {}) const;
    ShaderBundle buildFromEngineSource(std::string_view source, ShaderStage stage,
                                       std::string_view name, uint64_t version,
                                       std::vector<std::string> defines = {}) const;
    vk::UniqueShaderModule createShaderModule(std::span<const uint32_t> spirv) const;
    ShaderBundleId loadResolvedReference(const ResolvedRef &resolved, const ShaderReference &reference,
                                         std::string_view display_name,
                                         const std::vector<std::string> &defines);
    ShaderBundleId loadFromStemReference(const ShaderReference &reference, const PathResolver &resolver,
                                         const std::vector<std::string> &defines = {});
    void markDirty(ShaderBundleId id);

  public:
    explicit ShaderLibrary(ShaderLibraryModuleMode mode = ShaderLibraryModuleMode::create_modules);

    ShaderBundleId loadFromFile(const std::filesystem::path &path, std::vector<std::string> defines = {});
    ShaderBundleId loadFromReference(const ShaderReference &reference, const PathResolver &resolver,
                                     bool project_context, std::vector<std::string> defines = {});
    ShaderBundleId loadFromBytes(size_t len, const char *data, std::string_view name);
    ShaderBundleId loadFromSpirv(std::span<const uint32_t> spirv, std::string_view name);
    SurfaceShaderBundleIds loadFromSurface(const SurfaceFormatDocument &surface,
                                           std::string_view source_name,
                                           SurfacePass pass = SurfacePass::main,
                                           std::vector<std::string> defines = {});
    const ShaderBundle &get(ShaderBundleId id) const;

    bool reload(ShaderBundleId id);
    size_t reloadModifiedSources(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    std::vector<ShaderBundleId> takeDirtyBundles();
};

} // namespace Pelican
