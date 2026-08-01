#include "util.hpp"
#include "core.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace Pelican {

template <class T, size_t N> static std::array<T, N> vectorToArray(std::vector<T> v) {
    std::array<T, N> arr;
    assert(N == v.size());
    for (int i = 0; i < N; i++) {
        arr[i] = std::move(v[i]);
    }
    return arr;
}

VulkanUtils::VulkanUtils()
    : device{GET_MODULE(VulkanManageCore).getDevice()},
      genpurpose_cmd_bufs{vectorToArray<CommandBufWrapper, 8>(GET_MODULE(VulkanManageCore).allocCmdBufs(8))},
      genpurpose_cmd_bufs_index{0} {}

static vk::ImageAspectFlags aspectMaskForFormat(vk::Format format) {
    switch (format) {
    case vk::Format::eD16Unorm:
    case vk::Format::eD32Sfloat:
        return vk::ImageAspectFlagBits::eDepth;
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32SfloatS8Uint:
        return vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
    default:
        return vk::ImageAspectFlagBits::eColor;
    }
}

static void changeImageLayoutCommand(
    vk::CommandBuffer cmd_buf, const ImageWrapper &image,
    vk::ImageLayout old_layout, vk::ImageLayout new_layout,
    vk::ImageSubresourceRange subresource_range,
    const VulkanUtils::ChangeImageLayoutInfo &info) {
    vk::ImageMemoryBarrier barrier;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image.get();
    barrier.subresourceRange = subresource_range;
    barrier.srcAccessMask = info.src_access;
    barrier.dstAccessMask = info.dst_access;
    cmd_buf.pipelineBarrier(
        info.src_stage, info.dst_stage, {}, {}, {}, {barrier});
}

static vk::ImageSubresourceRange fullImageRange(
    const ImageWrapper &image) {
    return vk::ImageSubresourceRange{
        aspectMaskForFormat(image.format), 0, image.mip_levels,
        0, image.array_layers};
}

static void changeImageLayoutCommand(
    vk::CommandBuffer cmd_buf, const ImageWrapper &image,
    vk::ImageLayout old_layout, vk::ImageLayout new_layout,
    const VulkanUtils::ChangeImageLayoutInfo &info) {
    changeImageLayoutCommand(
        cmd_buf, image, old_layout, new_layout,
        fullImageRange(image), info);
}

void VulkanUtils::changeImageLayoutCmd(vk::CommandBuffer cmd_buf, const ImageWrapper &image, vk::ImageLayout old_layout,
                                       vk::ImageLayout new_layout, const ChangeImageLayoutInfo &info) {
    changeImageLayoutCommand(cmd_buf, image, old_layout, new_layout, info);
}

void VulkanUtils::changeImageLayout(const ImageWrapper &image, vk::ImageLayout old_layout, vk::ImageLayout new_layout,
                                    const ChangeImageLayoutInfo &info) {
    executeOneTimeCmd(
        [&](vk::CommandBuffer cmd_buf) { changeImageLayoutCommand(cmd_buf, image, old_layout, new_layout, info); });
}

void VulkanUtils::safeTransferMemoryToImage(const ImageWrapper &image, const void *src, vk::DeviceSize bytes_num,
                                            const ImageTransferInfo &info) {
    if (image.array_layers != 1) {
        throw std::invalid_argument(
            "safeTransferMemoryToImage only accepts single-layer images; "
            "use safeTransferMemoryToImageLevels with explicit array-layer regions");
    }
    vk::BufferImageCopy image_copy;
    image_copy.bufferOffset = 0;
    image_copy.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    image_copy.imageSubresource.mipLevel = 0;
    image_copy.imageSubresource.baseArrayLayer = 0;
    image_copy.imageSubresource.layerCount = 1;
    image_copy.imageOffset = vk::Offset3D{0, 0, 0};
    image_copy.imageExtent = image.extent;
    safeTransferMemoryToImageLevels(image, src, bytes_num, std::span<const vk::BufferImageCopy>{&image_copy, 1}, info);
}

