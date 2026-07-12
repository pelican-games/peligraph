#pragma once

#include <glm/glm.hpp>

namespace Pelican {

glm::vec2 screenSpaceVelocity(const glm::vec4 &current_clip,
                              const glm::vec4 &previous_clip);

} // namespace Pelican
