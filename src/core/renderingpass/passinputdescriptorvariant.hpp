#pragma once

#include "renderingpass.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class RenderTargetImageViewResolver;

// One descriptor set per history parity for one logical view of a pass
// input set. Material screen inputs and fullscreen/raster pass inputs
// resolve their image views by different rules, but the record they
// produce is the same.
struct PassInputDescriptorVariant {
    std::array<vk::UniqueDescriptorSet, 2> descsets;
    std::array<std::vector<vk::ImageView>, 2>
        bound_image_views;
};

// The cube view of a pass input: all six faces, every mip level. Both
// binding paths reach this from their own branch with the same arguments.
vk::ImageView passInputCubeImageView(
    const RenderTargetImageViewResolver &rt_views,
    GlobalRenderTargetId target, bool history,
    std::uint32_t parity);

// Accumulates one descriptor set's writes. The caller resolves the image
// view, the sampler and the descriptor type; everything after that is the
// same on both binding paths. The pointers vk::WriteDescriptorSet carries
// are taken at submit() time, so appending past the reserved capacity
// stays correct instead of silently writing through a dangling info.
class PassInputDescriptorWrites {
  public:
    PassInputDescriptorWrites(
        PassInputDescriptorVariant &descriptor_variant,
        std::uint32_t history_parity,
        std::size_t image_capacity,
        std::size_t buffer_capacity);

    // The image layout follows the descriptor type: an input attachment is
    // read through the local-read layout, anything else is sampled.
    void appendImage(std::uint32_t binding,
                     vk::DescriptorType descriptor_type,
                     vk::Sampler sampler,
                     vk::ImageView image_view);
    void appendBuffer(std::uint32_t binding,
                      vk::DescriptorBufferInfo info);
    void submit(vk::Device device) const;

  private:
    struct PendingWrite {
        std::uint32_t binding = 0;
        vk::DescriptorType descriptor_type =
            vk::DescriptorType::eCombinedImageSampler;
        std::size_t info_index = 0;
        bool image = false;
    };

    PassInputDescriptorVariant &variant;
    std::uint32_t parity;
    std::vector<vk::DescriptorImageInfo> image_infos;
    std::vector<vk::DescriptorBufferInfo> buffer_infos;
    std::vector<PendingWrite> pending;
};

} // namespace Pelican
