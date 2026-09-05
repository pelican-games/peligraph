#pragma once

#include <cstdint>
#include <string>

namespace Pelican {

struct SceneAccelerationStructureDefinition {
    std::string name;
    std::uint32_t instance_mask = 255;
    bool operator==(const SceneAccelerationStructureDefinition &) const = default;
};

} // namespace Pelican
