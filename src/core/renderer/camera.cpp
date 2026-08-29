#include "camera.hpp"
#include "../launchconfig.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/pathresolver.hpp"
#include "../loader/resolvedscene.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <any>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Pelican {

namespace {

float degreesToRadians(float degrees) {
    return degrees * static_cast<float>(3.14159265358979323846 / 180.0);
}

Camera::SceneCameraController projectSceneCameraController(
    const CameraControllerCodecData &controller,
    const std::string &object_name) {
    if (object_name.empty()) {
        throw std::runtime_error(
            "camera '<unnamed>' controller requires a named scene object");
    }
    Camera::SceneCameraController parsed;
    parsed.target = controller.target;
    parsed.offset = glm::vec3{controller.offset.x, controller.offset.y,
                              controller.offset.z};
    parsed.distance = controller.distance;
    parsed.yaw = controller.yaw;
    parsed.pitch = controller.pitch;
    parsed.damping = controller.damping;
    parsed.speed = controller.speed;
    parsed.sensitivity = controller.sensitivity;
    if (controller.type == CameraControllerCodecType::Orbit) {
        parsed.type = Camera::SceneCameraControllerType::Orbit;
    } else if (controller.type == CameraControllerCodecType::Follow) {
        parsed.type = Camera::SceneCameraControllerType::Follow;
    } else {
        parsed.type = Camera::SceneCameraControllerType::Fly;
    }
    return parsed;
}

const ResolvedComponent *findResolvedComponent(
    const ResolvedObject &object, std::string_view name) {
    const auto found = std::find_if(
        object.components.begin(), object.components.end(),
        [name](const auto &component) { return component.name == name; });
    return found == object.components.end() ? nullptr : &*found;
}

Camera::SceneCamera parseResolvedSceneCamera(
    const ResolvedObject &object, glm::vec3 fallback_up) {
    const auto *component = findResolvedComponent(object, "camera");
    if (component == nullptr || component->runtime_codec == nullptr) {
        throw std::logic_error("resolved camera has no runtime codec");
    }
    const CameraCodecData *resolved_data = nullptr;
    try {
        resolved_data = &std::any_cast<const CameraCodecData &>(
            component->requireRuntimeValue());
    } catch (const std::exception &error) {
        throw std::runtime_error(
            "Invalid camera on object '" +
            (object.name ? *object.name : std::string{"<unnamed>"}) +
            "': " + error.what());
    }
    const auto &data = *resolved_data;
    Camera::SceneCamera camera;
    camera.name = object.name.value_or(std::string{});
    if (!data.projection_specified || !data.sprite_specified || !data.yfov ||
        !data.znear || !data.zfar || !data.xmag || !data.ymag) {
        throw std::logic_error(
            "resolved camera is missing resolver-completed values");
    }
    camera.projection = CameraProjectionSpec{
        .kind = data.projection_kind,
        .yfov = *data.yfov,
        .znear = *data.znear,
        .zfar = *data.zfar,
        .aspect = data.aspect,
        .xmag = *data.xmag,
        .ymag = *data.ymag,
    };
    camera.sprite = CameraSpritePolicySpec{
        .pixel_perfect = data.pixel_perfect,
        .sort = data.sprite_sort,
    };
    camera.up = fallback_up;
    if (data.controller) {
        camera.controller = projectSceneCameraController(*data.controller,
                                                         camera.name);
    }

    if (const auto *transform = findResolvedComponent(object, "transform")) {
        if (transform->runtime_codec == nullptr) {
            throw std::logic_error("resolved transform has no runtime codec");
        }
        const auto &trs = std::any_cast<const TransformCodecData &>(
            transform->requireRuntimeValue());
        camera.pos = glm::vec3{trs.pos.x, trs.pos.y, trs.pos.z};
        if (trs.rotation_specified) {
            const auto rotation = glm::quat{trs.rotation.w, trs.rotation.x,
                                            trs.rotation.y, trs.rotation.z};
            const auto rotation_matrix = glm::mat3_cast(rotation);
            camera.dir = glm::normalize(rotation_matrix *
                                        glm::vec3{0.0f, 0.0f, 1.0f});
            camera.up = glm::normalize(rotation_matrix *
                                       glm::vec3{0.0f, 1.0f, 0.0f});
        }
    }
    return camera;
}

void rebuildPreparedProjection(Camera::PreparedSceneState &prepared) {
    if (prepared.projection.kind == CameraProjectionKind::Orthographic) {
        prepared.projection_matrix = glm::orthoRH_ZO(
            -prepared.projection.xmag, prepared.projection.xmag,
            -prepared.projection.ymag, prepared.projection.ymag,
            prepared.projection.znear, prepared.projection.zfar);
        return;
    }

    const auto aspect = prepared.projection.aspect.value_or(prepared.viewport_aspect);
    prepared.projection_matrix = glm::perspectiveRH_ZO(
        prepared.projection.yfov, aspect, prepared.projection.znear,
        prepared.projection.zfar);
}

std::string runtimeFreeCameraName(
    const std::unordered_map<std::string, Camera::SceneCamera> &scene_cameras) {
    constexpr std::string_view base_name = "__pelican_runtime_free_camera";
    std::string name{base_name};
    for (std::uint64_t suffix = 2; scene_cameras.contains(name); ++suffix) {
        name = std::string{base_name} + '_' + std::to_string(suffix);
    }
    return name;
}

void applyRuntimeFreeCameraOverlay(
    Camera::PreparedSceneState &prepared,
    const std::optional<Camera::SceneCamera> &scene_camera_basis) {
    const auto &launch = GET_MODULE(EngineLaunchConfig);
    if (!launch.free_camera) {
        return;
    }
    if (!std::isfinite(launch.free_camera->orbit_distance) ||
        launch.free_camera->orbit_distance <= 0.0f) {
        throw std::runtime_error("runtime free camera orbit distance must be positive and finite");
    }
    if (!std::isfinite(launch.free_camera->sensitivity) ||
        launch.free_camera->sensitivity <= 0.0f) {
        throw std::runtime_error("runtime free camera sensitivity must be positive and finite");
    }

    Camera::SceneCamera free_camera;
    free_camera.name = runtimeFreeCameraName(prepared.scene_cameras);
    free_camera.projection = prepared.projection;
    free_camera.sprite = prepared.sprite_policy;
    free_camera.pos = prepared.pos;
    free_camera.dir = prepared.dir;
    free_camera.up = prepared.up;
    if (scene_camera_basis) {
        free_camera.projection = scene_camera_basis->projection;
        free_camera.sprite = scene_camera_basis->sprite;
        free_camera.pos = scene_camera_basis->pos;
        free_camera.dir = scene_camera_basis->dir;
        free_camera.up = scene_camera_basis->up;
    }
    free_camera.controller = Camera::SceneCameraController{
        .type = Camera::SceneCameraControllerType::Orbit,
        .distance = launch.free_camera->orbit_distance,
        .sensitivity = launch.free_camera->sensitivity,
    };

    prepared.pos = free_camera.pos;
    prepared.dir = free_camera.dir;
    prepared.up = free_camera.up;
    prepared.projection = free_camera.projection;
    prepared.sprite_policy = free_camera.sprite;
    rebuildPreparedProjection(prepared);
    prepared.active_camera_name = free_camera.name;
    prepared.active_scene_camera_locked = true;
    prepared.controlled_scene_camera_order.push_back(free_camera.name);
    prepared.scene_cameras.emplace(free_camera.name, std::move(free_camera));
}

} // namespace

