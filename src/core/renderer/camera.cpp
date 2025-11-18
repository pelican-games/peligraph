#include "camera.hpp"
#include "../loader/basicconfig.hpp"
#include <glm/gtc/matrix_transform.hpp>

namespace Pelican {

Camera::Camera() {
    const auto &config = GET_MODULE(ProjectBasicConfig);
    pos = {0.0f, 0.0f, 0.0f};
    dir = {1.0f, 0.0f, 0.0f};

    const auto props = config.initailCameraProperty();
    const auto screen = config.initialWindowSize();
    up = props.up;
    aspect = static_cast<float>(screen.width) / screen.height;
    fov_y = props.fov_y;
    near = props.near;
    far = props.far;
    perspective_matrix = glm::perspective(glm::radians(fov_y), aspect, near, far);
}

glm::mat4 Camera::getVPMatrix() const { return perspective_matrix * glm::lookAt(pos, pos + dir, up); }

void Camera::setScreenSize(uint32_t width, uint32_t height) {
    aspect = static_cast<float>(width) / height;
    perspective_matrix = glm::perspective(glm::radians(fov_y), aspect, near, far);
}
void Camera::setNearFar(float new_fov_y, float new_near, float new_far) {
    fov_y = new_fov_y;
    near = new_near;
    far = new_far;
    perspective_matrix = glm::perspective(glm::radians(fov_y), aspect, near, far);
}

} // namespace Pelican
