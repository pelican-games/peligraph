#pragma once

#include "../cameradefinition.hpp"
#include "../container.hpp"
#include "clipspace.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json_fwd.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Pelican {
struct SceneLoadPhaseTrace;

class ResolvedScene;

DECLARE_MODULE(Camera) {
  public:
    enum class SceneCameraControllerType {
        Orbit,
        Follow,
        Fly,
    };

    struct SceneCameraController {
        SceneCameraControllerType type = SceneCameraControllerType::Orbit;
        std::string target; // Empty selects a runtime-only Orbit pivot; Follow still requires a name.
        glm::vec3 offset{0.0f, 0.0f, 0.0f};
        float distance = 0.0f;
        float yaw = 0.0f;
        float pitch = 0.0f;
        float damping = 0.0f;
        float speed = 0.0f;
        float sensitivity = 0.0f;
    };

    // CC-1a: dir and up are the camera's basis (clipspace.hpp cameraBasis):
    // dir = R * (0, 0, -1), up = R * (0, 1, 0). Without a rotation the camera
    // looks down -Z with +Y up.
    struct SceneCamera {
        std::string name;
        CameraProjectionSpec projection;
        CameraSpritePolicySpec sprite;
        glm::vec3 pos{0.0f, 0.0f, 0.0f};
        glm::vec3 dir{0.0f, 0.0f, -1.0f};
        glm::vec3 up{0.0f, 1.0f, 0.0f};
        std::optional<SceneCameraController> controller;
    };

    // PM-1b (docs/design_play_mode.md section 4.4). Which camera the frame is
    // drawn from when the launch made an editor camera (the runtime free
    // camera of --free-camera or --start-state edit): `editor` is that
    // camera, locked active; `game` is the camera the scene and game code
    // chose, as it would be without an editor camera. The view not shown is
    // parked with its pose and its active camera, and comes back as it was.
    // Without an editor camera the view is always `game`.
    enum class ViewCamera : std::uint8_t {
        editor,
        game,
    };

    struct ParkedView {
        bool valid = false;
        glm::vec3 pos{0.0f, 0.0f, 0.0f};
        glm::vec3 dir{0.0f, 0.0f, -1.0f};
        glm::vec3 up{0.0f, 1.0f, 0.0f};
        CameraProjectionSpec projection;
        CameraSpritePolicySpec sprite_policy;
        std::string active_camera_name;
        bool active_scene_camera_locked = false;
    };

    struct PreparedSceneState {
        glm::vec3 pos{0.0f, 0.0f, 0.0f};
        glm::vec3 dir{0.0f, 0.0f, -1.0f};
        glm::vec3 up{0.0f, 1.0f, 0.0f};
        float viewport_aspect = 1.0f;
        uint32_t viewport_width = 1;
        uint32_t viewport_height = 1;
        CameraProjectionSpec projection;
        CameraSpritePolicySpec sprite_policy;
        glm::mat4 projection_matrix{1.0f};
        std::unordered_map<std::string, SceneCamera> scene_cameras;
        std::vector<std::string> controlled_scene_camera_order;
        std::string active_camera_name;
        bool active_scene_camera_locked = false;
        std::uint64_t discontinuity_revision = 0;
        // PM-1b: the editor camera's name (empty without one), and the view
        // the prepared state does not show.
        std::string editor_camera_name;
        ParkedView parked_view;
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
    // PM-1b: see ViewCamera. The view is the engine's, not the scene's: a
    // scene load keeps it.
    std::string editor_camera_name;
    ParkedView parked_view;
    ViewCamera view_camera = ViewCamera::editor;

    void rebuildProjectionMatrix();
    ParkedView shownView() const;
    void showView(const ParkedView &view);
    // The project-config defaults both prepareSceneCameras overloads start
    // from, with the projection matrix already rebuilt.
    PreparedSceneState makeDefaultPreparedSceneState() const;
    void resetToConfigDefaults();
    void applySceneCamera(const SceneCamera &camera);

  public:
    Camera();

    void setPos(glm::vec3 new_pos);
    glm::vec3 getPos() const { return pos; }
    void setDir(glm::vec3 new_dir);
    glm::vec3 getDir() const { return dir; }
    // CC-1a: dir and up from a transform's rotation (cameraBasis). Ignored
    // while a scene camera is locked active, like setDir.
    void setRotation(const glm::quat &rotation);
    void setUp(glm::vec3 new_up) { up = new_up; }
    glm::vec3 getUp() const { return up; }
    void setScreenSize(uint32_t width, uint32_t height);
    void setNearFar(float new_fov_y, float new_near, float new_far);
    bool hasSceneCamera(std::string_view name) const;
    PreparedSceneState snapshotPrepared() const;
    PreparedSceneState prepareSceneCameras(std::string_view scene_id) const;
    PreparedSceneState prepareSceneCameras(
        std::string_view scene_id, const ResolvedScene &scene) const;
    void publishPrepared(PreparedSceneState &&prepared) noexcept;
    void publishPrepared(PreparedSceneState &&prepared,
                         SceneLoadPhaseTrace &phase_trace) noexcept;
    void loadSceneCameras(std::string_view scene_id);
    // Makes `name` the active camera, locked. Naming a scene camera while the
    // editor camera is shown switches to the game view first (the editor
    // camera is parked), so the choice is the game's; naming the editor
    // camera shows the editor view.
    void setActiveCamera(std::string_view name);
    const std::string &activeCameraName() const { return active_camera_name; }
    // PM-1b.
    const std::string &editorCameraName() const { return editor_camera_name; }
    ViewCamera viewCamera() const noexcept {
        return editor_camera_name.empty() ? ViewCamera::game : view_camera;
    }
    // Shows the other view and parks this one. Asking for the editor view
    // with no editor camera throws std::logic_error; the same view again, or
    // the game view without an editor camera, changes nothing.
    void setViewCamera(ViewCamera view);
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
    glm::mat4 getViewMatrix() const { return viewFromBasis(pos, dir, up); }

    // GP-4: the world ray through a framebuffer pixel position, by the inverse
    // of the projection and the view this camera draws the frame with. Pixel
    // (0, 0) is the viewport's top-left corner and +y is down, the way the
    // flat frame maps NDC to the framebuffer (gizmo.cpp projectPoint). The
    // origin lies on the near plane; the direction is normalized.
    struct PixelRay {
        glm::vec3 origin{0.0f};
        glm::vec3 direction{0.0f, 0.0f, -1.0f};
    };
    PixelRay rayThroughPixel(glm::vec2 position_px) const;
};

} // namespace Pelican
