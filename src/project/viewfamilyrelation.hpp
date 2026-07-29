#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Pelican {

inline constexpr std::string_view mainRenderViewFamilyId =
    "$main";
inline constexpr std::string_view
    directionalShadowRenderViewFamilyId =
        "$shadow/directional";
inline constexpr std::string_view
    directionalShadowRenderViewId =
        "$cascade/0";
inline constexpr std::size_t
    maximumRenderViewFamilyIdBytes = 255;

inline std::string
directionalShadowCascadeRenderViewId(
    std::uint32_t cascade_index) {
    return "$cascade/" +
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
