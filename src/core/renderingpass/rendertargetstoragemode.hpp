#pragma once

#include <cstdint>

namespace Pelican {

// Runtime allocation contract produced by the physical target compiler.
// Keep this neutral to the project-layer planner representation enum so the
// render-target allocator consumes only the modes it actually implements.
enum class RenderTargetStorageMode : std::uint8_t {
    materialized,
    transient_attachment,
};

} // namespace Pelican
