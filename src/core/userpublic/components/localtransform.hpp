#pragma once

#include <details/ecs/componentdeclare.hpp>
#include <details/ecs/entity.hpp>
#include <geom/quat.hpp>
#include <geom/vec.hpp>

namespace Pelican {

struct LocalTransformComponent {
    vec3 scale;
    quat rotation;
    vec3 pos;
    EntityId parent;

    template <class T> void ref(T &ar) {
        ar.prop("pos", pos);
        ar.prop("rotation", rotation);
        ar.prop("scale", scale);
    }
};

} // namespace Pelican
