#pragma once

#include "../cameradefinition.hpp"
#include "../container.hpp"
#include "sceneload.hpp"

#include <cstdint>
#include <cstddef>
#include <functional>
#include <glm/glm.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Pelican {

class ProjectBasicConfigProjectionTarget;
class PathResolver;
// Defined in project/renderpipeline.hpp.
enum class MaterialRouteDefault;

enum class SceneSaveErrorCode : std::uint8_t {
    ExternalModification,
    Unavailable,
    IoFailure,
};

class SceneSaveError : public std::runtime_error {
    SceneSaveErrorCode code_;

  public:
    SceneSaveError(SceneSaveErrorCode code, const std::string &message)
        : std::runtime_error{message}, code_{code} {}
    SceneSaveErrorCode code() const noexcept { return code_; }
};

enum class SceneSaveFaultPoint : std::uint8_t {
    AfterEncode,
    AfterDiskDigest,
    AfterTemporaryWrite,
    AfterTemporaryValidation,
    AfterCachePrepare,
    BeforeReplace,
};

enum class SceneImportFaultPoint : std::uint8_t {
    AfterCandidatePrepare,
    AfterPublication,
};

struct SceneSaveResult {
    SceneRevision scene_revision{};
    std::string digest;
    std::size_t byte_count = 0;
};

struct ScenePrefabBaselineEntry {
    std::string name;
    std::string path;
    std::string digest_sha256;
};

struct ScenePersistenceBaseline {
    std::string scene_digest_sha256;
    std::vector<ScenePrefabBaselineEntry> prefab_read_set;
};

