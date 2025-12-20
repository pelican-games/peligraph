#pragma once

#include "../container.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace Pelican {

DECLARE_MODULE(Camera) {
    glm::vec3 pos;
    glm::vec3 dir;
    glm::vec3 up;
    float aspect;
    float fov_y;
    float near, far;
    glm::mat4 perspective_matrix;

  public:
    Camera();

    void setPos(glm::vec3 new_pos) { pos = new_pos; }
    glm::vec3 getPos() const { return pos; }
    void setDir(glm::vec3 new_dir) { dir = new_dir; }
    void setUp(glm::vec3 new_up) { up = new_up; }
    void setScreenSize(uint32_t width, uint32_t height);
    void setNearFar(float new_fov_y, float new_near, float new_far);
    glm::mat4 getVPMatrix() const;
    glm::mat4 getProjectionMatrix() const { return perspective_matrix; }
    glm::mat4 getViewMatrix() const { return glm::lookAt(pos, pos + dir, up); }
};

} // namespace Pelican
