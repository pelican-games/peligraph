#pragma once

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

constexpr uint32_t fullscreenPushConstantBytes =
    static_cast<uint32_t>(sizeof(FullscreenProjectionViewPushConstant));

static_assert(sizeof(FullscreenCameraPositionPushConstant) <= fullscreenPushConstantBytes);

} // namespace Pelican
