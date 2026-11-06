#pragma once

// VO-5b: the Vulkan side of a bridge: its barriers, its copy and its submit. No
// Windows or D3D11 type appears here.
#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class VulkanManageCore;

enum class OutputCaptureCopy : std::uint8_t {
    // Same size class, so the bytes stay as they are, sRGB included.
    copy,
    // R8G8B8A8_UNORM only, and only where verifyOutputRgba8Blit said so. A blit
    // is a numeric conversion, not a byte swap.
    blit,
};

struct OutputCaptureImageTarget {
    vk::Image image;
    vk::Semaphore semaphore;
    std::uint64_t signal_value = 0;
    vk::Extent2D extent{};
    OutputCaptureCopy copy = OutputCaptureCopy::copy;
};

struct OutputBridgeBarrier {
    vk::PipelineStageFlags source_stage;
    vk::PipelineStageFlags destination_stage;
    vk::ImageMemoryBarrier image;
};
OutputBridgeBarrier outputBridgeAcquireBarrier(vk::Image image, std::uint32_t family);
OutputBridgeBarrier outputBridgeReleaseBarrier(vk::Image image, std::uint32_t family);

// Acquire from VK_QUEUE_FAMILY_EXTERNAL into `family`, to TRANSFER_DST_OPTIMAL. The
// old layout is UNDEFINED on purpose: every byte is overwritten, so the old contents
// are discarded. D3D11's work on them has finished: the bridge was free.
void recordOutputBridgeAcquire(vk::CommandBuffer command_buffer, vk::Image image,
                               std::uint32_t family);
// To GENERAL, the implied layout of a D3D11 KMT image, and release to
// VK_QUEUE_FAMILY_EXTERNAL, with the transfer write made available.
void recordOutputBridgeRelease(vk::CommandBuffer command_buffer, vk::Image image,
                               std::uint32_t family);
// Acquire, copy or blit the whole image from TRANSFER_SRC_OPTIMAL, release.
void recordOutputBridgeCopy(vk::CommandBuffer command_buffer, vk::Image source,
                            const OutputCaptureImageTarget &target, std::uint32_t family);
// Signals `semaphore` at `value`, and `fence`. It never waits on a semaphore, so
// nothing outside Vulkan can stall the queue through this submit.
void submitOutputBridgeCommands(vk::Queue queue, vk::CommandBuffer command_buffer,
                                vk::Fence fence, vk::Semaphore semaphore, std::uint64_t value);

// Does a blit from R8G8B8A8_UNORM to B8G8R8A8_UNORM keep all 256 values of every
// channel on this device, alpha below opaque and spatial patterns included? Runs once,
// at startup, on the thread that owns the graphics queue, before rendering begins.
struct OutputBlitVerification {
    bool exact = false;
    std::string detail;
};
OutputBlitVerification verifyOutputRgba8Blit(VulkanManageCore &vulkan);

#if PELICAN_OUTPUT_TEST_SEAMS
struct OutputBridgeSubmitRecord {
    std::uint32_t wait_semaphores = 0;
    std::uint32_t signal_semaphores = 0;
    std::uint64_t signal_value = 0;
};
std::vector<OutputBridgeSubmitRecord> takeOutputBridgeSubmitsForTesting();
#endif

} // namespace Pelican
