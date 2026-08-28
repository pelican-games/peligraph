#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace Pelican::TestSupport {

enum class BloomUpsampleStorage {
    rgba8_srgb,
    bgra8_srgb,
    rgba16_sfloat,
};

enum class BloomUpsampleAddressMode {
    repeat,
    clamp_to_edge,
};

enum class BloomUpsampleBlendMode {
    one_plus_one,
    replace,
};

struct BloomUpsampleImageView {
    BloomUpsampleStorage storage = BloomUpsampleStorage::rgba8_srgb;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::span<const std::uint8_t> bytes;
};

struct BloomUpsampleOracleRequest {
    BloomUpsampleImageView destination;
    BloomUpsampleImageView source;
    BloomUpsampleImageView actual;
    std::uint32_t sub_texel_precision_bits = 0;
    double pixel_center_x = 0.0;
    double pixel_center_y = 0.0;
    BloomUpsampleAddressMode address_mode =
        BloomUpsampleAddressMode::repeat;
    BloomUpsampleBlendMode blend_mode =
        BloomUpsampleBlendMode::one_plus_one;
};

struct BloomUpsampleOracleChannel {
    double ideal_linear = 0.0;
    double actual_linear = 0.0;
    double lower_linear = 0.0;
    double upper_linear = 0.0;
    double filter_bound_linear = 0.0;
    std::uint32_t ideal_storage = 0;
    std::uint32_t actual_storage = 0;
    std::uint32_t lower_storage = 0;
    std::uint32_t upper_storage = 0;
    std::uint32_t storage_distance = 0;
    std::uint32_t storage_allowance = 0;
    std::uint32_t storage_margin = 0;
    bool matches = false;
};

struct BloomUpsampleOracleResult {
    std::array<BloomUpsampleOracleChannel, 4> channels;
    bool matches = false;
};

// This is the sole public bloom-upsample oracle entry point. RGB channels of
// rgba8_srgb and bgra8_srgb RGB channels are decoded/encoded as sRGB; alpha
// always remains linear. Channel-order handling is implemented here rather
// than borrowed from renderer format helpers so the oracle stays independent.
BloomUpsampleOracleResult evaluateBloomUpsampleOracle(
    const BloomUpsampleOracleRequest &request);

} // namespace Pelican::TestSupport
