#include "../cameradefinition.hpp"
#include "../container.hpp"

#include <cstdint>
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <unordered_map>

namespace Pelican {

DECLARE_MODULE(ProjectBasicConfig) {
  public:
    struct window_size {
        int width, height;
    };
    struct InitialCameraProperty {
        glm::vec3 up;
        CameraProjectionSpec projection;
    };

  private:
    std::string window_title;
    window_size initial_window_size;
    bool initial_fullscr_state;
    float framerate_target;
    std::uint64_t deterministic_seed = 0;
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

    mutable std::optional<std::string> scene_data_json;
    mutable std::optional<std::string> asset_data_json;
    mutable std::optional<std::string> rendering_config_json;
    mutable std::optional<std::string> ui_config_json;
    mutable std::optional<std::string> input_actions_json;
    mutable std::unordered_map<std::string, std::string> input_profile_jsons;

  public:
    ProjectBasicConfig();

    std::string windowTitle() const;
    window_size initialWindowSize() const;
    bool initialFullScreenState() const;
    float framerateTarget() const;
    std::uint64_t seed() const;

    InitialCameraProperty initailCameraProperty() const;

    std::string defaultSceneId() const;
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
