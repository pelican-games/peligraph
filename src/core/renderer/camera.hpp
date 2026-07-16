#pragma once

#include "../cameradefinition.hpp"
#include "../container.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Pelican {

DECLARE_MODULE(Camera) {
  public:
    enum class SceneCameraControllerType {
        Orbit,
        Follow,
        Fly,
    };

    struct SceneCameraController {
        SceneCameraControllerType type = SceneCameraControllerType::Orbit;
        std::string target;
        glm::vec3 offset{0.0f, 0.0f, 0.0f};
        float distance = 0.0f;
        float yaw = 0.0f;
        float pitch = 0.0f;
        float damping = 0.0f;
        float speed = 0.0f;
        float sensitivity = 0.0f;
    };

    struct SceneCamera {
        std::string name;
        CameraProjectionSpec projection;
        CameraSpritePolicySpec sprite;
        glm::vec3 pos{0.0f, 0.0f, 0.0f};
        glm::vec3 dir{0.0f, 0.0f, 1.0f};
        glm::vec3 up{0.0f, 1.0f, 0.0f};
        std::optional<SceneCameraController> controller;
    };

  private:
    glm::vec3 pos;
    glm::vec3 dir;
    glm::vec3 up;
    float viewport_aspect = 1.0f;
    uint32_t viewport_width = 1;
    uint32_t viewport_height = 1;
    CameraProjectionSpec projection;
    CameraSpritePolicySpec sprite_policy;
    glm::mat4 projection_matrix;
    std::unordered_map<std::string, SceneCamera> scene_cameras;
    std::vector<std::string> controlled_scene_camera_order;
    std::string active_camera_name;
    bool active_scene_camera_locked = false;
    std::uint64_t discontinuity_revision = 0;

    void rebuildProjectionMatrix();
    void resetToConfigDefaults();
    void applySceneCamera(const SceneCamera &camera);

  public:
    Camera();

    void setPos(glm::vec3 new_pos);
    glm::vec3 getPos() const { return pos; }
    void setDir(glm::vec3 new_dir);
    glm::vec3 getDir() const { return dir; }
    void setUp(glm::vec3 new_up) { up = new_up; }
    glm::vec3 getUp() const { return up; }
    void setScreenSize(uint32_t width, uint32_t height);
    void setNearFar(float new_fov_y, float new_near, float new_far);
    bool hasSceneCamera(std::string_view name) const;
    void loadSceneCameras(std::string_view scene_id);
    void setActiveCamera(std::string_view name);
    const std::string &activeCameraName() const { return active_camera_name; }
    std::uint64_t discontinuityRevision() const { return discontinuity_revision; }
    const std::vector<std::string> &controlledSceneCameraNames() const { return controlled_scene_camera_order; }
    const SceneCameraController *sceneCameraController(std::string_view name) const;
    bool acceptsControllerPose(std::string_view name) const;
    void applyControllerPose(std::string_view name, glm::vec3 new_pos, glm::vec3 new_dir, glm::vec3 new_up);
    CameraProjectionSpec getProjectionSpec() const { return projection; }
    CameraSpritePolicySpec getSpritePolicy() const { return sprite_policy; }
    uint32_t viewportWidth() const { return viewport_width; }
    uint32_t viewportHeight() const { return viewport_height; }
    glm::mat4 getVPMatrix() const;
    glm::mat4 getProjectionMatrix() const { return projection_matrix; }
    glm::mat4 getViewMatrix() const { return glm::lookAt(pos, pos + dir, up); }
};

} // namespace Pelican
