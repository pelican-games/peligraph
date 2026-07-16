#pragma once

#include <glm/glm.hpp>

namespace Pelican {

glm::vec2 screenSpaceVelocity(const glm::vec4 &current_clip,
                              const glm::vec4 &previous_clip,
                              glm::vec2 current_jitter_ndc = glm::vec2{0.0f},
                              glm::vec2 previous_jitter_ndc = glm::vec2{0.0f});

} // namespace Pelican
