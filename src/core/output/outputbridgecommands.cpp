#include "outputbridgecommands.hpp"

#include "../vkcore/core.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>

namespace Pelican {
namespace {

constexpr vk::ImageSubresourceRange whole_color{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

#if PELICAN_OUTPUT_TEST_SEAMS
std::mutex submit_record_mutex;
std::vector<OutputBridgeSubmitRecord> &submitRecords() {
    static std::vector<OutputBridgeSubmitRecord> records;
    return records;
}
#endif

} // namespace

OutputBridgeBarrier outputBridgeAcquireBarrier(vk::Image image, std::uint32_t family) {
    // The source scope of an acquire is ignored by the queue family transfer.
    return {vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer,
            vk::ImageMemoryBarrier{
                {}, vk::AccessFlagBits::eTransferWrite,
                vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                VK_QUEUE_FAMILY_EXTERNAL, family, image, whole_color}};
}

OutputBridgeBarrier outputBridgeReleaseBarrier(vk::Image image, std::uint32_t family) {
    // The destination scope of a release is ignored the same way. The write is made
    // available here, and the semaphore signal after it covers it.
    return {vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eBottomOfPipe,
            vk::ImageMemoryBarrier{
                vk::AccessFlagBits::eTransferWrite, {},
                vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eGeneral,
                family, VK_QUEUE_FAMILY_EXTERNAL, image, whole_color}};
}

void recordOutputBridgeAcquire(vk::CommandBuffer command_buffer, vk::Image image,
                               std::uint32_t family) {
    const auto barrier = outputBridgeAcquireBarrier(image, family);
    command_buffer.pipelineBarrier(barrier.source_stage, barrier.destination_stage,
                                   {}, {}, {}, barrier.image);
}

void recordOutputBridgeRelease(vk::CommandBuffer command_buffer, vk::Image image,
                               std::uint32_t family) {
    const auto barrier = outputBridgeReleaseBarrier(image, family);
    command_buffer.pipelineBarrier(barrier.source_stage, barrier.destination_stage,
                                   {}, {}, {}, barrier.image);
}

void recordOutputBridgeCopy(vk::CommandBuffer command_buffer, vk::Image source,
                            const OutputCaptureImageTarget &target, std::uint32_t family) {
    recordOutputBridgeAcquire(command_buffer, target.image, family);
    const vk::ImageSubresourceLayers layer{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    if (target.copy == OutputCaptureCopy::blit) {
        const std::array corners{
            vk::Offset3D{0, 0, 0},
            vk::Offset3D{static_cast<std::int32_t>(target.extent.width),
                         static_cast<std::int32_t>(target.extent.height), 1}};
        const vk::ImageBlit region{layer, corners, layer, corners};
        command_buffer.blitImage(source, vk::ImageLayout::eTransferSrcOptimal, target.image,
                                 vk::ImageLayout::eTransferDstOptimal, region,
                                 vk::Filter::eNearest);
    } else {
        const vk::ImageCopy region{layer, vk::Offset3D{}, layer, vk::Offset3D{},
                                   vk::Extent3D{target.extent.width, target.extent.height, 1}};
        command_buffer.copyImage(source, vk::ImageLayout::eTransferSrcOptimal, target.image,
                                 vk::ImageLayout::eTransferDstOptimal, region);
    }
    recordOutputBridgeRelease(command_buffer, target.image, family);
}

void submitOutputBridgeCommands(vk::Queue queue, vk::CommandBuffer command_buffer,
                                vk::Fence fence, vk::Semaphore semaphore, std::uint64_t value) {
    const vk::TimelineSemaphoreSubmitInfo timeline{{}, value};
    vk::SubmitInfo submit{{}, {}, command_buffer, semaphore};
    submit.pNext = &timeline;
#if PELICAN_OUTPUT_TEST_SEAMS
    {
        std::lock_guard lock{submit_record_mutex};
        submitRecords().push_back(
            {submit.waitSemaphoreCount, submit.signalSemaphoreCount, value});
    }
    // A failure after the barriers are recorded and before anything is submitted.
    if (const char *fault = std::getenv("PELICAN_OUTPUT_SPOUT_GPU_FAULT")) {
        if (std::string_view{fault} == "submit_oom")
            throw vk::OutOfDeviceMemoryError("injected submit failure");
        if (std::string_view{fault} == "submit_invalid")
            throw vk::InitializationFailedError("injected submit failure");
    }
#endif
    queue.submit(submit, fence);
}

OutputBlitVerification verifyOutputRgba8Blit(VulkanManageCore &vulkan) {
    constexpr vk::Format source_format = vk::Format::eR8G8B8A8Unorm;
    constexpr vk::Format destination_format = vk::Format::eB8G8R8A8Unorm;
    const auto physical = vulkan.getPhysDevice();
    if (!(physical.getFormatProperties(source_format).optimalTilingFeatures &
          vk::FormatFeatureFlagBits::eBlitSrc)) {
        return {false, "R8G8B8A8_UNORM has no optimal BLIT_SRC here"};
    }
    if (!(physical.getFormatProperties(destination_format).optimalTilingFeatures &
          vk::FormatFeatureFlagBits::eBlitDst)) {
        return {false, "B8G8R8A8_UNORM has no optimal BLIT_DST here"};
    }

    // Six rows of 256: each channel running through every value with the others
    // held apart, then two rows where all four change at different rates, so a
    // channel crosstalk or a position error can't pass.
    constexpr std::uint32_t width = 256;
    constexpr std::uint32_t height = 6;
    constexpr vk::DeviceSize bytes = width * height * 4;
    std::array<std::uint8_t, bytes> pattern{};
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            auto *pixel = pattern.data() + (y * width + x) * 4;
            const auto value = static_cast<std::uint8_t>(x);
            const auto other = static_cast<std::uint8_t>(255 - x);
            switch (y) {
            case 0: pixel[0] = value; pixel[1] = 17; pixel[2] = 200; pixel[3] = 99; break;
            case 1: pixel[0] = 201; pixel[1] = value; pixel[2] = 18; pixel[3] = 98; break;
            case 2: pixel[0] = 19; pixel[1] = 202; pixel[2] = value; pixel[3] = 97; break;
            case 3: pixel[0] = 96; pixel[1] = 203; pixel[2] = 20; pixel[3] = value; break;
            case 4:
                pixel[0] = value;
                pixel[1] = other;
                pixel[2] = static_cast<std::uint8_t>(x * 3);
                pixel[3] = static_cast<std::uint8_t>(x * 7 + 1);
                break;
            default:
                pixel[0] = static_cast<std::uint8_t>(x * 5);
                pixel[1] = static_cast<std::uint8_t>(x ^ 0x5A);
                pixel[2] = other;
                pixel[3] = value;
                break;
            }
        }
    }