DECLARE_MODULE(ProjectBasicConfig) {
    friend class ProjectBasicConfigProjectionTarget;
    friend class MetadataCommitToken;
    friend class SceneLoader;
  public:
    struct window_size {
        int width, height;
    };
    struct InitialCameraProperty {
        glm::vec3 up;
        CameraProjectionSpec projection;
        CameraSpritePolicySpec sprite;
    };

  private:
    std::string window_title;
    window_size initial_window_size;
    bool initial_fullscr_state;
    float framerate_target;
    std::uint64_t deterministic_seed = 0;
    float sprite_pixels_per_unit = 100.0f;
    InitialCameraProperty camera_prop;

    std::string default_scene_id;
    std::string scene_data_json_ref;
    std::string asset_data_json_ref;
    std::string rendering_config_json_ref;
    std::string default_rendering_pass;
    std::string ui_config_json_ref;
    std::optional<std::string> input_actions_json_ref;
    std::unordered_map<std::string, std::string> input_profile_json_refs;
    std::optional<std::string> default_input_profile;
    bool project_source = false;
    PrefabRegistrySnapshot prefab_registry;
    struct SceneMetadataState {
        std::optional<SceneProjectionState> projection;
        std::optional<ScenePersistenceBaseline> persistence_baseline;
        BindableProviderSnapshot prefab_provider;
        std::uint64_t next_prefab_provider_generation = 1;
        std::uint64_t next_scene_revision = 1;
        std::uint64_t next_scene_resolver_generation = 1;
        std::uint64_t next_authoring_object_id = 1;
        SceneId current_scene_id;
        std::uint64_t epoch = 0;
        SceneAuthority active_authority{};

        void swap(SceneMetadataState &other) noexcept;
    };

    mutable SceneMetadataState scene_metadata;
    std::optional<SceneSaveFaultPoint> scene_save_fault;
    std::function<void(SceneSaveFaultPoint)> scene_save_hook;
    std::optional<SceneImportFaultPoint> scene_import_fault;
    mutable std::optional<std::string> asset_data_json;
    mutable std::optional<std::string> rendering_config_json;
    // G-4: read from the rendering document once per engine start. A
    // published edit does not change it; the engine restarts to apply one.
    mutable std::optional<MaterialRouteDefault> material_route_default;
    mutable std::optional<std::string> ui_config_json;
    mutable std::optional<std::string> input_actions_json;
    mutable std::unordered_map<std::string, std::string> input_profile_jsons;

    void publishSceneDocument(std::string_view scene_v1_bytes) const;
    void publishPreparedSceneState(SceneProjectionState &state) const noexcept;
    MetadataCommitToken prepareImportedSceneMetadata(
        SceneProjectionState candidate,
        ScenePersistenceBaseline persistence_baseline,
        BindableProviderSnapshot candidate_provider,
        bool publish_provider) const;
    MetadataCommitToken prepareBorrowedSceneMetadata(
        const SceneProjectionState &committed) const;

  public:
    ProjectBasicConfig();

    std::string windowTitle() const;
    window_size initialWindowSize() const;
    bool initialFullScreenState() const;
    float framerateTarget() const;
    std::uint64_t seed() const;
    float spritePixelsPerUnit() const;

    InitialCameraProperty initailCameraProperty() const;

    std::string defaultSceneId() const;
    const AuthoringSceneDocument &sceneDocument() const;
    const ResolvedScene &resolvedScene() const;
    const SceneProjectionState &sceneProjectionState() const;
    void updateSceneDocument(std::string_view scene_v1_bytes);
    void invalidateSceneDocument() noexcept;
    SceneImportResult importSceneDocument(std::string_view scene_bytes,
                                          const SceneImportCallback &reload);
    void refreshResolvedScene(const std::function<void()> &reload);
    // PM-2 (docs/design_play_mode.md 6.3). The metadata that puts a play
    // session's saved authoring back: its content and AuthoringObjectIds as
    // the next SceneRevision (the current one + 1, never an earlier number),
    // with the next AuthoringObjectId the larger of the saved and the current
    // one. Loading it (SceneLoadRequest::owned) commits it with the scene the
    // request names and a fresh authority, whose epoch closes the editor gate
    // on anything accepted before. Nothing is published until that load
    // commits.
    MetadataCommitToken prepareRestoredSceneMetadata(
        const AuthoringSceneDocument &saved) const;
    SceneSaveResult saveSceneDocument();
    void setSceneSaveFaultForTesting(
        std::optional<SceneSaveFaultPoint> fault) noexcept {
        scene_save_fault = fault;
    }
    void setSceneSaveHookForTesting(
        std::function<void(SceneSaveFaultPoint)> hook) {
        scene_save_hook = std::move(hook);
    }
    void setSceneImportFaultForTesting(
        std::optional<SceneImportFaultPoint> fault) noexcept {
        scene_import_fault = fault;
    }
    std::string sceneDataJson() const;
    std::string assetDataJson() const;
    std::string assetDataReference() const;
    std::string prepareAssetDataJson(std::string_view raw_json, PathResolver &resolver) const;
    void publishAssetDataJson(std::string prepared_json) noexcept;
    std::string renderingConfigJson() const;
    std::string renderingConfigReference() const;
    void publishRenderingConfigJson(
        std::string prepared_json) noexcept;
    // G-4: the rendering document's `material_route_default`, read once at
    // engine start by the renderer (snapshotMaterialRouteDefault, which
    // refuses a bad value by name). Before that, or without a renderer, it is
    // `auto`. A published edit never changes it; the engine restarts.
    void snapshotMaterialRouteDefault() const;
    MaterialRouteDefault materialRouteDefault() const;
    std::string defaultRenderingPass() const;
    std::string uiConfigJson() const;
    const std::string &uiConfigReference() const noexcept { return ui_config_json_ref; }
    std::optional<std::string> inputActionsJson() const;
    std::unordered_map<std::string, std::string> inputProfileJsons() const;
    std::optional<std::string> defaultInputProfile() const;
    bool usesProjectSource() const { return project_source; }

    const SceneId &currentSceneId() const noexcept {
        return scene_metadata.current_scene_id;
    }
    std::uint64_t runtimeSceneEpoch() const noexcept {
        return scene_metadata.epoch;
    }
    SceneAuthority activeSceneAuthority() const noexcept {
        return scene_metadata.active_authority;
    }

};

} // namespace Pelican
