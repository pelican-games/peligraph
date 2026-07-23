#pragma once

#include "shader.hpp"
#include "shadercompiler.hpp"
#include "shaderreference.hpp"
#include "shaderreflection.hpp"
#include "surfacecompiler.hpp"
#include "../container.hpp"
#include "../loader/pathresolver.hpp"
#include "../resourcecontainer.hpp"
#include "../watch/assetkey.hpp"
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

PELICAN_DEFINE_HANDLE(ShaderBundleId, int);

struct LoweredMaterial;

struct ShaderBundle {
    vk::UniqueShaderModule module;
    ShaderReflection reflection;
    std::filesystem::path source_path;
    std::vector<std::string> defines;
    uint64_t version = 1;
    std::string log;
    std::vector<SpvLinkBinding> binding_table;
    std::string cache_key;
    bool cache_hit = false;
    std::vector<std::filesystem::path> dependency_paths;
};

struct SurfaceShaderBundleIds {
    ShaderBundleId vertex;
    ShaderBundleId fragment;
};

struct PreparedShaderBundle {
    ShaderBundleId id;
    ShaderBundle replacement;
    std::size_t reload_unit = 0;
};

struct PreparedShaderReload {
    std::vector<PreparedShaderBundle> candidates;
    std::map<watch::AssetKey, SurfaceFormatDocument> surface_documents;
    std::vector<watch::AssetKey> changed_keys;
    std::size_t cache_hits = 0;
    std::size_t cache_misses = 0;

    bool empty() const noexcept { return candidates.empty(); }
    std::vector<ShaderBundleId> affectedBundleIds() const;
};

struct ShaderReloadTrackingStatus {
    std::size_t units = 0;
    std::size_t bundles = 0;
    std::size_t dependencies = 0;
};

enum class ShaderLibraryModuleMode {
    create_modules,
    reflection_only,
};

DECLARE_MODULE(ShaderLibrary) {
    friend class PipelineFactory;

    struct FileReloadRecipe {
        std::filesystem::path path;
        std::vector<std::string> defines;
    };
    struct SurfaceReloadRecipe {
        std::filesystem::path path;
        watch::AssetKey source;
        std::string source_name;
        SurfacePass pass = SurfacePass::main;
        std::vector<std::string> defines;
    };
    struct ReloadUnit {
        std::variant<FileReloadRecipe, SurfaceReloadRecipe> recipe;
        std::vector<ShaderBundleId> bundle_ids;
        watch::AssetKey primary_source;
        std::vector<watch::AssetKey> dependencies;
    };

    ResourceContainer<ShaderBundleId, ShaderBundle> bundles;
    std::vector<ShaderBundleId> bundle_ids;
    std::vector<ShaderBundleId> dirty_bundles;
    std::vector<ReloadUnit> reload_units;
    std::unordered_map<ShaderBundleId, std::size_t, ShaderBundleId::Hash> unit_by_bundle;
    std::map<watch::AssetKey, std::vector<std::size_t>> units_by_dependency;
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
    std::optional<watch::AssetKey> logicalKeyForPath(const std::filesystem::path &path) const;
    std::optional<std::pair<std::filesystem::path, watch::AssetKey>>
    resolveReloadableSurface(std::string_view source_name) const;
    std::vector<watch::AssetKey>
    logicalDependencies(const ShaderBundle &bundle,
                        std::optional<watch::AssetKey> primary = std::nullopt) const;
    void registerFileReloadUnit(ShaderBundleId id, const ShaderBundle &bundle,
                                std::vector<std::string> defines);
    void registerSurfaceReloadUnit(SurfaceShaderBundleIds ids,
                                   const ShaderBundle &vertex_bundle,
                                   const ShaderBundle &fragment_bundle,
                                   std::filesystem::path path,
                                   watch::AssetKey source,
                                   std::string source_name,
                                   SurfacePass pass,
                                   std::vector<std::string> defines);
    void rebuildReverseDependencies();
    PreparedShaderReload prepareUnits(const std::set<std::size_t> &units,
                                      std::vector<watch::AssetKey> changed_keys) const;
    void activatePrepared(PreparedShaderReload &prepared);
    void finalizePrepared(const PreparedShaderReload &prepared);
    void markDirty(ShaderBundleId id);

  public:
    struct RegistrationCheckpoint {
        std::size_t bundle_count = 0;
        std::size_t reload_unit_count = 0;
        std::unordered_map<ShaderBundleId, std::size_t,
                           ShaderBundleId::Hash>
            unit_by_bundle;
        std::map<watch::AssetKey, std::vector<std::size_t>>
            units_by_dependency;
        std::vector<ShaderBundleId> dirty_bundles;
    };

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
    // Selects the fragment-output ABI and generated model defines together,
    // preventing a routed material from accidentally pairing a forward shader
    // with a G-buffer pipeline (or vice versa).
    SurfaceShaderBundleIds loadFromSurfaceForMaterial(
        const SurfaceFormatDocument &surface, std::string_view source_name,
        const LoweredMaterial &material,
        std::vector<std::string> additional_defines = {});
    const ShaderBundle &get(ShaderBundleId id) const;

    bool reload(ShaderBundleId id);
    bool handlesReload(const watch::AssetKey &key) const;
    PreparedShaderReload prepareReload(std::span<const watch::AssetKey> changed_keys) const;
    PreparedShaderReload prepareReloadAll() const;
    void recordReloadFailure(std::span<const watch::AssetKey> changed_keys,
                             std::string_view error);
    ShaderReloadTrackingStatus reloadTrackingStatus() const noexcept;
    std::vector<ShaderBundleId> takeDirtyBundles();

    RegistrationCheckpoint checkpointRegistrations() const;
    void rollbackRegistrations(
        RegistrationCheckpoint &checkpoint);
    std::vector<ShaderBundleId>
    registrationsSince(
        const RegistrationCheckpoint &checkpoint) const;
    std::size_t registrationCount() const noexcept {
        return bundle_ids.size();
    }
};

} // namespace Pelican