void VulkanUtils::safeTransferMemoryToImageLevels(const ImageWrapper &image, const void *src,
                                                  vk::DeviceSize bytes_num,
                                                  std::span<const vk::BufferImageCopy> regions,
                                                  const ImageTransferInfo &info) {
    if (regions.empty()) throw std::runtime_error("Image transfer requires at least one mip region");
    const auto &vkcore = GET_MODULE(VulkanManageCore);

    const auto &staging_buf =
        vkcore.allocBuf(bytes_num, vk::BufferUsageFlagBits::eTransferSrc, vma::MemoryUsage::eAutoPreferHost,
                        vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
    vkcore.writeBuf(staging_buf, src, 0, bytes_num);

    executeOneTimeCmd(
        [&](vk::CommandBuffer cmd_buf) {
            changeImageLayoutCommand(cmd_buf, image, info.old_layout, vk::ImageLayout::eTransferDstOptimal,
                                     ChangeImageLayoutInfo{
                                         .src_stage = vk::PipelineStageFlagBits::eTopOfPipe,
                                         .dst_stage = vk::PipelineStageFlagBits::eTransfer,
                                         .src_access = {},
                                         .dst_access = vk::AccessFlagBits::eTransferWrite,
                                     });
            cmd_buf.copyBufferToImage(staging_buf.buffer.get(), image.image.get(), vk::ImageLayout::eTransferDstOptimal,
                                      regions);
            changeImageLayoutCommand(cmd_buf, image, vk::ImageLayout::eTransferDstOptimal, info.new_layout,
                                     ChangeImageLayoutInfo{
                                         .src_stage = vk::PipelineStageFlagBits::eTransfer,
                                         .dst_stage = info.dst_stage,
                                         .src_access = vk::AccessFlagBits::eTransferWrite,
                                         .dst_access = info.dst_access,
                                     });
        },
        true);
}

void VulkanUtils::requireLinearBlitSupport(
    vk::Format format,
    vk::FormatFeatureFlags optimal_tiling_features) {
    const auto required =
        vk::FormatFeatureFlagBits::eBlitSrc |
        vk::FormatFeatureFlagBits::eBlitDst |
        vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
    if ((optimal_tiling_features & required) == required) {
        return;
    }
    throw std::runtime_error(
        "GPU mip generation for format " + vk::to_string(format) +
        " lacks required optimal-tiling linear blit features "
        "(BLIT_SRC, BLIT_DST, SAMPLED_IMAGE_FILTER_LINEAR)");
}

void VulkanUtils::safeTransferMemoryToImageAndGenerateMipmaps(
    const ImageWrapper &image, const void *src,
    vk::DeviceSize bytes_num,
    const ImageTransferInfo &info) {
    if (image.mip_levels <= 1) {
        safeTransferMemoryToImage(image, src, bytes_num, info);
        return;
    }
    if (image.image_type != vk::ImageType::e2D ||
        image.extent.depth != 1 || image.array_layers != 1) {
        throw std::invalid_argument(
            "GPU mip generation only accepts single-layer 2D images");
    }
    const auto required_usage =
        vk::ImageUsageFlagBits::eTransferSrc |
        vk::ImageUsageFlagBits::eTransferDst;
    if ((image.usage & required_usage) != required_usage) {
        throw std::invalid_argument(
            "GPU mip generation requires transfer-src and transfer-dst "
            "image usage");
    }

    const auto &vkcore = GET_MODULE(VulkanManageCore);
    requireLinearBlitSupport(
        image.format,
        vkcore.getPhysDevice()
            .getFormatProperties(image.format)
            .optimalTilingFeatures);

    const auto &staging_buf = vkcore.allocBuf(
        bytes_num, vk::BufferUsageFlagBits::eTransferSrc,
        vma::MemoryUsage::eAutoPreferHost,
        vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
    vkcore.writeBuf(staging_buf, src, 0, bytes_num);

    executeOneTimeCmd(
        [&](vk::CommandBuffer cmd_buf) {
            changeImageLayoutCommand(
                cmd_buf, image, info.old_layout,
                vk::ImageLayout::eTransferDstOptimal,
                ChangeImageLayoutInfo{
                    .src_stage = vk::PipelineStageFlagBits::eTopOfPipe,
                    .dst_stage = vk::PipelineStageFlagBits::eTransfer,
                    .src_access = {},
                    .dst_access = vk::AccessFlagBits::eTransferWrite,
                });

            vk::BufferImageCopy base_copy;
            base_copy.imageSubresource = {
                vk::ImageAspectFlagBits::eColor, 0, 0, 1};
            base_copy.imageExtent = image.extent;
            cmd_buf.copyBufferToImage(
                staging_buf.buffer.get(), image.image.get(),
                vk::ImageLayout::eTransferDstOptimal, {base_copy});

            auto source_width = image.extent.width;
            auto source_height = image.extent.height;
            for (std::uint32_t mip = 1;
                 mip < image.mip_levels; ++mip) {
                const vk::ImageSubresourceRange source_range{
                    vk::ImageAspectFlagBits::eColor, mip - 1, 1,
                    0, 1};
                changeImageLayoutCommand(
                    cmd_buf, image,
                    vk::ImageLayout::eTransferDstOptimal,
                    vk::ImageLayout::eTransferSrcOptimal,
                    source_range,
                    ChangeImageLayoutInfo{
                        .src_stage =
                            vk::PipelineStageFlagBits::eTransfer,
                        .dst_stage =
                            vk::PipelineStageFlagBits::eTransfer,
                        .src_access =
                            vk::AccessFlagBits::eTransferWrite,
                        .dst_access =
                            vk::AccessFlagBits::eTransferRead,
                    });

                const auto destination_width =
                    std::max(1u, source_width / 2);
                const auto destination_height =
                    std::max(1u, source_height / 2);
                vk::ImageBlit blit;
                blit.srcSubresource = {
                    vk::ImageAspectFlagBits::eColor, mip - 1, 0, 1};
                blit.srcOffsets[1] = vk::Offset3D{
                    static_cast<std::int32_t>(source_width),
                    static_cast<std::int32_t>(source_height), 1};
                blit.dstSubresource = {
                    vk::ImageAspectFlagBits::eColor, mip, 0, 1};
                blit.dstOffsets[1] = vk::Offset3D{
                    static_cast<std::int32_t>(destination_width),
                    static_cast<std::int32_t>(destination_height), 1};
                cmd_buf.blitImage(
                    image.image.get(),
                    vk::ImageLayout::eTransferSrcOptimal,
                    image.image.get(),
                    vk::ImageLayout::eTransferDstOptimal,
                    {blit}, vk::Filter::eLinear);
                source_width = destination_width;
                source_height = destination_height;
            }

            const vk::ImageSubresourceRange generated_sources{
                vk::ImageAspectFlagBits::eColor, 0,
                image.mip_levels - 1, 0, 1};
            changeImageLayoutCommand(
                cmd_buf, image,
                vk::ImageLayout::eTransferSrcOptimal,
                info.new_layout, generated_sources,
                ChangeImageLayoutInfo{
                    .src_stage = vk::PipelineStageFlagBits::eTransfer,
                    .dst_stage = info.dst_stage,
                    .src_access = vk::AccessFlagBits::eTransferRead,
                    .dst_access = info.dst_access,
                });
            const vk::ImageSubresourceRange final_mip{
                vk::ImageAspectFlagBits::eColor,
                image.mip_levels - 1, 1, 0, 1};
            changeImageLayoutCommand(
                cmd_buf, image,
                vk::ImageLayout::eTransferDstOptimal,
                info.new_layout, final_mip,
                ChangeImageLayoutInfo{
                    .src_stage = vk::PipelineStageFlagBits::eTransfer,
                    .dst_stage = info.dst_stage,
                    .src_access = vk::AccessFlagBits::eTransferWrite,
                    .dst_access = info.dst_access,
                });
        },
        true);
}

void VulkanUtils::bufferCopy(const BufferWrapper &src, const BufferWrapper &dst, vk::DeviceSize src_offset,
                             vk::DeviceSize dst_offset, vk::DeviceSize bytes_num) {
    vk::BufferCopy copy_info;
    copy_info.size = bytes_num;
    copy_info.srcOffset = src_offset;
    copy_info.dstOffset = dst_offset;
    executeOneTimeCmd(
        [&](vk::CommandBuffer cmd_buf) { cmd_buf.copyBuffer(src.buffer.get(), dst.buffer.get(), {copy_info}); }, true);
}

} // namespace Pelican
