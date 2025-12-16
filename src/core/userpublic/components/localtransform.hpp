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

DECLARE_COMPONENT(LocalTransformComponent, 16);

} // namespace Pelican
