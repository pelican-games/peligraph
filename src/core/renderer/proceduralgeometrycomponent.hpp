#pragma once

#include <details/ecs/componentdeclare.hpp>

#include <string>
#include <vector>

namespace Pelican {

// Scene carrier for procedural assets sharing one object transform. The GPU
// allocation and immutable row snapshot are owned by
// ProceduralGeometryContainer, not by this ECS value.
struct ProceduralGeometryComponent {
    std::vector<std::string> assets;

    bool operator==(const ProceduralGeometryComponent &) const = default;
};

DECLARE_COMPONENT(ProceduralGeometryComponent, 20);

} // namespace Pelican
