#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace Pelican {

// The authored count remains independent from a concrete output extent.
// full_chain is resolved again whenever the runtime recreates the image.
enum class ImageMipLevelMode : std::uint8_t {
    fixed,
    full_chain,
};

inline std::string_view imageMipLevelModeName(
    ImageMipLevelMode mode) {
    switch (mode) {
    case ImageMipLevelMode::fixed:
        return "fixed";
    case ImageMipLevelMode::full_chain:
        return "full";
    }
    throw std::runtime_error(
        "unknown image mip-level mode");
}

struct ImageMipLevelCount {
    ImageMipLevelMode mode =
        ImageMipLevelMode::fixed;
    // Ignored for full_chain. Keeping the canonical value at one makes
    // fingerprints and equality deterministic.
    std::uint32_t count = 1;

    bool operator==(
        const ImageMipLevelCount &) const = default;
};

inline std::uint32_t maximumImageMipLevels(
    std::uint32_t width, std::uint32_t height) {
    const auto largest = std::max(width, height);
    return largest == 0 ? 0u : std::bit_width(largest);
}

inline std::uint32_t resolveImageMipLevels(
    ImageMipLevelCount authored,
    std::uint32_t width, std::uint32_t height) {
    const auto maximum =
        maximumImageMipLevels(width, height);
    if (maximum == 0) {
        throw std::runtime_error(
            "image mip levels require a non-zero extent");
    }
    if (authored.mode ==
        ImageMipLevelMode::full_chain) {
        return maximum;
    }
    if (authored.count == 0 ||
        authored.count > maximum) {
        throw std::runtime_error(
            "fixed image mip-level count exceeds the image extent");
    }
    return authored.count;
}

enum class ImageSubresourceMipCountMode : std::uint8_t {
    fixed,
    remaining,
};

// A shader-visible image view. remaining is an authored/runtime-relative
// count, resolved to an explicit count before a Vulkan image view is cached
// or created. Array-layer counts remain explicit.
struct ImageSubresourceRange {
    std::uint32_t base_mip_level = 0;
    std::uint32_t level_count = 1;
    std::uint32_t base_array_layer = 0;
    std::uint32_t layer_count = 1;
    ImageSubresourceMipCountMode mip_count_mode =
        ImageSubresourceMipCountMode::fixed;

    bool operator==(
        const ImageSubresourceRange &) const = default;
};

struct ImageSubresourceViewKey {
    ImageSubresourceRange range;
    bool array_view = false;

    bool operator==(
        const ImageSubresourceViewKey &) const = default;
    bool operator<(
        const ImageSubresourceViewKey &other) const {
        if (range.base_mip_level !=
            other.range.base_mip_level) {
            return range.base_mip_level <
                   other.range.base_mip_level;
        }
        if (range.level_count !=
            other.range.level_count) {
            return range.level_count <
                   other.range.level_count;
        }
        if (range.base_array_layer !=
            other.range.base_array_layer) {
            return range.base_array_layer <
                   other.range.base_array_layer;
        }
        if (range.layer_count !=
            other.range.layer_count) {
            return range.layer_count <
                   other.range.layer_count;
        }
        if (range.mip_count_mode !=
            other.range.mip_count_mode) {
            return range.mip_count_mode <
                   other.range.mip_count_mode;
        }
        return array_view < other.array_view;
    }
};

inline bool validImageSubresourceRange(
    const ImageSubresourceRange &range,
    std::uint32_t mip_levels,
    std::uint32_t array_layers) {
    const auto valid_mips =
        range.base_mip_level < mip_levels &&
        (range.mip_count_mode ==
                 ImageSubresourceMipCountMode::
                     remaining ||
         (range.level_count != 0 &&
          range.level_count <=
              mip_levels -
                  range.base_mip_level));
    return range.layer_count != 0 &&
           valid_mips &&
           range.base_array_layer < array_layers &&
           range.layer_count <=
               array_layers - range.base_array_layer;
}

inline ImageSubresourceRange
resolveImageSubresourceRange(
    ImageSubresourceRange range,
    std::uint32_t mip_levels,
    std::uint32_t array_layers) {
    if (!validImageSubresourceRange(
            range, mip_levels, array_layers)) {
        throw std::runtime_error(
            "image subresource range is outside the image");
    }
    if (range.mip_count_mode ==
        ImageSubresourceMipCountMode::remaining) {
        range.level_count =
            mip_levels - range.base_mip_level;
        range.mip_count_mode =
            ImageSubresourceMipCountMode::fixed;
    }
    return range;
}

inline bool imageSubresourceRangesOverlap(
    const ImageSubresourceRange &left,
    const ImageSubresourceRange &right) {
    const auto left_mip_end =
        left.mip_count_mode ==
                ImageSubresourceMipCountMode::
                    remaining
            ? std::numeric_limits<
                  std::uint64_t>::max()
            : static_cast<std::uint64_t>(
                  left.base_mip_level) +
                  left.level_count;
    const auto right_mip_end =
        right.mip_count_mode ==
                ImageSubresourceMipCountMode::
                    remaining
            ? std::numeric_limits<
                  std::uint64_t>::max()
            : static_cast<std::uint64_t>(
                  right.base_mip_level) +
                  right.level_count;
    const auto mip_overlap =
        left.base_mip_level < right_mip_end &&
        right.base_mip_level < left_mip_end;
    const auto left_layer_end =
        static_cast<std::uint64_t>(
            left.base_array_layer) +
        left.layer_count;
    const auto right_layer_end =
        static_cast<std::uint64_t>(
            right.base_array_layer) +
        right.layer_count;
    const auto layer_overlap =
        left.base_array_layer <
            right_layer_end &&
        right.base_array_layer <
            left_layer_end;
    return mip_overlap && layer_overlap;
}

} // namespace Pelican
