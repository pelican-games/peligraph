#include "../cameradefinition.hpp"
#include "../container.hpp"
#include "authoringscenedocument.hpp"

#include <cstdint>
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <unordered_map>

namespace Pelican {

class ProjectBasicConfigProjectionTarget;

DECLARE_MODULE(ProjectBasicConfig) {
    friend class ProjectBasicConfigProjectionTarget;
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

    mutable std::optional<AuthoringSceneDocument> scene_document;
    mutable std::uint64_t next_scene_revision = 1;
    mutable std::uint64_t next_authoring_object_id = 1;
    mutable std::optional<std::string> asset_data_json;
    mutable std::optional<std::string> rendering_config_json;
    mutable std::optional<std::string> ui_config_json;
    mutable std::optional<std::string> input_actions_json;
    mutable std::unordered_map<std::string, std::string> input_profile_jsons;

    void publishSceneDocument(std::string_view scene_v1_bytes) const;

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
    void updateSceneDocument(std::string_view scene_v1_bytes);
    void invalidateSceneDocument() noexcept;
    std::string sceneDataJson() const;
    std::string assetDataJson() const;
    std::string renderingConfigJson() const;
    std::string defaultRenderingPass() const;
    std::string uiConfigJson() const;
    std::optional<std::string> inputActionsJson() const;
    std::unordered_map<std::string, std::string> inputProfileJsons() const;
    std::optional<std::string> defaultInputProfile() const;
    bool usesProjectSource() const { return project_source; }

};

} // namespace Pelican
