#pragma once

#include "../../renderer/viewfamily.hpp"

namespace Pelican {

inline constexpr std::string_view
    cubeCaptureRenderFeatureName =
        "cube_capture";
inline constexpr std::string_view
    cubeCaptureRenderViewFamilyId =
        "$capture/cube";
inline constexpr std::string_view
    cubeCapturePositiveXViewId =
        "$face/+x";
inline constexpr std::string_view
    cubeCaptureNegativeXViewId =
        "$face/-x";
inline constexpr std::string_view
    cubeCapturePositiveYViewId =
        "$face/+y";
inline constexpr std::string_view
    cubeCaptureNegativeYViewId =
        "$face/-y";
inline constexpr std::string_view
    cubeCapturePositiveZViewId =
        "$face/+z";
inline constexpr std::string_view
    cubeCaptureNegativeZViewId =
        "$face/-z";

struct CubeCaptureViewSettings {
    glm::vec3 position{0.0f};
    float near_distance = 0.1f;
    float far_distance = 1000.0f;
};

// Creates one stable view for each Vulkan cube layer in +X, -X, +Y, -Y,
// +Z, -Z order. This is replaceable camera policy; the renderer only sees
// the returned generic ViewFamily.
RenderViewFamily buildCubeCaptureViewFamily(
    const CubeCaptureViewSettings
        &settings);

} // namespace Pelican
