#pragma once

#include "../../geomhelper/geomhelper.hpp"
#include <glm/ext/quaternion_float.hpp>
#include <glm/glm.hpp>

namespace Pelican {

struct TransformComponent {
    glm::vec3 pos;
    glm::quat rotation;
    glm::vec3 scale;

    template <class T> void ref(T &ar) {
        vec3 loaded_pos;
        quat loaded_rotation;
        vec3 loaded_scale;

        ar.prop("pos", loaded_pos);
        ar.prop("rotation", loaded_rotation);
        ar.prop("scale", loaded_scale);

        pos = to_glm(loaded_pos);
        rotation = to_glm(loaded_rotation);
        scale = to_glm(loaded_scale);
    }
};

} // namespace Pelican
