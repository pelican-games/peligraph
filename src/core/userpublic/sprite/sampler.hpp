#pragma once

#include <cstdint>

namespace Pelican::sprite {

// Asset declarations own this choice; SpriteView has no per-instance override.
// Keeping the key in the public sprite contract lets S2D-0b batch on it without
// exposing the consumer-neutral atlas parser as public API.
enum class SamplerKey : std::uint8_t { nearest, linear };

} // namespace Pelican::sprite
