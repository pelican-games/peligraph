#include "gamesystem.hpp"

#include "../loader/basicconfig.hpp"
#include "../loader/scene.hpp"
#include "../renderer/camera.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Pelican {

namespace {

using ControllerSpec = Camera::SceneCameraController;
using ControllerType = Camera::SceneCameraControllerType;

struct CameraPose {
    glm::vec3 pos{0.0f, 0.0f, 0.0f};
    glm::vec3 dir{0.0f, 0.0f, 1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
};

struct ControllerState {
    bool initialized = false;
    bool has_frame = false;
    std::uint64_t last_frame = 0;
    std::string signature;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float distance = 0.0f;
    glm::vec3 orbit_target{0.0f, 0.0f, 0.0f}, pan_offset{0.0f, 0.0f, 0.0f};
    CameraPose pose;
};
constexpr float maxPitchRadians = 1.55334306f;

std::string controllerSignature(const ControllerSpec &controller) {
    std::ostringstream stream;
    stream.precision(9);
    stream << static_cast<int>(controller.type) << '|'
           << controller.target << '|'
           << controller.offset.x << ',' << controller.offset.y << ',' << controller.offset.z << '|'
           << controller.distance << '|'
           << controller.yaw << '|'
           << controller.pitch << '|'
           << controller.damping << '|'
           << controller.speed << '|'
           << controller.sensitivity;
    return stream.str();
}

glm::vec3 normalizeOr(glm::vec3 value, glm::vec3 fallback) {
    const auto len2 = glm::dot(value, value);
    if (len2 <= 0.00000001f) {
        return glm::normalize(fallback);
    }
    return value * glm::inversesqrt(len2);
}

glm::vec3 projectWorldUp() {
    return normalizeOr(GET_MODULE(ProjectBasicConfig).initailCameraProperty().up,
                       glm::vec3{0.0f, 1.0f, 0.0f});
}

glm::vec3 leastAlignedCanonicalAxis(glm::vec3 axis) {
    const auto x_alignment = std::abs(axis.x);
    const auto y_alignment = std::abs(axis.y);
    const auto z_alignment = std::abs(axis.z);
    // Prefer Z on ties so the existing +Y convention keeps its pole fallback.
    if (z_alignment <= x_alignment && z_alignment <= y_alignment) {
        return glm::vec3{0.0f, 0.0f, 1.0f};
    }
    if (x_alignment <= y_alignment) {
        return glm::vec3{1.0f, 0.0f, 0.0f};
    }
    return glm::vec3{0.0f, 1.0f, 0.0f};
}

glm::vec3 worldUpFor(glm::vec3 dir) {
    const auto world_up = projectWorldUp();
    if (std::abs(glm::dot(glm::normalize(dir), world_up)) > 0.98f) {
        return leastAlignedCanonicalAxis(world_up);
    }
    return world_up;
}

struct YawPitchBasis {
    glm::vec3 up;
    glm::vec3 forward;
    glm::vec3 right;
};

YawPitchBasis yawPitchBasis() {
    const auto up = projectWorldUp();
    constexpr glm::vec3 canonical_forward{0.0f, 0.0f, 1.0f};
    const auto forward = normalizeOr(canonical_forward - up * glm::dot(canonical_forward, up),
                                     leastAlignedCanonicalAxis(up));
    return YawPitchBasis{
        .up = up,
        .forward = forward,
        .right = normalizeOr(glm::cross(up, forward), glm::vec3{1.0f, 0.0f, 0.0f}),
    };
}

CameraPose makePose(glm::vec3 pos, glm::vec3 dir_hint) {
    CameraPose pose;
    pose.pos = pos;
    pose.dir = normalizeOr(dir_hint, glm::vec3{0.0f, 0.0f, 1.0f});
    const auto up_hint = worldUpFor(pose.dir);
    const auto right = normalizeOr(glm::cross(up_hint, pose.dir), glm::vec3{1.0f, 0.0f, 0.0f});
    pose.up = normalizeOr(glm::cross(pose.dir, right), up_hint);
    return pose;
}

CameraPose lookAtPose(glm::vec3 pos, glm::vec3 target) {
    return makePose(pos, target - pos);
}

glm::vec3 directionFromYawPitch(float yaw, float pitch) {
    const auto basis = yawPitchBasis();
    const auto cos_pitch = std::cos(pitch);
    return normalizeOr((basis.forward * std::cos(yaw) + basis.right * std::sin(yaw)) * cos_pitch +
                           basis.up * std::sin(pitch),
                       basis.forward);
}

float yawFromDirection(glm::vec3 dir) {
    const auto basis = yawPitchBasis();
    const auto normalized = normalizeOr(dir, glm::vec3{0.0f, 0.0f, 1.0f});
    return std::atan2(glm::dot(normalized, basis.right), glm::dot(normalized, basis.forward));
}

float pitchFromDirection(glm::vec3 dir) {
    const auto basis = yawPitchBasis();
    const auto normalized = normalizeOr(dir, glm::vec3{0.0f, 0.0f, 1.0f});
    return std::asin(std::clamp(glm::dot(normalized, basis.up), -1.0f, 1.0f));
}

float clampPitch(float pitch) {
    return std::clamp(pitch, -maxPitchRadians, maxPitchRadians);
}

float blendWeight(float damping, double dt) {
    if (damping <= 0.0f) {
        return 1.0f;
    }
    if (dt <= 0.0) {
        return 0.0f;
    }
    return 1.0f - std::exp(-damping * static_cast<float>(dt));
}

CameraPose blendPose(CameraPose current, CameraPose desired, float weight) {
    if (weight >= 1.0f) {
        return desired;
    }
    if (weight <= 0.0f) {
        return current;
    }
    return CameraPose{
        .pos = glm::mix(current.pos, desired.pos, weight),
        .dir = normalizeOr(glm::mix(current.dir, desired.dir, weight), desired.dir),
        .up = normalizeOr(glm::mix(current.up, desired.up, weight), desired.up),
    };
}

glm::quat rotationFromPose(const CameraPose &pose) {
    const auto dir = normalizeOr(pose.dir, glm::vec3{0.0f, 0.0f, 1.0f});
    const auto up_hint = worldUpFor(dir);
    const auto right_hint = normalizeOr(glm::cross(up_hint, dir), glm::vec3{1.0f, 0.0f, 0.0f});
    const auto right = normalizeOr(glm::cross(pose.up, dir), right_hint);
    const auto up = normalizeOr(glm::cross(dir, right), up_hint);
    return glm::quat_cast(glm::mat3{right, up, dir});
}

bool isUnknownActionError(const std::runtime_error &error) {
    const std::string message = error.what();
    return message.rfind("unknown input action:", 0) == 0;
}

ActionAxis2 optionalAxis2(GameContext &ctx, std::string_view action_name) {
    if (!ctx.actionsConfigured()) {
        return {};
    }
    try {
        return ctx.actionAxis2(action_name);
    } catch (const std::runtime_error &error) {
        if (isUnknownActionError(error)) {
            return {};
        }
        throw;
    }
}

SceneObjectTransform requireTargetTransform(std::string_view camera_name, const ControllerSpec &controller) {
    auto &scene = GET_MODULE(SceneLoader);
    if (!scene.hasObjectTransform(controller.target)) {
        throw std::runtime_error("camera '" + std::string{camera_name} + "' controller target '" +
                                 controller.target + "' has no transform");
    }
    return scene.objectTransform(controller.target);
}

void resetIfNeeded(GameContext &ctx, ControllerState &state, const ControllerSpec &controller) {
    const auto signature = controllerSignature(controller);
    if (state.signature != signature || (state.has_frame && ctx.frameIndex() <= state.last_frame)) {
        state = ControllerState{};
        state.signature = signature;
    }
    state.has_frame = true;
    state.last_frame = ctx.frameIndex();
}

void applyPoseToSceneCamera(std::string_view camera_name, const CameraPose &pose) {
    auto &scene = GET_MODULE(SceneLoader);
    if (scene.hasObjectTransform(camera_name)) {
        auto transform = scene.objectTransform(camera_name);
        transform.pos = pose.pos;
        transform.rotation = rotationFromPose(pose);
        scene.applyObjectTransform(camera_name, transform);
    }
    GET_MODULE(Camera).applyControllerPose(camera_name, pose.pos, pose.dir, pose.up);
}
float optionalAxis1(GameContext &ctx, std::string_view action_name);
glm::vec3 resolveOrbitTarget(std::string_view, const ControllerSpec &, ControllerState &, bool);
glm::vec3 panOrbitTarget(glm::vec3, glm::vec3, ActionAxis2, float, ControllerState &);
CameraPose updateOrbit(GameContext &ctx, std::string_view camera_name, const ControllerSpec &controller,
                       ControllerState &state) {
    const bool first_update = !state.initialized;
    auto target = resolveOrbitTarget(camera_name, controller, state, first_update);
    const auto dt = std::max(ctx.deltaTime(), 0.0);
    if (first_update) {
        if (!controller.target.empty()) {
            state.yaw = controller.yaw; state.pitch = clampPitch(controller.pitch);
        }
        state.distance = controller.distance; state.initialized = true;
    }
    const auto look = optionalAxis2(ctx, "look");
    const auto move = optionalAxis2(ctx, "move");
    const auto pan = optionalAxis2(ctx, "pan");
    const auto zoom_input = move.y + optionalAxis1(ctx, "zoom");
    state.yaw += (look.x + move.x) * controller.sensitivity * static_cast<float>(dt);
    state.pitch = clampPitch(state.pitch + look.y * controller.sensitivity * static_cast<float>(dt));
    state.distance = std::max(0.001f, state.distance - zoom_input * controller.sensitivity *
                                         std::max(1.0f, state.distance) * static_cast<float>(dt));
    const auto offset = directionFromYawPitch(state.yaw, state.pitch) * state.distance;
    target = panOrbitTarget(target, offset, pan, controller.sensitivity *
                                 std::max(1.0f, state.distance) * static_cast<float>(dt), state);
    const auto desired = lookAtPose(target + offset, target);
    state.pose = first_update ? desired : blendPose(state.pose, desired, blendWeight(controller.damping, dt));
    return state.pose;
}

CameraPose updateFollow(GameContext &ctx, std::string_view camera_name, const ControllerSpec &controller,
                        ControllerState &state) {
    const auto target = requireTargetTransform(camera_name, controller);
    const auto desired = lookAtPose(target.pos + controller.offset, target.pos);
    if (!state.initialized) {
        state.pose = desired;
        state.initialized = true;
        return state.pose;
    }
    state.pose = blendPose(state.pose, desired, blendWeight(controller.damping, std::max(ctx.deltaTime(), 0.0)));
    return state.pose;
}

CameraPose initialFlyPose(std::string_view camera_name) {
    auto &scene = GET_MODULE(SceneLoader);
    if (scene.hasObjectTransform(camera_name)) {
        const auto transform = scene.objectTransform(camera_name);
        return CameraPose{
            .pos = transform.pos,
            .dir = normalizeOr(transform.rotation * glm::vec3{0.0f, 0.0f, 1.0f},
                               glm::vec3{0.0f, 0.0f, 1.0f}),
            .up = normalizeOr(transform.rotation * glm::vec3{0.0f, 1.0f, 0.0f},
                              projectWorldUp()),
        };
    }

    auto &camera = GET_MODULE(Camera);
    return CameraPose{
        .pos = camera.getPos(),
        .dir = camera.getDir(),
        .up = camera.getUp(),
    };
}

CameraPose updateFly(GameContext &ctx, std::string_view camera_name, const ControllerSpec &controller,
                     ControllerState &state) {
    if (!state.initialized) {
        state.pose = initialFlyPose(camera_name);
        state.yaw = yawFromDirection(state.pose.dir);
        state.pitch = pitchFromDirection(state.pose.dir);
        state.initialized = true;
    }
    const auto dt = static_cast<float>(std::max(ctx.deltaTime(), 0.0));
    const auto look = optionalAxis2(ctx, "look");
    const auto move = optionalAxis2(ctx, "move");
    state.yaw += look.x * controller.sensitivity * dt;
    state.pitch = clampPitch(state.pitch + look.y * controller.sensitivity * dt);

    const auto dir = directionFromYawPitch(state.yaw, state.pitch);
    const auto up_hint = worldUpFor(dir);
    const auto right = normalizeOr(glm::cross(up_hint, dir), glm::vec3{1.0f, 0.0f, 0.0f});
    const auto up = normalizeOr(glm::cross(dir, right), up_hint);
    state.pose.pos += (right * move.x + dir * move.y) * controller.speed * dt;
    state.pose.dir = dir;
    state.pose.up = up;
    return state.pose;
}

class BuiltinCameraControllerSystem {
    std::unordered_map<std::string, ControllerState> states;

