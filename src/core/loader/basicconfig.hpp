#include "../container.hpp"

#include <glm/glm.hpp>
#include <optional>
#include <string>

namespace Pelican {

DECLARE_MODULE(ProjectBasicConfig) {
  public:
    struct window_size {
        int width, height;
    };
    struct InitialCameraProperty {
        glm::vec3 up;
        float fov_y, near, far;
    };

  private:
    std::string window_title;
    window_size initial_window_size;
    bool initial_fullscr_state;
    float framerate_target;
    InitialCameraProperty camera_prop;

    std::string default_scene_id;
    std::string scene_data_json_ref;
    std::string asset_data_json_ref;
    std::string rendering_config_json_ref;
    std::string default_rendering_pass;
    std::string ui_config_json_ref;

    mutable std::optional<std::string> scene_data_json;
    mutable std::optional<std::string> asset_data_json;
    mutable std::optional<std::string> rendering_config_json;
    mutable std::optional<std::string> ui_config_json;

  public:
    ProjectBasicConfig();

    std::string windowTitle() const;
    window_size initialWindowSize() const;
    bool initialFullScreenState() const;
    float framerateTarget() const;

    InitialCameraProperty initailCameraProperty() const;

    std::string defaultSceneId() const;
    std::string sceneDataJson() const;
    std::string assetDataJson() const;
    std::string renderingConfigJson() const;
    std::string defaultRenderingPass() const;
    std::string uiConfigJson() const;
};

} // namespace Pelican
