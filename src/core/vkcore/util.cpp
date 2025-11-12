#include "util.hpp"
#include "core.hpp"

namespace Pelican {

template <class T, size_t N> static std::array<T, N> vectorToArray(std::vector<T> v) {
    std::array<T, N> arr;
    assert(N == v.size());
    for (int i = 0; i < N; i++) {
        arr[i] = std::move(v[i]);
    }
    return arr;
}

VulkanUtils::VulkanUtils(DependencyContainer &_con)
    : con{_con}, device{con.get<VulkanManageCore>().getDevice()},
      genpurpose_cmd_bufs{vectorToArray<CommandBufWrapper, 8>(con.get<VulkanManageCore>().allocCmdBufs(8))},
      genpurpose_cmd_bufs_index{0} {}

static void changeImageLayoutCommand(vk::CommandBuffer cmd_buf, const ImageWrapper &image, vk::ImageLayout old_layout,
                                     vk::ImageLayout new_layout, const VulkanUtils::ChangeImageLayoutInfo &info) {
    vk::ImageMemoryBarrier barrior;
    barrior.oldLayout = old_layout;
    barrior.newLayout = new_layout;
    barrior.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrior.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrior.image = image.image.get();
    barrior.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    barrior.subresourceRange.baseMipLevel = 0;
    barrior.subresourceRange.levelCount = 1;
    barrior.subresourceRange.baseArrayLayer = 0;
    barrior.subresourceRange.layerCount = 1;
    barrior.srcAccessMask = info.src_access;
    barrior.dstAccessMask = info.dst_access;
    cmd_buf.pipelineBarrier(info.src_stage, info.dst_stage, {}, {}, {}, {barrior});
}

void VulkanUtils::changeImageLayout(const ImageWrapper &image, vk::ImageLayout old_layout, vk::ImageLayout new_layout,
                                    const ChangeImageLayoutInfo &info) {
    executeOneTimeCmd(
        [&](vk::CommandBuffer cmd_buf) { changeImageLayoutCommand(cmd_buf, image, old_layout, new_layout, info); });
}

void VulkanUtils::safeTransferMemoryToImage(const ImageWrapper &image, const void *src, vk::DeviceSize bytes_num,
                                            const ImageTransferInfo &info) {
    const auto &vkcore = con.get<VulkanManageCore>();

    const auto &staging_buf =
        vkcore.allocBuf(bytes_num, vk::BufferUsageFlagBits::eTransferSrc, vma::MemoryUsage::eAutoPreferHost,
                        vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
    vkcore.writeBuf(staging_buf, src, 0, bytes_num);

    vk::BufferImageCopy image_copy;
    image_copy.bufferOffset = 0;
    image_copy.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    image_copy.imageSubresource.mipLevel = 0;
    image_copy.imageSubresource.baseArrayLayer = 0;
    image_copy.imageSubresource.layerCount = 1;
    image_copy.imageOffset = vk::Offset3D{0, 0, 0};
    image_copy.imageExtent = image.extent;

    executeOneTimeCmd([&](vk::CommandBuffer cmd_buf) {
        changeImageLayoutCommand(cmd_buf, image, info.old_layout, vk::ImageLayout::eTransferDstOptimal,
                                 ChangeImageLayoutInfo{
                                     .src_stage = vk::PipelineStageFlagBits::eTopOfPipe,
                                     .dst_stage = vk::PipelineStageFlagBits::eTransfer,
                                     .src_access = {},
                                     .dst_access = vk::AccessFlagBits::eTransferWrite,
                                 });
        cmd_buf.copyBufferToImage(staging_buf.buffer.get(), image.image.get(), vk::ImageLayout::eTransferDstOptimal,
                                  {image_copy});
        changeImageLayoutCommand(cmd_buf, image, vk::ImageLayout::eTransferDstOptimal, info.new_layout,
                                 ChangeImageLayoutInfo{
                                     .src_stage = vk::PipelineStageFlagBits::eTransfer,
                                     .dst_stage = info.dst_stage,
                                     .src_access = vk::AccessFlagBits::eTransferWrite,
                                     .dst_access = info.dst_access,
                                 });
    });
}

} // namespace Pelican