  public:
    void update(GameContext &ctx) {
        auto &camera = GET_MODULE(Camera);
        const auto &controlled_cameras = camera.controlledSceneCameraNames();
        const auto active_camera = camera.activeCameraName();

        for (const auto &camera_name : controlled_cameras) {
            if (!active_camera.empty() && camera_name != active_camera) {
                continue;
            }

            const auto *controller = camera.sceneCameraController(camera_name);
            if (controller == nullptr) {
                continue;
            }

            auto &state = states[camera_name];
            resetIfNeeded(ctx, state, *controller);

            CameraPose pose;
            switch (controller->type) {
            case ControllerType::Orbit:
                pose = updateOrbit(ctx, camera_name, *controller, state);
                break;
            case ControllerType::Follow:
                pose = updateFollow(ctx, camera_name, *controller, state);
                break;
            case ControllerType::Fly:
                pose = updateFly(ctx, camera_name, *controller, state);
                break;
            }
            applyPoseToSceneCamera(camera_name, pose);

            if (active_camera.empty()) {
                break;
            }
        }
    }
};

float optionalAxis1(GameContext &ctx, std::string_view action_name) {
    if (!ctx.actionsConfigured()) {
        return 0.0f;
    }
    try {
        return ctx.actionAxis1(action_name);
    } catch (const std::runtime_error &error) {
        if (isUnknownActionError(error)) {
            return 0.0f;
        }
        throw;
    }
}

glm::vec3 resolveOrbitTarget(std::string_view camera_name, const ControllerSpec &controller,
                             ControllerState &state, bool first_update) {
    if (!controller.target.empty()) {
        return requireTargetTransform(camera_name, controller).pos;
    }
    if (!first_update) {
        return state.orbit_target;
    }

    const auto initial_pose = initialFlyPose(camera_name);
    const auto initial_dir = normalizeOr(initial_pose.dir, yawPitchBasis().forward);
    state.orbit_target = initial_pose.pos + initial_dir * controller.distance;
    const auto camera_offset = initial_pose.pos - state.orbit_target;
    state.yaw = yawFromDirection(camera_offset);
    state.pitch = clampPitch(pitchFromDirection(camera_offset));
    return state.orbit_target;
}

glm::vec3 panOrbitTarget(glm::vec3 target, glm::vec3 camera_offset, ActionAxis2 pan,
                         float scale, ControllerState &state) {
    target += state.pan_offset;
    if (pan.x == 0.0f && pan.y == 0.0f) {
        return target;
    }

    const auto pose = lookAtPose(target + camera_offset, target);
    const auto right = normalizeOr(glm::cross(pose.up, pose.dir), yawPitchBasis().right);
    const auto delta = (right * pan.x + pose.up * pan.y) * scale;
    state.pan_offset += delta;
    return target + delta;
}

} // namespace

PELICAN_REGISTER_SYSTEM(BuiltinCameraControllerSystem, 10000);

} // namespace Pelican
