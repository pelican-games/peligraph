#pragma once

#include <cstdint>

namespace Pelican {

inline constexpr uint32_t PELICAN_SET_FRAME = 0;
inline constexpr uint32_t PELICAN_SET_PASS_INPUT = 1;
inline constexpr uint32_t PELICAN_SET_MATERIAL = 2;
inline constexpr uint32_t PELICAN_SET_FREE = 3;

inline constexpr uint32_t PELICAN_FRAME_UBO_BINDING = 0;
inline constexpr uint32_t PELICAN_OBJECT_BUFFER_BINDING = 1;
inline constexpr uint32_t PELICAN_LIGHT_UBO_BINDING = 2;
inline constexpr uint32_t PELICAN_PREVIOUS_OBJECT_BUFFER_BINDING = 3;
inline constexpr uint32_t PELICAN_FRAME_RESOLUTION_UBO_BINDING = 4;
inline constexpr uint32_t PELICAN_DIRECTIONAL_SHADOW_DATA_BINDING = 5;
inline constexpr uint32_t PELICAN_MATERIAL_BUFFER_BINDING = 6;
inline constexpr uint32_t PELICAN_SKIN_PALETTE_BINDING = 2;
inline constexpr uint32_t PELICAN_PREVIOUS_SKIN_PALETTE_BINDING = 3;
inline constexpr uint32_t PELICAN_MORPH_INSTANCE_BINDING = 4;
inline constexpr uint32_t PELICAN_MORPH_WEIGHT_BINDING = 5;
inline constexpr uint32_t PELICAN_PREVIOUS_MORPH_WEIGHT_BINDING = 6;
inline constexpr uint32_t PELICAN_MORPH_METADATA_BINDING = 7;
inline constexpr uint32_t PELICAN_MORPH_DELTA_BINDING = 8;
inline constexpr uint32_t PELICAN_MATERIAL_INSTANCE_OVERRIDE_BINDING = 9;
inline constexpr uint32_t PELICAN_MATERIAL_INSTANCE_ABSOLUTE_HEADER_BINDING = 10;
inline constexpr uint32_t PELICAN_MATERIAL_INSTANCE_ABSOLUTE_RECORD_BINDING = 11;

inline constexpr uint32_t PELICAN_PUSH_ENGINE_BYTES = 64;
inline constexpr uint32_t PELICAN_PUSH_SHADER_BYTES = 64;
inline constexpr uint32_t PELICAN_PUSH_TOTAL_BYTES = PELICAN_PUSH_ENGINE_BYTES + PELICAN_PUSH_SHADER_BYTES;

} // namespace Pelican