Camera::Camera() {
    resetToConfigDefaults();
    loadSceneCameras(GET_MODULE(ProjectBasicConfig).defaultSceneId());
}

void Camera::rebuildProjectionMatrix() {
    if (projection.kind == CameraProjectionKind::Orthographic) {
        projection_matrix =
            glm::orthoRH_ZO(-projection.xmag, projection.xmag,
                            -projection.ymag, projection.ymag,
                            projection.znear, projection.zfar);
        return;
    }

    const auto aspect = projection.aspect.value_or(viewport_aspect);
    projection_matrix =
        glm::perspectiveRH_ZO(projection.yfov, aspect, projection.znear, projection.zfar);
}

void Camera::resetToConfigDefaults() {
    const auto &config = GET_MODULE(ProjectBasicConfig);
    pos = {0.0f, 0.0f, 0.0f};
    dir = {1.0f, 0.0f, 0.0f};

    const auto props = config.initailCameraProperty();
    const auto screen = config.initialWindowSize();
    up = props.up;
    viewport_aspect = static_cast<float>(screen.width) / screen.height;
    viewport_width = static_cast<uint32_t>(screen.width);
    viewport_height = static_cast<uint32_t>(screen.height);
    projection = props.projection;
    sprite_policy = props.sprite;
    scene_cameras.clear();
    controlled_scene_camera_order.clear();
    active_camera_name.clear();
    active_scene_camera_locked = false;
    rebuildProjectionMatrix();
}

