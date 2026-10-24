#pragma once

#include <cstdint>

namespace Pelican {

// Runtime allocation contract produced by the physical target compiler.
// Keep this neutral to the project-layer planner representation enum so the
// render-target allocator consumes only the modes it actually implements.
// RC-3c2a: project's, as the generation's resource description
// (generationresources.hpp) carries it; core's header includes this one.
enum class RenderTargetStorageMode : std::uint8_t {
    materialized,
    transient_attachment,
    tile_local_attachment,
};

} // namespace Pelican
