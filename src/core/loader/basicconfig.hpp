#include "../container.hpp"

#include <glm/glm.hpp>
#include <string>

namespace Pelican {

class ProjectBasicConfig {
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

  public:
    ProjectBasicConfig(DependencyContainer &container);

    std::string windowTitle() const;
    window_size initialWindowSize() const;
    bool initialFullScreenState() const;
    float framerateTarget() const;

    InitialCameraProperty initailCameraProperty() const;
};

} // namespace Pelican
