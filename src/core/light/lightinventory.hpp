#pragma once

#include "light.hpp"

#include <cstdint>
#include <limits>
#include <span>
#include <vector>
#include <glm/glm.hpp>
#include <vulkan/vulkan.hpp>

namespace Pelican {

inline constexpr std::uint32_t
    lightInventoryV2Magic = 0x504C4932u;
inline constexpr std::uint32_t
    lightInventoryV2Version = 2u;
inline constexpr std::uint32_t
    lightInventoryV2HeaderElements = 2u;
inline constexpr std::uint32_t
    lightInventoryV2RecordElements = 4u;
inline constexpr std::uint32_t
    lightInventoryNoExtension =
        std::numeric_limits<std::uint32_t>::max();

enum class LightInventoryPresetV2 : std::uint32_t {
    directional = 0,
    point = 1,
    spot = 2,
    // Values from 16 onward are reserved for feature-owned record decoders.
    feature_first = 16,
};

struct PackedLightInventoryV2 {
    std::vector<glm::uvec4> elements;
    std::uint32_t input_count = 0;
    std::uint32_t accepted_count = 0;
    std::uint32_t dropped_count = 0;
    std::uint32_t directional_count = 0;
    std::uint32_t point_count = 0;
    std::uint32_t spot_count = 0;
    std::uint32_t record_capacity = 0;
};

// Packs the standard presets into one std430 uvec4 array:
//   header[0] = magic, version, accepted, dropped
//   header[1] = accepted directional/point/spot counts, record stride
//   record[n] = meta + three payload vectors
// meta.y is a feature-owned extension-buffer index. Standard scene lights use
// lightInventoryNoExtension, leaving area/cookie/IES data outside this ABI.
PackedLightInventoryV2 packLightInventoryV2(
    std::span<const DirectionalLight> directional,
    std::span<const PointLight> point,
    std::span<const SpotLight> spot,
    vk::DeviceSize byte_capacity);

} // namespace Pelican
