#include "vulkanimagevocabulary.hpp"

#include <array>
#include <stdexcept>
#include <string>
#include <utility>

namespace Pelican {

namespace {

// In ImageFormat's order (one row each; static_assert below).
constexpr std::array kVulkanFormats{
    std::pair{ImageFormat::r8_unorm, vk::Format::eR8Unorm},
    std::pair{ImageFormat::r8_snorm, vk::Format::eR8Snorm},
    std::pair{ImageFormat::r8_uint, vk::Format::eR8Uint},
    std::pair{ImageFormat::r8_sint, vk::Format::eR8Sint},
    std::pair{ImageFormat::r8g8_unorm, vk::Format::eR8G8Unorm},
    std::pair{ImageFormat::r8g8_snorm, vk::Format::eR8G8Snorm},
    std::pair{ImageFormat::r8g8_uint, vk::Format::eR8G8Uint},
    std::pair{ImageFormat::r8g8_sint, vk::Format::eR8G8Sint},
    std::pair{ImageFormat::r8g8b8a8_unorm, vk::Format::eR8G8B8A8Unorm},
    std::pair{ImageFormat::r8g8b8a8_snorm, vk::Format::eR8G8B8A8Snorm},
    std::pair{ImageFormat::r8g8b8a8_uint, vk::Format::eR8G8B8A8Uint},
    std::pair{ImageFormat::r8g8b8a8_sint, vk::Format::eR8G8B8A8Sint},
    std::pair{ImageFormat::r8g8b8a8_srgb, vk::Format::eR8G8B8A8Srgb},
    std::pair{ImageFormat::b8g8r8a8_unorm, vk::Format::eB8G8R8A8Unorm},
    std::pair{ImageFormat::b8g8r8a8_srgb, vk::Format::eB8G8R8A8Srgb},
    std::pair{ImageFormat::a2b10g10r10_unorm_pack32, vk::Format::eA2B10G10R10UnormPack32},
    std::pair{ImageFormat::a2r10g10b10_unorm_pack32, vk::Format::eA2R10G10B10UnormPack32},
    std::pair{ImageFormat::b10g11r11_ufloat_pack32, vk::Format::eB10G11R11UfloatPack32},
    std::pair{ImageFormat::r16_unorm, vk::Format::eR16Unorm},
    std::pair{ImageFormat::r16_snorm, vk::Format::eR16Snorm},
    std::pair{ImageFormat::r16_uint, vk::Format::eR16Uint},
    std::pair{ImageFormat::r16_sint, vk::Format::eR16Sint},
    std::pair{ImageFormat::r16_sfloat, vk::Format::eR16Sfloat},
    std::pair{ImageFormat::r16g16_unorm, vk::Format::eR16G16Unorm},
    std::pair{ImageFormat::r16g16_snorm, vk::Format::eR16G16Snorm},
    std::pair{ImageFormat::r16g16_uint, vk::Format::eR16G16Uint},
    std::pair{ImageFormat::r16g16_sint, vk::Format::eR16G16Sint},
    std::pair{ImageFormat::r16g16_sfloat, vk::Format::eR16G16Sfloat},
    std::pair{ImageFormat::r16g16b16a16_unorm, vk::Format::eR16G16B16A16Unorm},
    std::pair{ImageFormat::r16g16b16a16_snorm, vk::Format::eR16G16B16A16Snorm},
    std::pair{ImageFormat::r16g16b16a16_uint, vk::Format::eR16G16B16A16Uint},
    std::pair{ImageFormat::r16g16b16a16_sint, vk::Format::eR16G16B16A16Sint},
    std::pair{ImageFormat::r16g16b16a16_sfloat, vk::Format::eR16G16B16A16Sfloat},
    std::pair{ImageFormat::r32_uint, vk::Format::eR32Uint},
    std::pair{ImageFormat::r32_sint, vk::Format::eR32Sint},
    std::pair{ImageFormat::r32_sfloat, vk::Format::eR32Sfloat},
    std::pair{ImageFormat::r32g32_uint, vk::Format::eR32G32Uint},
    std::pair{ImageFormat::r32g32_sint, vk::Format::eR32G32Sint},
    std::pair{ImageFormat::r32g32_sfloat, vk::Format::eR32G32Sfloat},
    std::pair{ImageFormat::r32g32b32a32_uint, vk::Format::eR32G32B32A32Uint},
    std::pair{ImageFormat::r32g32b32a32_sint, vk::Format::eR32G32B32A32Sint},
    std::pair{ImageFormat::r32g32b32a32_sfloat, vk::Format::eR32G32B32A32Sfloat},
    std::pair{ImageFormat::d16_unorm, vk::Format::eD16Unorm},
    std::pair{ImageFormat::d24_unorm_s8_uint, vk::Format::eD24UnormS8Uint},
    std::pair{ImageFormat::d32_sfloat, vk::Format::eD32Sfloat},
    std::pair{ImageFormat::d32_sfloat_s8_uint, vk::Format::eD32SfloatS8Uint},
};

constexpr bool vulkanFormatsFollowEnum() {
    for (std::size_t index = 0; index < kVulkanFormats.size(); ++index) {
        if (static_cast<std::size_t>(kVulkanFormats[index].first) != index) return false;
    }
    return true;
}
static_assert(vulkanFormatsFollowEnum(), "kVulkanFormats rows must follow ImageFormat's order");
static_assert(static_cast<std::size_t>(ImageFormat::d32_sfloat_s8_uint) + 1 ==
              kVulkanFormats.size());

constexpr std::array kVulkanUsages{
    std::pair{ImageUsageBit::color_attachment, vk::ImageUsageFlagBits::eColorAttachment},
    std::pair{ImageUsageBit::depth_stencil_attachment,
              vk::ImageUsageFlagBits::eDepthStencilAttachment},
    std::pair{ImageUsageBit::sampled, vk::ImageUsageFlagBits::eSampled},
    std::pair{ImageUsageBit::storage, vk::ImageUsageFlagBits::eStorage},
    std::pair{ImageUsageBit::transfer_dst, vk::ImageUsageFlagBits::eTransferDst},
    std::pair{ImageUsageBit::transfer_src, vk::ImageUsageFlagBits::eTransferSrc},
    std::pair{ImageUsageBit::input_attachment, vk::ImageUsageFlagBits::eInputAttachment},
    std::pair{ImageUsageBit::transient_attachment,
              vk::ImageUsageFlagBits::eTransientAttachment},
};

} // namespace

vk::Format toVulkanFormat(ImageFormat format) {
    const auto index = static_cast<std::size_t>(format);
    if (index >= kVulkanFormats.size()) throw std::out_of_range("unknown image format");
    return kVulkanFormats[index].second;
}

std::optional<ImageFormat> imageFormatFromVulkan(vk::Format format) {
    for (const auto &[image_format, vulkan] : kVulkanFormats) {
        if (vulkan == format) return image_format;
    }
    return std::nullopt;
}

ImageFormat requireImageFormatFromVulkan(vk::Format format, std::string_view what) {
    if (const auto found = imageFormatFromVulkan(format)) return *found;
    throw std::runtime_error(std::string{what} + " has a format the engine does not list: " +
                             vk::to_string(format));
}

std::string vulkanFormatName(vk::Format format) {
    if (const auto found = imageFormatFromVulkan(format)) {
        return std::string{imageFormatName(*found)};
    }
    return vk::to_string(format);
}

vk::ImageUsageFlags toVulkanImageUsage(ImageUsage usage) {
    vk::ImageUsageFlags flags;
    for (const auto &[bit, vulkan] : kVulkanUsages) {
        if (usage & bit) flags |= vulkan;
    }
    return flags;
}

ImageUsage imageUsageFromVulkan(vk::ImageUsageFlags flags) {
    ImageUsage usage;
    for (const auto &[bit, vulkan] : kVulkanUsages) {
        if (flags & vulkan) usage |= bit;
    }
    return usage;
}

vk::Extent2D toVulkanExtent(ResolvedResourceExtent extent) {
    return vk::Extent2D{extent.width, extent.height};
}

ResolvedResourceExtent extentFromVulkan(vk::Extent2D extent) {
    return ResolvedResourceExtent{extent.width, extent.height};
}

} // namespace Pelican
