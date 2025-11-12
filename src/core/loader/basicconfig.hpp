#include "../container.hpp"

#include <glm/glm.hpp>
#include <string>

namespace Pelican {

class ProjectBasicConfig {

  public:
    ProjectBasicConfig(DependencyContainer &container);

    std::string windowTitle() const;
    struct window_size {
        int width, height;
    };
    window_size initialWindowSize() const;
    bool initialFullScreenState() const;
    float framerateTarget() const;

    struct InitialCameraProperty {
        glm::vec3 up;
        float fov_y, near, far;
    };
    InitialCameraProperty initailCameraProperty() const;
};

} // namespace Pelican