    const auto device = vulkan.getDevice();
    const auto upload = vulkan.allocBuf(
        bytes, vk::BufferUsageFlagBits::eTransferSrc, vma::MemoryUsage::eAutoPreferHost,
        vma::AllocationCreateFlagBits::eMapped |
            vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
    const auto download = vulkan.allocBuf(
        bytes, vk::BufferUsageFlagBits::eTransferDst, vma::MemoryUsage::eAutoPreferHost,
        vma::AllocationCreateFlagBits::eMapped |
            vma::AllocationCreateFlagBits::eHostAccessRandom);
    auto allocator = vulkan.getAllocator();
    std::memcpy(allocator.getAllocationInfo(upload.allocation.get()).pMappedData,
                pattern.data(), pattern.size());
    allocator.flushAllocation(upload.allocation.get(), 0, VK_WHOLE_SIZE);

    const auto make_image = [&](vk::Format format, vk::ImageUsageFlags usage) {
        vk::ImageCreateInfo info;
        info.imageType = vk::ImageType::e2D;
        info.format = format;
        info.extent = vk::Extent3D{width, height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.usage = usage;
        return device.createImageUnique(info);
    };
    const auto bind_image = [&](vk::Image image) {
        const auto requirements = device.getImageMemoryRequirements(image);
        const auto memory = physical.getMemoryProperties();
        std::uint32_t index = 0;
        for (; index < memory.memoryTypeCount; ++index) {
            if ((requirements.memoryTypeBits & (1u << index)) != 0 &&
                (memory.memoryTypes[index].propertyFlags &
                 vk::MemoryPropertyFlagBits::eDeviceLocal)) break;
        }
        auto allocation = device.allocateMemoryUnique({requirements.size, index});
        device.bindImageMemory(image, allocation.get(), 0);
        return allocation;
    };
    auto source = make_image(source_format, vk::ImageUsageFlagBits::eTransferSrc |
                                                vk::ImageUsageFlagBits::eTransferDst);
    auto destination = make_image(destination_format, vk::ImageUsageFlagBits::eTransferSrc |
                                                          vk::ImageUsageFlagBits::eTransferDst);
    const auto source_memory = bind_image(source.get());
    const auto destination_memory = bind_image(destination.get());

    const auto pool = device.createCommandPoolUnique(
        {vk::CommandPoolCreateFlagBits::eTransient, vulkan.getGraphicsQueueFamilyIndex()});
    const auto buffers = device.allocateCommandBuffersUnique(
        {pool.get(), vk::CommandBufferLevel::ePrimary, 1});
    const auto command_buffer = buffers.front().get();
    command_buffer.begin(vk::CommandBufferBeginInfo{
        vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    const auto barrier = [&](vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
                             vk::AccessFlags source_access, vk::AccessFlags destination_access) {
        const vk::ImageMemoryBarrier memory{
            source_access, destination_access, from, to, VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED, image, whole_color};
        command_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                       vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, memory);
    };
    barrier(source.get(), vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, {},
            vk::AccessFlagBits::eTransferWrite);
    barrier(destination.get(), vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
            {}, vk::AccessFlagBits::eTransferWrite);
    const vk::ImageSubresourceLayers layer{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    const vk::BufferImageCopy upload_region{0, 0, 0, layer, vk::Offset3D{},
                                            vk::Extent3D{width, height, 1}};
    command_buffer.copyBufferToImage(upload.buffer.get(), source.get(),
                                     vk::ImageLayout::eTransferDstOptimal, upload_region);
    barrier(source.get(), vk::ImageLayout::eTransferDstOptimal,
            vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits::eTransferWrite,
            vk::AccessFlagBits::eTransferRead);
    const std::array corners{vk::Offset3D{0, 0, 0}, vk::Offset3D{width, height, 1}};
    const vk::ImageBlit region{layer, corners, layer, corners};
    command_buffer.blitImage(source.get(), vk::ImageLayout::eTransferSrcOptimal,
                             destination.get(), vk::ImageLayout::eTransferDstOptimal, region,
                             vk::Filter::eNearest);
    barrier(destination.get(), vk::ImageLayout::eTransferDstOptimal,
            vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits::eTransferWrite,
            vk::AccessFlagBits::eTransferRead);
    command_buffer.copyImageToBuffer(destination.get(), vk::ImageLayout::eTransferSrcOptimal,
                                     download.buffer.get(), upload_region);
    const vk::BufferMemoryBarrier host_read{
        vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eHostRead,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, download.buffer.get(), 0, VK_WHOLE_SIZE};
    command_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                   vk::PipelineStageFlagBits::eHost, {}, {}, host_read, {});
    command_buffer.end();

    const auto fence = device.createFenceUnique({});
    const vk::SubmitInfo submit{{}, {}, command_buffer};
    vulkan.getGraphicsQueue().submit(submit, fence.get());
    // At startup and on Vulkan alone. A hung device reports device lost from here.
    if (device.waitForFences(fence.get(), vk::True, UINT64_MAX) != vk::Result::eSuccess)
        return {false, "the check's blit didn't finish"};
    allocator.invalidateAllocation(download.allocation.get(), 0, VK_WHOLE_SIZE);
    const auto *result = static_cast<const std::uint8_t *>(
        allocator.getAllocationInfo(download.allocation.get()).pMappedData);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto *from = pattern.data() + (y * width + x) * 4;
            const auto *to = result + (y * width + x) * 4;
            if (to[0] != from[2] || to[1] != from[1] || to[2] != from[0] || to[3] != from[3]) {
                return {false, "the blit changed row " + std::to_string(y) + " column " +
                                   std::to_string(x)};
            }
        }
    }
    return {true, {}};
}

#if PELICAN_OUTPUT_TEST_SEAMS
std::vector<OutputBridgeSubmitRecord> takeOutputBridgeSubmitsForTesting() {
    std::lock_guard lock{submit_record_mutex};
    auto records = std::move(submitRecords());
    submitRecords().clear();
    return records;
}
#endif

} // namespace Pelican
