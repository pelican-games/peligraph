#include "camera.hpp"
#include "../launchconfig.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/pathresolver.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

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

float checkedNumber(const nlohmann::json &value, const std::string &field, const std::string &camera_name) {
    if (!value.is_number()) {
        throw std::runtime_error("camera '" + camera_name + "' field '" + field + "' must be numeric");
    }
    return value.get<float>();
}

struct JsonCameraParam {
    nlohmann::json value;
    std::string field;
};

std::optional<JsonCameraParam> findCameraParam(const std::vector<const nlohmann::json *> &roots,
                                               std::string_view field) {
    for (const auto *root : roots) {
        if (root == nullptr || !root->is_object()) {
            continue;
        }
        if (const auto it = root->find(field); it != root->end()) {
            return JsonCameraParam{*it, std::string{field}};
        }
    }
    return std::nullopt;
}

float requireCameraParam(const std::vector<const nlohmann::json *> &roots, std::string_view primary,
                         const std::string &camera_name) {
    const auto param = findCameraParam(roots, primary);
    if (!param) {
        throw std::runtime_error("camera '" + camera_name + "' requires numeric field '" +
                                 std::string{primary} + "'");
    }
    return checkedNumber(param->value, param->field, camera_name);
}

std::optional<float> optionalCameraParam(const std::vector<const nlohmann::json *> &roots,
                                         std::string_view primary, const std::string &camera_name) {
    const auto param = findCameraParam(roots, primary);
    if (!param) {
        return std::nullopt;
    }

    return checkedNumber(param->value, param->field, camera_name);
}

void rejectDeprecatedCameraFields(const std::vector<const nlohmann::json *> &roots,
                                  const std::string &camera_name) {
    struct DeprecatedField {
        std::string_view name;
        std::string_view replacement;
        std::string_view suffix;
    };
    static constexpr DeprecatedField deprecated_fields[] = {
        {"fov_y", "yfov", " (radians)"},
        {"near", "znear", ""},
        {"far", "zfar", ""},
    };
    for (const auto &field : deprecated_fields) {
        for (const auto *root : roots) {
            if (root != nullptr && root->is_object() && root->contains(field.name)) {
                throw std::runtime_error("camera '" + camera_name + "' field '" +
                                         std::string{field.name} + "' is not supported in v1; use '" +
                                         std::string{field.replacement} + "'" + std::string{field.suffix});
            }
        }
    }
}

const nlohmann::json *optionalObject(const nlohmann::json &json, const char *name, const std::string &camera_name) {
    const auto it = json.find(name);
    if (it == json.end()) {
        return nullptr;
    }
    if (!it->is_object()) {
        throw std::runtime_error("camera '" + camera_name + "' field '" + name + "' must be an object");
    }
    return &*it;
}

std::string checkedString(const nlohmann::json &value, const std::string &field, const std::string &camera_name) {
    if (!value.is_string()) {
        throw std::runtime_error("camera '" + camera_name + "' field '" + field + "' must be a string");
    }
    return value.get<std::string>();
}

float requireControllerNumber(const nlohmann::json &controller, const char *field,
                              const std::string &camera_name) {
    const auto it = controller.find(field);
    if (it == controller.end()) {
        throw std::runtime_error("camera '" + camera_name + "' controller requires numeric field '" +
                                 std::string{field} + "'");
    }
    return checkedNumber(*it, std::string{"controller."} + field, camera_name);
}

std::optional<float> optionalControllerNumber(const nlohmann::json &controller, const char *field,
                                              const std::string &camera_name) {
    const auto it = controller.find(field);
    if (it == controller.end()) {
        return std::nullopt;
    }
    return checkedNumber(*it, std::string{"controller."} + field, camera_name);
}

float optionalControllerAngle(const nlohmann::json &controller, const char *field, const char *degrees_field,
                              const std::string &camera_name, float fallback) {
    if (const auto radians = optionalControllerNumber(controller, field, camera_name)) {
        return *radians;
    }
    if (const auto degrees = optionalControllerNumber(controller, degrees_field, camera_name)) {
        return degreesToRadians(*degrees);
    }
    return fallback;
}

float optionalControllerNonNegative(const nlohmann::json &controller, const char *field,
                                    const std::string &camera_name, float fallback) {
    const auto value = optionalControllerNumber(controller, field, camera_name).value_or(fallback);
    if (value < 0.0f) {
        throw std::runtime_error("camera '" + camera_name + "' controller field '" + field +
                                 "' must be non-negative");
    }
    return value;
}

float requireControllerPositive(const nlohmann::json &controller, const char *field,
                                const std::string &camera_name) {
    const auto value = requireControllerNumber(controller, field, camera_name);
    if (value <= 0.0f) {
        throw std::runtime_error("camera '" + camera_name + "' controller field '" + field +
                                 "' must be positive");
    }
    return value;
}

