#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// RC-3a1: what an image is used for, in the engine's own terms (core maps it
// to Vulkan's usage flags in vulkanimagevocabulary). A render target
// declaration names the authorable bits; the engine adds the others.
enum class ImageUsageBit : std::uint32_t {
    color_attachment = 1u << 0,
    depth_stencil_attachment = 1u << 1,
    sampled = 1u << 2,
    storage = 1u << 3,
    transfer_dst = 1u << 4,
    transfer_src = 1u << 5,
    // The engine's own: a tile-local attachment.
    input_attachment = 1u << 6,
    transient_attachment = 1u << 7,
};

// A set of ImageUsageBit, used as Vulkan's flags are.
class ImageUsage {
  public:
    constexpr ImageUsage() noexcept = default;
    constexpr ImageUsage(ImageUsageBit bit) noexcept // NOLINT(google-explicit-constructor)
        : bits_{static_cast<std::uint32_t>(bit)} {}
    static constexpr ImageUsage fromBits(std::uint32_t bits) noexcept {
        ImageUsage usage;
        usage.bits_ = bits & kAllBits;
        return usage;
    }

    constexpr std::uint32_t bits() const noexcept { return bits_; }
    constexpr explicit operator bool() const noexcept { return bits_ != 0; }
    constexpr bool operator!() const noexcept { return bits_ == 0; }
    constexpr bool contains(ImageUsage other) const noexcept {
        return (bits_ & other.bits_) == other.bits_;
    }

    constexpr ImageUsage operator|(ImageUsage other) const noexcept {
        return fromBits(bits_ | other.bits_);
    }
    constexpr ImageUsage operator&(ImageUsage other) const noexcept {
        return fromBits(bits_ & other.bits_);
    }
    constexpr ImageUsage operator^(ImageUsage other) const noexcept {
        return fromBits(bits_ ^ other.bits_);
    }
    constexpr ImageUsage operator~() const noexcept { return fromBits(~bits_); }
    constexpr ImageUsage &operator|=(ImageUsage other) noexcept {
        bits_ = (bits_ | other.bits_) & kAllBits;
        return *this;
    }
    constexpr ImageUsage &operator&=(ImageUsage other) noexcept {
        bits_ &= other.bits_;
        return *this;
    }
    constexpr ImageUsage &operator^=(ImageUsage other) noexcept {
        bits_ = (bits_ ^ other.bits_) & kAllBits;
        return *this;
    }
    constexpr bool operator==(const ImageUsage &) const noexcept = default;

  private:
    static constexpr std::uint32_t kAllBits = (1u << 8) - 1;
    std::uint32_t bits_ = 0;
};

constexpr ImageUsage operator|(ImageUsageBit left, ImageUsageBit right) noexcept {
    return ImageUsage{left} | ImageUsage{right};
}
constexpr ImageUsage operator&(ImageUsageBit left, ImageUsageBit right) noexcept {
    return ImageUsage{left} & ImageUsage{right};
}
constexpr ImageUsage operator~(ImageUsageBit bit) noexcept {
    return ~ImageUsage{bit};
}

struct ImageUsageInfo {
    ImageUsageBit bit;
    // The author's spelling ("COLOR_ATTACHMENT").
    std::string_view name;
    // Whether a render target declaration may name it.
    bool authorable = true;
};

// The one table of usage names: the authorable bits first, in the order the
// render target parser has always listed them (Studio's usage choices).
std::span<const ImageUsageInfo> imageUsageTable();
std::string_view imageUsageBitName(ImageUsageBit bit);
// The names of the bits `usage` holds, in table order.
std::vector<std::string> imageUsageNames(ImageUsage usage);
// The names a render target declaration may give, in table order.
std::vector<std::string> authoredImageUsageNames();
// A render target declaration's usage: throws for an empty list
// ("Image usage flags must not be empty") or a name the table does not list
// as authorable ("Unknown image usage flag: <name>").
ImageUsage parseAuthoredImageUsage(std::span<const std::string> names);

} // namespace Pelican
