#pragma once

#include <cstdint>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

enum class OutputTargetKind : std::uint32_t {
    window = 0,
    offscreen = 1,
};

enum class OutputEncodingPath : std::uint32_t {
    srgb_hardware = 0,
    srgb_shader_unorm = 1,
};

struct OutputCompileFacts {
    OutputTargetKind target_kind = OutputTargetKind::window;
    vk::Extent2D extent{};
    vk::Format color_format = vk::Format::eUndefined;
    vk::ColorSpaceKHR color_space =
        vk::ColorSpaceKHR::eSrgbNonlinear;
    OutputEncodingPath encoding_path =
        OutputEncodingPath::srgb_hardware;
    vk::ImageUsageFlags selected_usage{};
    bool capture_available = false;
    vk::SurfaceTransformFlagBitsKHR surface_transform =
        vk::SurfaceTransformFlagBitsKHR::eIdentity;
    std::uint32_t graphics_queue_family = 0;
    std::uint32_t presentation_queue_family = 0;

    bool operator==(const OutputCompileFacts &) const = default;
};

struct WsiPresentConfiguration {
    vk::PresentModeKHR present_mode =
        vk::PresentModeKHR::eFifo;
    std::uint32_t image_count = 0;
    vk::CompositeAlphaFlagBitsKHR composite_alpha =
        vk::CompositeAlphaFlagBitsKHR::eOpaque;
    bool clipped = true;

    bool operator==(const WsiPresentConfiguration &) const = default;
};

// Stable little-endian representation. It deliberately excludes
// WsiPresentConfiguration because present cadence does not change target
// lowering or graphics-pipeline compatibility.
std::vector<std::uint8_t> canonicalOutputCompileFacts(
    const OutputCompileFacts &facts);
std::uint64_t outputCompileFactsFingerprint(
    const OutputCompileFacts &facts);

std::string_view outputTargetKindName(
    OutputTargetKind kind) noexcept;
std::string_view outputEncodingPathName(
    OutputEncodingPath path) noexcept;
// The RPC color contract predates the typed compiler vocabulary. Keep its
// concise wire spelling in this display adapter only.
std::string_view outputEncodingPathRpcName(
    OutputEncodingPath path) noexcept;

} // namespace Pelican
