#pragma once

#include "../../light/light.hpp"
#include "../../renderer/viewfamily.hpp"
#include <span>

namespace Pelican {

inline constexpr std::string_view pointShadowViewFamilyId = "$shadow/point";
inline constexpr std::string_view spotShadowViewFamilyId = "$shadow/spot";

// Light-major ordering; point lights contain Vulkan's six cube faces per light.
// Near/far policy belongs to the requesting feature, not to the camera helper.
RenderViewFamily buildPointShadowViewFamily(std::span<const PointLight> lights,
                                            float near_distance, float far_distance);
RenderViewFamily buildSpotShadowViewFamily(std::span<const SpotLight> lights,
                                           float near_distance, float far_distance);

} // namespace Pelican
