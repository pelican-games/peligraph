#include "temporal.hpp"

#include <stdexcept>

namespace Pelican {

glm::vec2 screenSpaceVelocity(const glm::vec4 &current_clip,
                              const glm::vec4 &previous_clip,
                              glm::vec2 current_jitter_ndc,
                              glm::vec2 previous_jitter_ndc) {
    if (current_clip.w == 0.0f || previous_clip.w == 0.0f) {
        throw std::runtime_error("screen-space velocity requires non-zero clip w");
    }
    const glm::vec2 current_ndc = glm::vec2{current_clip} / current_clip.w - current_jitter_ndc;
    const glm::vec2 previous_ndc =
        glm::vec2{previous_clip} / previous_clip.w - previous_jitter_ndc;
    return (current_ndc - previous_ndc) * 0.5f;
}

} // namespace Pelican
