#include "camera.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/pathresolver.hpp"
#include "../../project/sceneformat.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

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
    bool alias = false;
};

std::optional<JsonCameraParam> findCameraParam(const std::vector<const nlohmann::json *> &roots,
                                               std::string_view primary, std::string_view alias = {}) {
    for (const auto *root : roots) {
        if (root == nullptr || !root->is_object()) {
            continue;
        }
        if (const auto it = root->find(primary); it != root->end()) {
            return JsonCameraParam{*it, std::string{primary}, false};
        }
        if (!alias.empty()) {
            if (const auto it = root->find(alias); it != root->end()) {
                return JsonCameraParam{*it, std::string{alias}, true};
            }
        }
    }
    return std::nullopt;
}

float requireCameraParam(const std::vector<const nlohmann::json *> &roots, std::string_view primary,
                         const std::string &camera_name, std::string_view alias = {},
                         bool alias_is_degrees = false) {
    const auto param = findCameraParam(roots, primary, alias);
    if (!param) {
        auto message = "camera '" + camera_name + "' requires numeric field '" + std::string{primary} + "'";
        if (!alias.empty()) {
            message += " (alias '" + std::string{alias} + "')";
        }
        throw std::runtime_error(message);
    }

    auto value = checkedNumber(param->value, param->field, camera_name);
    if (param->alias && alias_is_degrees) {
        value = degreesToRadians(value);
    }
    return value;
}

std::optional<float> optionalCameraParam(const std::vector<const nlohmann::json *> &roots,
                                         std::string_view primary, const std::string &camera_name,
                                         std::string_view alias = {}, bool alias_is_degrees = false) {
    const auto param = findCameraParam(roots, primary, alias);
    if (!param) {
        return std::nullopt;
    }

    auto value = checkedNumber(param->value, param->field, camera_name);
    if (param->alias && alias_is_degrees) {
        value = degreesToRadians(value);
    }
    return value;
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
        if (auto znear = optionalCameraParam(roots, "znear", camera_name, "near")) {
            projection.znear = *znear;
        }
        if (auto zfar = optionalCameraParam(roots, "zfar", camera_name, "far")) {
            projection.zfar = *zfar;
        }
        projection.aspect.reset();
        return projection;
    }

    projection.kind = CameraProjectionKind::Perspective;
    const std::vector<const nlohmann::json *> roots{perspective, &camera_component};
    if (auto yfov = optionalCameraParam(roots, "yfov", camera_name, "fov_y", true)) {
        projection.yfov = *yfov;
    } else if (fallback.kind != CameraProjectionKind::Perspective) {
        projection.yfov = requireCameraParam(roots, "yfov", camera_name, "fov_y", true);
    }
    if (auto znear = optionalCameraParam(roots, "znear", camera_name, "near")) {
        projection.znear = *znear;
    }
    if (auto zfar = optionalCameraParam(roots, "zfar", camera_name, "far")) {
        projection.zfar = *zfar;
    }
    projection.aspect = optionalCameraParam(roots, "aspect", camera_name);
    return projection;
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
                                     glm::vec3 fallback_up) {
    Camera::SceneCamera camera;
    camera.name = object.value("name", std::string{});
    camera.projection = parseSceneCameraProjection(camera_component, fallback_projection, camera.name);
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

} // namespace

Camera::Camera() {
    const auto &config = GET_MODULE(ProjectBasicConfig);
    pos = {0.0f, 0.0f, 0.0f};
    dir = {1.0f, 0.0f, 0.0f};

    const auto props = config.initailCameraProperty();
    const auto screen = config.initialWindowSize();
    up = props.up;
    viewport_aspect = static_cast<float>(screen.width) / screen.height;
    projection = props.projection;
    rebuildProjectionMatrix();
    loadSceneCameras();
}

void Camera::rebuildProjectionMatrix() {
    if (projection.kind == CameraProjectionKind::Orthographic) {
        projection_matrix =
            glm::ortho(-projection.xmag, projection.xmag, -projection.ymag, projection.ymag,
                       projection.znear, projection.zfar);
        return;
    }

    const auto aspect = projection.aspect.value_or(viewport_aspect);
    projection_matrix = glm::perspective(projection.yfov, aspect, projection.znear, projection.zfar);
}

void Camera::loadSceneCameras() {
    if (!GET_MODULE(PathResolver).isSetup()) {
        return;
    }

    const auto &config = GET_MODULE(ProjectBasicConfig);
    const auto scene_document = normalizeSceneDataJson(nlohmann::json::parse(config.sceneDataJson()));
    const auto scene_it = scene_document.scenes.find(config.defaultSceneId());
    if (scene_it == scene_document.scenes.end()) {
        return;
    }

    bool first_camera = true;
    for (const auto &object : scene_it.value().at("objects")) {
        const auto *camera_component = findComponent(object, "camera");
        if (camera_component == nullptr) {
            continue;
        }

        auto scene_camera = parseSceneCamera(object, *camera_component, projection, up);
        if (first_camera) {
            projection = scene_camera.projection;
            rebuildProjectionMatrix();
            active_scene_camera_locked = false;
            first_camera = false;
        }
        if (!scene_camera.name.empty()) {
            scene_cameras.insert_or_assign(scene_camera.name, std::move(scene_camera));
        }
    }
}

void Camera::applySceneCamera(const SceneCamera &camera) {
    pos = camera.pos;
    dir = camera.dir;
    up = camera.up;
    projection = camera.projection;
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

void Camera::setActiveCamera(std::string_view name) {
    const auto camera_it = scene_cameras.find(std::string{name});
    if (camera_it == scene_cameras.end()) {
        throw std::runtime_error("unknown camera name: " + std::string{name});
    }

    applySceneCamera(camera_it->second);
    active_camera_name = camera_it->first;
    active_scene_camera_locked = true;
}

} // namespace Pelican
