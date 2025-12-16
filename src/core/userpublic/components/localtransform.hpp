#pragma once

#include <geom/quat.hpp>
#include <geom/vec.hpp>
#include <details/ecs/entity.hpp>
#include <details/ecs/componentdeclare.hpp>

namespace Pelican {

struct LocalTransformComponent {
    vec3 scale;
    quat rotation;
    vec3 pos;
    EntityId parent;
};

} // namespace Pelican