Camera::PreparedSceneState Camera::snapshotPrepared() const {
    return PreparedSceneState{
        .pos = pos,
        .dir = dir,
        .up = up,
        .viewport_aspect = viewport_aspect,
        .viewport_width = viewport_width,
        .viewport_height = viewport_height,
        .projection = projection,
        .sprite_policy = sprite_policy,
        .projection_matrix = projection_matrix,
        .scene_cameras = scene_cameras,
        .controlled_scene_camera_order = controlled_scene_camera_order,
        .active_camera_name = active_camera_name,
        .active_scene_camera_locked = active_scene_camera_locked,
        .discontinuity_revision = discontinuity_revision,
    };
}

Camera::PreparedSceneState
Camera::prepareSceneCameras(std::string_view scene_id) const {
    const auto &config = GET_MODULE(ProjectBasicConfig);
    const auto props = config.initailCameraProperty();
    const auto screen = config.initialWindowSize();
    PreparedSceneState prepared{
        .pos = {0.0f, 0.0f, 0.0f},
        .dir = {1.0f, 0.0f, 0.0f},
        .up = props.up,
        .viewport_aspect = static_cast<float>(screen.width) / screen.height,
        .viewport_width = static_cast<uint32_t>(screen.width),
        .viewport_height = static_cast<uint32_t>(screen.height),
        .projection = props.projection,
        .sprite_policy = props.sprite,
        .discontinuity_revision = discontinuity_revision + 1,
    };
    const auto rebuild = [&prepared] {
        if (prepared.projection.kind == CameraProjectionKind::Orthographic) {
            prepared.projection_matrix = glm::orthoRH_ZO(
                -prepared.projection.xmag, prepared.projection.xmag,
                -prepared.projection.ymag, prepared.projection.ymag,
                prepared.projection.znear, prepared.projection.zfar);
        } else {
            const auto aspect = prepared.projection.aspect.value_or(
                prepared.viewport_aspect);
            prepared.projection_matrix = glm::perspectiveRH_ZO(
                prepared.projection.yfov, aspect, prepared.projection.znear,
                prepared.projection.zfar);
        }
    };
    rebuild();
    if (!GET_MODULE(PathResolver).isSetup()) {
        applyRuntimeFreeCameraOverlay(prepared, std::nullopt);
        return prepared;
    }

    return prepareSceneCameras(scene_id, config.resolvedScene());
}

Camera::PreparedSceneState Camera::prepareSceneCameras(
    std::string_view scene_id, const ResolvedScene &resolved) const {
    const auto &config = GET_MODULE(ProjectBasicConfig);
    const auto props = config.initailCameraProperty();
    const auto screen = config.initialWindowSize();
    PreparedSceneState prepared{
        .pos = {0.0f, 0.0f, 0.0f},
        .dir = {1.0f, 0.0f, 0.0f},
        .up = props.up,
        .viewport_aspect = static_cast<float>(screen.width) / screen.height,
        .viewport_width = static_cast<uint32_t>(screen.width),
        .viewport_height = static_cast<uint32_t>(screen.height),
        .projection = props.projection,
        .sprite_policy = props.sprite,
        .discontinuity_revision = discontinuity_revision + 1,
    };
    const auto rebuild = [&prepared] {
        if (prepared.projection.kind == CameraProjectionKind::Orthographic) {
            prepared.projection_matrix = glm::orthoRH_ZO(
                -prepared.projection.xmag, prepared.projection.xmag,
                -prepared.projection.ymag, prepared.projection.ymag,
                prepared.projection.znear, prepared.projection.zfar);
        } else {
            const auto aspect = prepared.projection.aspect.value_or(
                prepared.viewport_aspect);
            prepared.projection_matrix = glm::perspectiveRH_ZO(
                prepared.projection.yfov, aspect, prepared.projection.znear,
                prepared.projection.zfar);
        }
    };
    rebuild();
    const auto *scene = resolved.findScene(scene_id);
    if (scene == nullptr) {
        applyRuntimeFreeCameraOverlay(prepared, std::nullopt);
        return prepared;
    }

    bool first_camera = true;
    std::optional<SceneCamera> first_scene_camera;
    for (const auto &object : scene->objects) {
        if (findResolvedComponent(object, "camera") == nullptr) {
            continue;
        }

        auto scene_camera = parseResolvedSceneCamera(object, prepared.up);
        if (first_camera) {
            first_scene_camera = scene_camera;
            prepared.projection = scene_camera.projection;
            prepared.sprite_policy = scene_camera.sprite;
            rebuild();
            prepared.active_scene_camera_locked = false;
            first_camera = false;
        }
        if (!scene_camera.name.empty()) {
            if (scene_camera.controller) {
                prepared.controlled_scene_camera_order.push_back(
                    scene_camera.name);
            }
            prepared.scene_cameras.insert_or_assign(scene_camera.name,
                                                     std::move(scene_camera));
        }
    }
    applyRuntimeFreeCameraOverlay(prepared, first_scene_camera);
    return prepared;
}

