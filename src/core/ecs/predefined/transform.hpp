#pragma once

#include <glm/glm.hpp>
#include <glm/ext/quaternion_float.hpp>

namespace Pelican {

struct TransformComponent {
    glm::vec3 pos;
    glm::quat rotation;
    glm::vec3 scale;
};

} // namespace Pelican
