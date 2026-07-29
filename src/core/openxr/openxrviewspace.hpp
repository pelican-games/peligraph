#pragma once

#include "openxrdiscovery.hpp"
#include "../renderer/viewfamily.hpp"

#include <glm/glm.hpp>
#include <span>
#include <string_view>
#include <vector>

namespace Pelican {

namespace OpenXr {

struct XrReferenceSpaceStatus {
    XrReferenceSpaceType type = XR_REFERENCE_SPACE_TYPE_MAX_ENUM;
    std::string_view reference_space;
    std::string_view floor_semantics;
    bool floor_level_guaranteed = false;
    float applied_floor_offset_m = 0.0f;
};

// Selects the first supported space in the normative order
// STAGE -> LOCAL_FLOOR -> LOCAL. No pose offset is applied to LOCAL: its lack
// of a floor-level origin remains explicit in XrReferenceSpaceStatus.
XrReferenceSpaceType selectReferenceSpace(
    std::span<const XrReferenceSpaceType> supported_spaces);
XrReferenceSpaceStatus describeReferenceSpace(XrReferenceSpaceType type);

// Pelican and OpenXR share RH/+Y-up/-Z-forward/metres/xyzw externally. The
// adapter still composes explicit matrices so no axis or sign convention is
// copied implicitly into the renderer.
glm::mat4 stageFromEye(const XrPosef &pose);
glm::mat4 asymmetricProjectionRhZo(const XrFovf &fov, float z_near,
                                   float z_far);
RenderViewParameters buildRenderViewParameters(
    const glm::mat4 &active_camera_view_at_frame_start, const XrView &view,
    float z_near, float z_far);
std::vector<RenderViewParameters> buildRenderViewParameters(
    const glm::mat4 &active_camera_view_at_frame_start,
    std::span<const XrView> views, float z_near, float z_far);
RenderViewFamily buildMainRenderViewFamily(
    const glm::mat4 &active_camera_view_at_frame_start,
    std::span<const XrView> views, float z_near, float z_far);

} // namespace OpenXr
} // namespace Pelican
