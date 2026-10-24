#pragma once

#include "../../project/attachmentoperations.hpp"
#include "../../project/imageformat.hpp"
#include "../../project/imageusage.hpp"
#include "../../project/resourceextent.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vulkan/vulkan.hpp>

namespace Pelican {

// RC-3a1: the one place the engine's image vocabulary (ImageFormat,
// ImageUsage, the extent) becomes Vulkan's, and the way back for what
// Vulkan hands the engine (the swapchain's format, a device's image).

vk::Format toVulkanFormat(ImageFormat format);
// The ImageFormat of a Vulkan format; nullopt for one the table does not
// list.
std::optional<ImageFormat> imageFormatFromVulkan(vk::Format format);
// imageFormatFromVulkan, throwing for a format the table does not list,
// naming `what` and the format.
ImageFormat requireImageFormatFromVulkan(vk::Format format,
                                         std::string_view what);

// A Vulkan format's name for a message or a status: the table's (the
// author's spelling), or vk::to_string's for a format the table lacks.
std::string vulkanFormatName(vk::Format format);

vk::ImageUsageFlags toVulkanImageUsage(ImageUsage usage);
// The ImageUsage bits of Vulkan's usage flags (other bits are dropped).
ImageUsage imageUsageFromVulkan(vk::ImageUsageFlags usage);

vk::Extent2D toVulkanExtent(ResolvedResourceExtent extent);
ResolvedResourceExtent extentFromVulkan(vk::Extent2D extent);

// RC-3a2: an attachment's load / store op, where a rendering scope is
// recorded (the only way they reach Vulkan).
vk::AttachmentLoadOp toVulkanAttachmentLoadOp(AttachmentLoadOp op);
vk::AttachmentStoreOp toVulkanAttachmentStoreOp(AttachmentStoreOp op);

} // namespace Pelican
