#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Pelican {

// LP-Va: a view of a provider-owned family carries named integer axis
// coordinates (for example `$shadow/directional` views carry `light` and
// `cascade`). A pass or compute task may name some of them in `view_select`;
// the scheduler then runs it only on views whose coordinates match. Ordered
// maps keep the JSON and the fingerprints deterministic.
using RenderViewAxisCoordinates =
    std::map<std::string, std::uint32_t, std::less<>>;
using RenderViewSelect = RenderViewAxisCoordinates;

inline constexpr std::string_view renderViewAxisLight = "light";
inline constexpr std::string_view renderViewAxisCascade = "cascade";
inline constexpr std::string_view renderViewAxisFace = "face";

inline constexpr std::string_view mainRenderViewFamilyId =
    "$main";
inline constexpr std::string_view
    directionalShadowRenderViewFamilyId =
        "$shadow/directional";
inline constexpr std::string_view
    planarReflectionRenderViewFamilyId =
        "$reflection/planar";
inline constexpr std::string_view
    directionalShadowRenderViewId =
        "$cascade/0";
inline constexpr std::uint32_t
    maximumDirectionalShadowCascades = 8;
inline constexpr std::size_t
    maximumRenderViewFamilyIdBytes = 255;

inline std::string
directionalShadowCascadeRenderViewId(
    std::uint32_t cascade_index) {
    return "$cascade/" +
           std::to_string(cascade_index);
}

inline std::string
directionalShadowLightCascadeRenderViewId(
    std::uint32_t light_inventory_index,
    std::uint32_t cascade_index) {
    if (light_inventory_index == 0) {
        return directionalShadowCascadeRenderViewId(
            cascade_index);
    }
    return "$light/" +
           std::to_string(light_inventory_index) +
           "/cascade/" +
           std::to_string(cascade_index);
}

inline void validateRenderViewFamilyId(
    std::string_view family_id,
    std::string_view context) {
    if (family_id.empty()) {
        throw std::runtime_error(
            std::string{context} +
            " requires a non-empty view_family");
    }
    if (family_id.size() >
        maximumRenderViewFamilyIdBytes) {
        throw std::runtime_error(
            std::string{context} +
            " view_family exceeds " +
            std::to_string(
                maximumRenderViewFamilyIdBytes) +
            " bytes");
    }
}

} // namespace Pelican