std::string requireControllerString(const nlohmann::json &controller, const char *field,
                                    const std::string &camera_name) {
    const auto it = controller.find(field);
    if (it == controller.end()) {
        throw std::runtime_error("camera '" + camera_name + "' controller requires string field '" +
                                 std::string{field} + "'");
    }
    return checkedString(*it, std::string{"controller."} + field, camera_name);
}

std::string requireControllerNonEmptyString(const nlohmann::json &controller, const char *field,
                                            const std::string &camera_name) {
    auto value = requireControllerString(controller, field, camera_name);
    if (value.empty()) {
        throw std::runtime_error("camera '" + camera_name + "' controller field '" + field +
                                 "' must not be empty");
    }
    return value;
}

glm::vec3 readControllerVec3(const nlohmann::json &array, const std::string &field,
                             const std::string &camera_name) {
    if (!array.is_array() || array.size() != 3) {
        throw std::runtime_error("camera '" + camera_name + "' controller field '" + field +
                                 "' must be a vec3 array");
    }
    return glm::vec3{
        checkedNumber(array.at(0), std::string{"controller."} + field, camera_name),
        checkedNumber(array.at(1), std::string{"controller."} + field, camera_name),
        checkedNumber(array.at(2), std::string{"controller."} + field, camera_name),
    };
}

glm::vec3 requireControllerVec3(const nlohmann::json &controller, const char *field,
                                const std::string &camera_name) {
    const auto it = controller.find(field);
    if (it == controller.end()) {
        throw std::runtime_error("camera '" + camera_name + "' controller requires vec3 field '" +
                                 std::string{field} + "'");
    }
    return readControllerVec3(*it, field, camera_name);
}

bool hasAnyCameraProjectionParam(const nlohmann::json &camera_component) {
    static constexpr const char *fields[] = {
        "type", "perspective", "orthographic", "yfov", "fov_y", "znear", "near",
        "zfar", "far", "aspect", "xmag", "ymag",
    };
    for (const auto *field : fields) {
        if (camera_component.contains(field)) {
            return true;
        }
    }
    return false;
}

std::string cameraDisplayName(const std::string &name) {
    return name.empty() ? std::string{"<unnamed>"} : name;
}

CameraProjectionSpec parseSceneCameraProjection(const nlohmann::json &camera_component,
                                                const CameraProjectionSpec &fallback,
                                                const std::string &object_name) {
    const auto camera_name = cameraDisplayName(object_name);
    if (!hasAnyCameraProjectionParam(camera_component)) {
        return fallback;
    }

    const auto *perspective = optionalObject(camera_component, "perspective", camera_name);
    const auto *orthographic = optionalObject(camera_component, "orthographic", camera_name);
    rejectDeprecatedCameraFields({perspective, orthographic, &camera_component}, camera_name);

    std::string type = camera_component.value("type", std::string{});
    if (type.empty()) {
        type = orthographic != nullptr || camera_component.contains("xmag") || camera_component.contains("ymag")
                   ? "orthographic"
                   : "perspective";
    }
    if (type != "perspective" && type != "orthographic") {
        throw std::runtime_error("camera '" + camera_name + "' type must be 'perspective' or 'orthographic'");
    }

    CameraProjectionSpec projection = fallback;
    if (type == "orthographic") {
        projection.kind = CameraProjectionKind::Orthographic;
        const std::vector<const nlohmann::json *> roots{orthographic, &camera_component};
        if (auto xmag = optionalCameraParam(roots, "xmag", camera_name)) {
            projection.xmag = *xmag;
        } else if (fallback.kind != CameraProjectionKind::Orthographic) {
            projection.xmag = requireCameraParam(roots, "xmag", camera_name);
        }
        if (auto ymag = optionalCameraParam(roots, "ymag", camera_name)) {
            projection.ymag = *ymag;
        } else if (fallback.kind != CameraProjectionKind::Orthographic) {
            projection.ymag = requireCameraParam(roots, "ymag", camera_name);
        }
        if (auto znear = optionalCameraParam(roots, "znear", camera_name)) {
            projection.znear = *znear;
        }
        if (auto zfar = optionalCameraParam(roots, "zfar", camera_name)) {
            projection.zfar = *zfar;
        }
        projection.aspect.reset();
        return projection;
    }

    projection.kind = CameraProjectionKind::Perspective;
    const std::vector<const nlohmann::json *> roots{perspective, &camera_component};
    if (auto yfov = optionalCameraParam(roots, "yfov", camera_name)) {
        projection.yfov = *yfov;
    } else if (fallback.kind != CameraProjectionKind::Perspective) {
        projection.yfov = requireCameraParam(roots, "yfov", camera_name);
    }
    if (auto znear = optionalCameraParam(roots, "znear", camera_name)) {
        projection.znear = *znear;
    }
    if (auto zfar = optionalCameraParam(roots, "zfar", camera_name)) {
        projection.zfar = *zfar;
    }
    projection.aspect = optionalCameraParam(roots, "aspect", camera_name);
    return projection;
}

