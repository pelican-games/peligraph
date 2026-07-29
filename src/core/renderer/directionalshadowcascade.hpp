#pragma once

#include "viewfamily.hpp"

#include <cstdint>

#include <glm/glm.hpp>

namespace Pelican {

inline constexpr std::uint32_t
    maximumDirectionalShadowCascades = 8;

struct DirectionalShadowCascadeSettings {
    std::uint32_t cascade_count = 1;
    float max_distance = 100.0f;
    float split_lambda = 0.65f;
    bool stabilize = true;
};

// Builds camera-relative directional-shadow views. Every result has a stable
// $cascade/N identity and a main-camera depth range used by the lighting ABI.
// Multiple main views (XR) contribute to the same cascade bounds.
RenderViewFamily buildDirectionalShadowCascadeFamily(
    const RenderViewFamily &main_family,
    glm::vec3 light_direction,
    glm::uvec2 shadow_extent,
    const DirectionalShadowCascadeSettings
        &settings);

} // namespace Pelican
