#include "openxrviewspace.hpp"

#include "../vkcore/renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Pelican::OpenXr {
namespace {

bool contains(std::span<const XrReferenceSpaceType> supported_spaces,
              XrReferenceSpaceType candidate) {
    return std::find(supported_spaces.begin(), supported_spaces.end(), candidate) !=
           supported_spaces.end();
}

void requireFinite(float value, const char *name) {
    if (!std::isfinite(value)) {
        throw std::runtime_error(std::string{"OpenXR view "} + name +
                                 " must be finite");
    }
}

} // namespace

XrReferenceSpaceType selectReferenceSpace(
    std::span<const XrReferenceSpaceType> supported_spaces) {
    constexpr std::array priority{
        XR_REFERENCE_SPACE_TYPE_STAGE,
        XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR,
        XR_REFERENCE_SPACE_TYPE_LOCAL,
    };
    for (const auto candidate : priority) {
        if (contains(supported_spaces, candidate)) return candidate;
    }
    throw std::runtime_error(
        "OpenXR runtime exposes none of STAGE, LOCAL_FLOOR, or LOCAL reference spaces");
}

XrReferenceSpaceStatus describeReferenceSpace(XrReferenceSpaceType type) {
    switch (type) {
    case XR_REFERENCE_SPACE_TYPE_STAGE:
        return {type, "STAGE", "stage_floor", true, 0.0f};
    case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR:
        return {type, "LOCAL_FLOOR", "estimated_floor", true, 0.0f};
    case XR_REFERENCE_SPACE_TYPE_LOCAL:
        return {type, "LOCAL", "floor_not_guaranteed", false, 0.0f};
    default:
        throw std::invalid_argument("OpenXR reference-space status requested for an unsupported type");
    }
}

glm::mat4 stageFromEye(const XrPosef &pose) {
    requireFinite(pose.position.x, "position.x");
    requireFinite(pose.position.y, "position.y");
    requireFinite(pose.position.z, "position.z");
    requireFinite(pose.orientation.x, "orientation.x");
    requireFinite(pose.orientation.y, "orientation.y");
    requireFinite(pose.orientation.z, "orientation.z");
    requireFinite(pose.orientation.w, "orientation.w");

    const glm::quat orientation{pose.orientation.w, pose.orientation.x,
                                pose.orientation.y, pose.orientation.z};
    const auto length_squared = glm::dot(orientation, orientation);
    if (!(length_squared > 0.0f) || !std::isfinite(length_squared)) {
        throw std::runtime_error("OpenXR view orientation must be a non-zero finite quaternion");
    }
    return glm::translate(glm::mat4{1.0f},
                          {pose.position.x, pose.position.y, pose.position.z}) *
           glm::mat4_cast(glm::normalize(orientation));
}

glm::mat4 asymmetricProjectionRhZo(const XrFovf &fov, float z_near,
                                   float z_far) {
    requireFinite(fov.angleLeft, "fov.angleLeft");
    requireFinite(fov.angleRight, "fov.angleRight");
    requireFinite(fov.angleDown, "fov.angleDown");
    requireFinite(fov.angleUp, "fov.angleUp");
    requireFinite(z_near, "z_near");
    requireFinite(z_far, "z_far");
    if (!(z_near > 0.0f && z_far > z_near)) {
        throw std::runtime_error("OpenXR projection requires 0 < z_near < z_far");
    }

    const float tangent_left = std::tan(fov.angleLeft);
    const float tangent_right = std::tan(fov.angleRight);
    const float tangent_down = std::tan(fov.angleDown);
    const float tangent_up = std::tan(fov.angleUp);
    const float tangent_width = tangent_right - tangent_left;
    const float tangent_height = tangent_up - tangent_down;
    if (!(tangent_width > 0.0f && tangent_height > 0.0f) ||
        !std::isfinite(tangent_width) || !std::isfinite(tangent_height)) {
        throw std::runtime_error("OpenXR projection FOV has an invalid tangent extent");
    }

    glm::mat4 projection{0.0f};
    projection[0][0] = 2.0f / tangent_width;
    // Pelican records a positive-height Vulkan viewport. Flip clip-space Y so
    // angleUp reaches the top (NDC -1) and angleDown reaches the bottom (+1).
    projection[1][1] = -2.0f / tangent_height;
    projection[2][0] = (tangent_right + tangent_left) / tangent_width;
    projection[2][1] = -(tangent_up + tangent_down) / tangent_height;
    projection[2][2] = -z_far / (z_far - z_near);
    projection[2][3] = -1.0f;
    projection[3][2] = -(z_far * z_near) / (z_far - z_near);
    return projection;
}

RenderViewParameters buildRenderViewParameters(
    const glm::mat4 &active_camera_view_at_frame_start, const XrView &view,
    float z_near, float z_far) {
    const glm::mat4 world_from_stage =
        glm::inverse(active_camera_view_at_frame_start);
    const glm::mat4 stage_from_eye = stageFromEye(view.pose);
    const glm::mat4 world_from_eye = world_from_stage * stage_from_eye;
    return {
        .view = glm::inverse(world_from_eye),
        .projection = asymmetricProjectionRhZo(view.fov, z_near, z_far),
        .camera_position = glm::vec3{world_from_eye[3]},
        .first_person_view = true,
    };
}

std::vector<RenderViewParameters> buildRenderViewParameters(
    const glm::mat4 &active_camera_view_at_frame_start,
    std::span<const XrView> views, float z_near, float z_far) {
    std::vector<RenderViewParameters> result;
    result.reserve(views.size());
    for (const auto &view : views) {
        result.push_back(buildRenderViewParameters(
            active_camera_view_at_frame_start, view, z_near, z_far));
    }
    return result;
}

} // namespace Pelican::OpenXr