CameraSpritePolicySpec parseSceneCameraSpritePolicy(const nlohmann::json &camera_component,
                                                    CameraSpritePolicySpec fallback,
                                                    const std::string &object_name) {
    const auto found = camera_component.find("sprite");
    if (found == camera_component.end()) return fallback;
    const auto camera_name = cameraDisplayName(object_name);
    if (!found->is_object()) {
        throw std::runtime_error("camera '" + camera_name + "' field 'sprite' must be an object");
    }
    for (const auto &[field, value] : found->items()) {
        (void)value;
        if (field != "pixel_perfect" && field != "sort") {
            throw std::runtime_error("camera '" + camera_name +
                                     "' sprite policy has unknown field '" + field + "'");
        }
    }
    if (const auto value = found->find("pixel_perfect"); value != found->end()) {
        if (!value->is_string()) {
            throw std::runtime_error("camera '" + camera_name +
                                     "' sprite.pixel_perfect must be a string");
        }
        const auto name = value->get<std::string>();
        if (name == "off") fallback.pixel_perfect = CameraPixelPerfectMode::off;
        else if (name == "strict") fallback.pixel_perfect = CameraPixelPerfectMode::strict;
        else {
            throw std::runtime_error("camera '" + camera_name +
                                     "' sprite.pixel_perfect must be 'off' or 'strict'");
        }
    }
    if (const auto value = found->find("sort"); value != found->end()) {
        if (!value->is_string()) {
            throw std::runtime_error("camera '" + camera_name + "' sprite.sort must be a string");
        }
        const auto name = value->get<std::string>();
        if (name == "z") fallback.sort = CameraSpriteSortPolicy::z;
        else if (name == "y_down") fallback.sort = CameraSpriteSortPolicy::y_down;
        else if (name == "declaration") fallback.sort = CameraSpriteSortPolicy::declaration;
        else {
            throw std::runtime_error("camera '" + camera_name +
                                     "' sprite.sort must be 'z', 'y_down', or 'declaration'");
        }
    }
    return fallback;
}

const nlohmann::json *findControllerObject(const nlohmann::json &camera_component,
                                           const std::string &camera_name) {
    const nlohmann::json *controller = nullptr;
    if (const auto it = camera_component.find("controller"); it != camera_component.end()) {
        if (!it->is_object()) {
            throw std::runtime_error("camera '" + camera_name + "' field 'controller' must be an object");
        }
        controller = &*it;
    }

    const auto params_it = camera_component.find("params");
    if (params_it != camera_component.end() && params_it->is_object()) {
        const auto nested_it = params_it->find("controller");
        if (nested_it != params_it->end()) {
            if (!nested_it->is_object()) {
                throw std::runtime_error("camera '" + camera_name + "' field 'params.controller' must be an object");
            }
            if (controller != nullptr) {
                throw std::runtime_error("camera '" + camera_name +
                                         "' controller must be declared only once");
            }
            controller = &*nested_it;
        }
    }

    return controller;
}

Camera::SceneCameraController parseSceneCameraController(const nlohmann::json &controller,
                                                         const std::string &object_name) {
    const auto camera_name = cameraDisplayName(object_name);
    if (object_name.empty()) {
        throw std::runtime_error("camera '" + camera_name + "' controller requires a named scene object");
    }

    const auto type = requireControllerString(controller, "type", camera_name);
    Camera::SceneCameraController parsed;
    parsed.damping = optionalControllerNonNegative(controller, "damping", camera_name, 0.0f);

    if (type == "orbit") {
        parsed.type = Camera::SceneCameraControllerType::Orbit;
        if (controller.contains("target"))
            parsed.target = requireControllerNonEmptyString(controller, "target", camera_name);
        parsed.distance = requireControllerPositive(controller, "distance", camera_name);
        parsed.yaw = optionalControllerAngle(controller, "yaw", "yaw_degrees", camera_name, 0.0f);
        parsed.pitch = optionalControllerAngle(controller, "pitch", "pitch_degrees", camera_name, 0.0f);
        parsed.sensitivity = optionalControllerNonNegative(controller, "sensitivity", camera_name, 1.0f);
        return parsed;
    }
    if (type == "follow") {
        parsed.type = Camera::SceneCameraControllerType::Follow;
        parsed.target = requireControllerNonEmptyString(controller, "target", camera_name);
        parsed.offset = requireControllerVec3(controller, "offset", camera_name);
        return parsed;
    }

    if (type == "fly") {
        parsed.type = Camera::SceneCameraControllerType::Fly;
        parsed.speed = requireControllerPositive(controller, "speed", camera_name);
        parsed.sensitivity = requireControllerPositive(controller, "sensitivity", camera_name);
        return parsed;
    }

    throw std::runtime_error("camera '" + camera_name + "' controller type '" + type +
                             "' is not supported");
}

