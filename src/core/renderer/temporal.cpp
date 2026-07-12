#include "temporal.hpp"

#include <stdexcept>

namespace Pelican {

glm::vec2 screenSpaceVelocity(const glm::vec4 &current_clip,
                              const glm::vec4 &previous_clip) {
    if (current_clip.w == 0.0f || previous_clip.w == 0.0f) {
        throw std::runtime_error("screen-space velocity requires non-zero clip w");
    }
    const glm::vec2 current_ndc = glm::vec2{current_clip} / current_clip.w;
    const glm::vec2 previous_ndc = glm::vec2{previous_clip} / previous_clip.w;
    return (current_ndc - previous_ndc) * 0.5f;
}

} // namespace Pelican
