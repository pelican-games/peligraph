#pragma once

#include "../cameradefinition.hpp"
#include "../container.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Pelican {

DECLARE_MODULE(Camera) {
  public:
    struct SceneCamera {
        std::string name;
        CameraProjectionSpec projection;
        glm::vec3 pos{0.0f, 0.0f, 0.0f};
        glm::vec3 dir{0.0f, 0.0f, 1.0f};
        glm::vec3 up{0.0f, 1.0f, 0.0f};
    };

  private:
    glm::vec3 pos;
    glm::vec3 dir;
    glm::vec3 up;
    float viewport_aspect = 1.0f;
    CameraProjectionSpec projection;
    glm::mat4 projection_matrix;
    std::unordered_map<std::string, SceneCamera> scene_cameras;
    std::string active_camera_name;
    bool active_scene_camera_locked = false;

    void rebuildProjectionMatrix();
    void loadSceneCameras();
    void applySceneCamera(const SceneCamera &camera);

  public:
    Camera();

    void setPos(glm::vec3 new_pos);
    glm::vec3 getPos() const { return pos; }
    void setDir(glm::vec3 new_dir);
    void setUp(glm::vec3 new_up) { up = new_up; }
    void setScreenSize(uint32_t width, uint32_t height);
    void setNearFar(float new_fov_y, float new_near, float new_far);
    bool hasSceneCamera(std::string_view name) const;
    void setActiveCamera(std::string_view name);
    const std::string &activeCameraName() const { return active_camera_name; }
    CameraProjectionSpec getProjectionSpec() const { return projection; }
    glm::mat4 getVPMatrix() const;
    glm::mat4 getProjectionMatrix() const { return projection_matrix; }
    glm::mat4 getViewMatrix() const { return glm::lookAt(pos, pos + dir, up); }
};

} // namespace Pelican
