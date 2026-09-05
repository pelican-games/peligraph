#pragma once

#include <details/ecs/componentdeclare.hpp>

#include <string>

namespace Pelican {

// Fixed-leaf scene carrier for one procedural asset occurrence. The GPU
// allocation and immutable row snapshot are owned by
// ProceduralGeometryContainer, not by this ECS value.
struct ProceduralGeometryComponent {
    std::string asset;

    bool operator==(const ProceduralGeometryComponent &) const = default;
};

DECLARE_COMPONENT(ProceduralGeometryComponent, 20);

} // namespace Pelican