void Camera::publishPrepared(PreparedSceneState &&prepared) noexcept {
    pos = prepared.pos;
    dir = prepared.dir;
    up = prepared.up;
    viewport_aspect = prepared.viewport_aspect;
    viewport_width = prepared.viewport_width;
    viewport_height = prepared.viewport_height;
    projection = prepared.projection;
    sprite_policy = prepared.sprite_policy;
    projection_matrix = prepared.projection_matrix;
    scene_cameras.swap(prepared.scene_cameras);
    controlled_scene_camera_order.swap(
        prepared.controlled_scene_camera_order);
    active_camera_name.swap(prepared.active_camera_name);
    active_scene_camera_locked = prepared.active_scene_camera_locked;
    discontinuity_revision = prepared.discontinuity_revision;
}

void Camera::loadSceneCameras(std::string_view scene_id) {
    auto prepared = prepareSceneCameras(scene_id);
    publishPrepared(std::move(prepared));
}

void Camera::applySceneCamera(const SceneCamera &camera) {
    pos = camera.pos;
    dir = camera.dir;
    up = camera.up;
    projection = camera.projection;
    sprite_policy = camera.sprite;
    rebuildProjectionMatrix();
}

void Camera::setPos(glm::vec3 new_pos) {
    if (active_scene_camera_locked) {
        return;
    }
    pos = new_pos;
}

void Camera::setDir(glm::vec3 new_dir) {
    if (active_scene_camera_locked) {
        return;
    }
    dir = new_dir;
}

glm::mat4 Camera::getVPMatrix() const { return projection_matrix * glm::lookAt(pos, pos + dir, up); }

void Camera::setScreenSize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) throw std::runtime_error("camera viewport must be non-zero");
    viewport_width = width;
    viewport_height = height;
    viewport_aspect = static_cast<float>(width) / height;
    rebuildProjectionMatrix();
}
void Camera::setNearFar(float new_fov_y, float new_near, float new_far) {
    projection.kind = CameraProjectionKind::Perspective;
    projection.yfov = degreesToRadians(new_fov_y);
    projection.znear = new_near;
    projection.zfar = new_far;
    projection.aspect.reset();
    rebuildProjectionMatrix();
}

bool Camera::hasSceneCamera(std::string_view name) const {
    return scene_cameras.find(std::string{name}) != scene_cameras.end();
}

const Camera::SceneCameraController *Camera::sceneCameraController(std::string_view name) const {
    const auto camera_it = scene_cameras.find(std::string{name});
    if (camera_it == scene_cameras.end() || !camera_it->second.controller) {
        return nullptr;
    }
    return &*camera_it->second.controller;
}

bool Camera::acceptsControllerPose(std::string_view name) const {
    return active_camera_name.empty() || active_camera_name == name;
}

void Camera::applyControllerPose(std::string_view name, glm::vec3 new_pos, glm::vec3 new_dir, glm::vec3 new_up) {
    if (!acceptsControllerPose(name)) {
        return;
    }
    pos = new_pos;
    dir = glm::normalize(new_dir);
    up = glm::normalize(new_up);
}

void Camera::setActiveCamera(std::string_view name) {
    const auto camera_it = scene_cameras.find(std::string{name});
    if (camera_it == scene_cameras.end()) {
        throw std::runtime_error("unknown camera name: " + std::string{name});
    }

    applySceneCamera(camera_it->second);
    active_camera_name = camera_it->first;
    active_scene_camera_locked = true;
    ++discontinuity_revision;
}

} // namespace Pelican
