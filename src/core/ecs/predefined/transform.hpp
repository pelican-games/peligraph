#pragma once

#include <glm/ext/quaternion_float.hpp>
#include <glm/glm.hpp>

namespace Pelican {

struct TransformComponent {
    glm::vec3 pos;
    glm::quat rotation;
    glm::vec3 scale;

    template <class T> void ref(T &ar) {}
};

} // namespace Pelican
