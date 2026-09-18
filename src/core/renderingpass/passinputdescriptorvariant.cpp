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

vk::ImageView resolvePassInputImageView(
    const ResolvedPassInput &input,
    const RenderTargetImageViewResolver &rt_views) {
    if (input.subresource) {
        auto subresource = *input.subresource;
        if (input.layer_binding ==
            PassInputLayerBinding::per_variant) {
            subresource.base_array_layer +=
                input.variant;
            subresource.layer_count = 1;
        }
        return rt_views
            .getImageSubresourceViewForFrame(
                input.target, subresource,
                input.descriptor_dimension,
                input.history, input.parity);
    }
    if (input.descriptor_dimension ==
        ImageSubresourceViewDimension::cube) {
        return passInputCubeImageView(
            rt_views, input.target, input.history,
            input.parity);
    }
    // The producer family is one array descriptor regardless of how the
    // physical target is laid out, so this dispatches on the declared view
    // and not on the descriptor dimension derived from it.
    if (input.view ==
        PassInputViewDimension::family_2d_array) {
        return rt_views.getLayeredImageViewForFrame(
            input.target, input.history, input.parity);
    }
    if (input.logical_view_count &&
        input.view ==
            PassInputViewDimension::layered_2d_array) {
        return rt_views.arrayLayers(input.target) ==
                       *input.logical_view_count
                   ? rt_views
                         .getLayeredImageViewForFrame(
                             input.target,
                             input.history,
                             input.parity)
                   : rt_views
                         .getImageSubresourceViewForFrame(
                             input.target,
                             ImageSubresourceRange{
                                 .layer_count =
                                     *input
                                          .logical_view_count,
                             },
                             ImageSubresourceViewDimension::
                                 two_d_array,
                             input.history,
                             input.parity);
    }
    if (input.layer_binding ==
        PassInputLayerBinding::per_variant) {
        return rt_views.getImageLayerViewForFrame(
            input.target, input.variant,
            input.history, input.parity);
    }
    if (input.layer_binding ==
        PassInputLayerBinding::whole_array) {
        return rt_views.getLayeredImageViewForFrame(
            input.target, input.history, input.parity);
    }
    return rt_views.getImageViewForFrame(
        input.target, input.history, input.parity);
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
