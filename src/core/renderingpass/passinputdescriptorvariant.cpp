#include "passinputdescriptorvariant.hpp"
#include "rendertargetimageviewresolver.hpp"

namespace Pelican {

vk::ImageView passInputCubeImageView(
    const RenderTargetImageViewResolver &rt_views,
    GlobalRenderTargetId target, bool history,
    std::uint32_t parity) {
    return rt_views.getImageSubresourceViewForFrame(
        target,
        ImageSubresourceRange{
            .base_mip_level = 0,
            .level_count =
                rt_views.mipLevels(target),
            .base_array_layer = 0,
            .layer_count = 6,
        },
        ImageSubresourceViewDimension::cube, history,
        parity);
}

PassInputDescriptorWrites::PassInputDescriptorWrites(
    PassInputDescriptorVariant &descriptor_variant,
    std::uint32_t history_parity,
    std::size_t image_capacity,
    std::size_t buffer_capacity)
    : variant{descriptor_variant},
      parity{history_parity} {
    image_infos.reserve(image_capacity);
    buffer_infos.reserve(buffer_capacity);
    pending.reserve(image_capacity + buffer_capacity);
    variant.bound_image_views[parity].reserve(
        image_capacity);
}

void PassInputDescriptorWrites::appendImage(
    std::uint32_t binding,
    vk::DescriptorType descriptor_type,
    vk::Sampler sampler, vk::ImageView image_view) {
    image_infos.push_back(vk::DescriptorImageInfo{
        sampler, image_view,
        descriptor_type ==
                vk::DescriptorType::eInputAttachment
            ? vk::ImageLayout::eRenderingLocalReadKHR
            : vk::ImageLayout::
                  eShaderReadOnlyOptimal});
    variant.bound_image_views[parity].push_back(
        image_view);
    pending.push_back(PendingWrite{
        binding, descriptor_type,
        image_infos.size() - 1, true});
}

void PassInputDescriptorWrites::appendBuffer(
    std::uint32_t binding,
    vk::DescriptorBufferInfo info) {
    buffer_infos.push_back(info);
    pending.push_back(PendingWrite{
        binding, vk::DescriptorType::eStorageBuffer,
        buffer_infos.size() - 1, false});
}

void PassInputDescriptorWrites::submit(
    vk::Device device) const {
    std::vector<vk::WriteDescriptorSet> writes;
    writes.reserve(pending.size());
    for (const auto &entry : pending) {
        vk::WriteDescriptorSet write{
            variant.descsets[parity].get(),
            entry.binding, 0, 1,
            entry.descriptor_type};
        if (entry.image) {
            write.pImageInfo =
                &image_infos[entry.info_index];
        } else {
            write.pBufferInfo =
                &buffer_infos[entry.info_index];
        }
        writes.push_back(write);
    }
    device.updateDescriptorSets(writes, {});
}

} // namespace Pelican
