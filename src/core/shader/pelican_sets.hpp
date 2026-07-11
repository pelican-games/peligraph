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
inline constexpr uint32_t PELICAN_MATERIAL_BUFFER_BINDING = 6;

inline constexpr uint32_t PELICAN_PUSH_ENGINE_BYTES = 64;
inline constexpr uint32_t PELICAN_PUSH_SHADER_BYTES = 64;
inline constexpr uint32_t PELICAN_PUSH_TOTAL_BYTES = PELICAN_PUSH_ENGINE_BYTES + PELICAN_PUSH_SHADER_BYTES;

} // namespace Pelican
