#include "../container.hpp"

#include <glm/glm.hpp>
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
    std::string scene_data_json;
    std::string asset_data_json;

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
};

} // namespace Pelican
