#include "launchcameraoverride.hpp"

#include "../container.hpp"
#include "../launchconfig.hpp"
#include "../renderer/camera.hpp"

#include <glm/glm.hpp>
#include <cmath>
#include <stdexcept>

namespace Pelican {

void applyLaunchCameraOverride() {
    const auto &launch_config = GET_MODULE(EngineLaunchConfig);
    if (!launch_config.camera_override) {
        return;
    }

    auto &camera = GET_MODULE(Camera);
    const auto &override = *launch_config.camera_override;
    const glm::vec3 position{override.position[0], override.position[1], override.position[2]};
    const glm::vec3 target{override.target[0], override.target[1], override.target[2]};
    const auto dir = target - position;
    if (glm::length(dir) <= 0.0f) {
        throw std::runtime_error("--camera position and target must not be identical");
    }

    // CC-1a: look at the target with the world's +Y up (+Z when looking
    // straight up or down), through the same basis a transform gives.
    const auto forward = glm::normalize(dir);
    const glm::vec3 up = std::abs(forward.y) > 0.999f
                             ? glm::vec3{0.0f, 0.0f, forward.y > 0.0f ? 1.0f : -1.0f}
                             : glm::vec3{0.0f, 1.0f, 0.0f};
    camera.setPos(position);
    camera.setRotation(cameraRotation(forward, up));
    camera.setNearFar(override.fov_y, 0.01f, 1000.0f);
}

} // namespace Pelican