std::optional<Camera::SceneCameraController> parseOptionalSceneCameraController(
    const nlohmann::json &camera_component, const std::string &object_name) {
    const auto camera_name = cameraDisplayName(object_name);
    const auto *controller = findControllerObject(camera_component, camera_name);
    if (controller == nullptr) {
        return std::nullopt;
    }
    return parseSceneCameraController(*controller, object_name);
}

const nlohmann::json *findComponent(const nlohmann::json &object, std::string_view name) {
    if (!object.contains("components") || !object.at("components").is_array()) {
        return nullptr;
    }
    for (const auto &component : object.at("components")) {
        if (component.is_object() && component.value("name", std::string{}) == name) {
            return &component;
        }
    }
    return nullptr;
}

glm::vec3 readVec3(const nlohmann::json &array, const std::string &field, const std::string &object_name) {
    if (!array.is_array() || array.size() != 3) {
        throw std::runtime_error("transform field '" + field + "' on object '" + cameraDisplayName(object_name) +
                                 "' must be a vec3 array");
    }
    return glm::vec3{
        checkedNumber(array.at(0), field, cameraDisplayName(object_name)),
        checkedNumber(array.at(1), field, cameraDisplayName(object_name)),
        checkedNumber(array.at(2), field, cameraDisplayName(object_name)),
    };
}

glm::quat readQuat(const nlohmann::json &array, const std::string &field, const std::string &object_name) {
    if (!array.is_array() || array.size() != 4) {
        throw std::runtime_error("transform field '" + field + "' on object '" + cameraDisplayName(object_name) +
                                 "' must be a quat array");
    }
    const auto x = checkedNumber(array.at(0), field, cameraDisplayName(object_name));
    const auto y = checkedNumber(array.at(1), field, cameraDisplayName(object_name));
    const auto z = checkedNumber(array.at(2), field, cameraDisplayName(object_name));
    const auto w = checkedNumber(array.at(3), field, cameraDisplayName(object_name));
    return glm::quat{w, x, y, z};
}

Camera::SceneCamera parseSceneCamera(const nlohmann::json &object, const nlohmann::json &camera_component,
                                     const CameraProjectionSpec &fallback_projection,
                                     CameraSpritePolicySpec fallback_sprite,
                                     glm::vec3 fallback_up) {
    Camera::SceneCamera camera;
    camera.name = object.value("name", std::string{});
    camera.projection = parseSceneCameraProjection(camera_component, fallback_projection, camera.name);
    camera.sprite = parseSceneCameraSpritePolicy(camera_component, fallback_sprite, camera.name);
    camera.controller = parseOptionalSceneCameraController(camera_component, camera.name);
    camera.up = fallback_up;

    if (const auto *transform = findComponent(object, "transform")) {
        if (const auto pos_it = transform->find("pos"); pos_it != transform->end()) {
            camera.pos = readVec3(*pos_it, "pos", camera.name);
        }
        if (const auto rotation_it = transform->find("rotation"); rotation_it != transform->end()) {
            const auto rotation = readQuat(*rotation_it, "rotation", camera.name);
            const auto rotation_matrix = glm::mat3_cast(rotation);
            camera.dir = glm::normalize(rotation_matrix * glm::vec3{0.0f, 0.0f, 1.0f});
            camera.up = glm::normalize(rotation_matrix * glm::vec3{0.0f, 1.0f, 0.0f});
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
    if (!std::isfinite(launch.free_camera->speed) || launch.free_camera->speed <= 0.0f) {
        throw std::runtime_error("runtime free camera speed must be positive and finite");
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
        .type = Camera::SceneCameraControllerType::Fly,
        .speed = launch.free_camera->speed,
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

    const auto &scenes = config.sceneDocument().scenesJson();
    return prepareSceneCameras(scene_id, scenes);
}

Camera::PreparedSceneState Camera::prepareSceneCameras(
    std::string_view scene_id, const nlohmann::json &scenes) const {
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
    const auto scene_it = scenes.find(std::string{scene_id});
    if (scene_it == scenes.end()) {
        applyRuntimeFreeCameraOverlay(prepared, std::nullopt);
        return prepared;
    }

    bool first_camera = true;
    std::optional<SceneCamera> first_scene_camera;
    for (const auto &object : scene_it.value().at("objects")) {
        const auto *camera_component = findComponent(object, "camera");
        if (camera_component == nullptr) {
            continue;
        }

        auto scene_camera = parseSceneCamera(
            object, *camera_component, prepared.projection,
            prepared.sprite_policy, prepared.up);
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
