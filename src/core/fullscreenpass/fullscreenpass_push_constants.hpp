#pragma once

#include "../shader/pelican_sets.hpp"
#include <cstdint>
#include <glm/glm.hpp>

namespace Pelican {

struct FullscreenCameraPositionPushConstant {
    glm::vec4 cameraPos;
};

struct FullscreenProjectionViewPushConstant {
    glm::mat4 proj;
    glm::mat4 view;
};

constexpr uint32_t fullscreenPushConstantBytes = PELICAN_PUSH_TOTAL_BYTES;

static_assert(sizeof(FullscreenCameraPositionPushConstant) <= fullscreenPushConstantBytes);
static_assert(sizeof(FullscreenProjectionViewPushConstant) <= fullscreenPushConstantBytes);

} // namespace Pelican
