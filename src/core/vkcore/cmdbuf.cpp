#include "cmdbuf.hpp"

#include <stdexcept>
#include <string>

namespace Pelican {

CommandBufWrapper::CommandBufWrapper(vk::Device _device, vk::Queue _queue, vk::UniqueCommandBuffer &&_cmd_buf,
                                     vk::UniqueFence &&_fence)
    : device{_device}, queue{_queue}, cmd_buf{std::move(_cmd_buf)}, fence{std::move(_fence)} {}
void CommandBufWrapper::recordBegin() const {
    const auto wait_result =
        device.waitForFences({fence.get()}, VK_TRUE, UINT64_MAX);
    if (wait_result != vk::Result::eSuccess) {
        throw std::runtime_error(
            "failed to wait for command-buffer submission fence: " +
            vk::to_string(wait_result));
    }
    cmd_buf->reset();

    vk::CommandBufferBeginInfo begin_info;
    begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    cmd_buf->begin(begin_info);
}
void CommandBufWrapper::recordEnd() const { cmd_buf->end(); }

void CommandBufWrapper::recordEndSubmit(std::initializer_list<vk::Semaphore> signal_semaphores,
                                        std::initializer_list<vk::Semaphore> wait_semaphores,
                                        std::initializer_list<vk::PipelineStageFlags> wait_stages) const {
    assert(wait_semaphores.size() == wait_stages.size());

    recordEnd();
    vk::SubmitInfo submit_info;
    submit_info.setCommandBuffers(cmd_buf.get());
    submit_info.setSignalSemaphores(signal_semaphores);
    submit_info.setWaitSemaphores(wait_semaphores);
    submit_info.setWaitDstStageMask(wait_stages);
    // Keep the fence signaled throughout recording. If an exception aborts
    // the frame before this point, the slot remains reusable.
    device.resetFences({fence.get()});
    try {
        queue.submit({submit_info}, fence.get());
    } catch (...) {
        // A failed submit cannot signal the reset fence. Recreate it signaled
        // so a recoverable submit error cannot poison this slot forever.
        try {
            vk::FenceCreateInfo fence_info;
            fence_info.flags = vk::FenceCreateFlagBits::eSignaled;
            fence = device.createFenceUnique(fence_info);
        } catch (...) {
            // Device loss/OOM may also prevent fence recreation. Preserve the
            // original submit failure; terminal device handling owns the rest.
        }
        throw;
    }
}

void CommandBufWrapper::consumeSemaphore(
    vk::Semaphore semaphore,
    vk::PipelineStageFlags wait_stage) const {
    const auto wait_result =
        device.waitForFences({fence.get()}, VK_TRUE, UINT64_MAX);
    if (wait_result != vk::Result::eSuccess) {
        throw std::runtime_error(
            "failed to wait for semaphore-consumption fence: " +
            vk::to_string(wait_result));
    }

    vk::SubmitInfo submit_info;
    submit_info.setWaitSemaphores(semaphore);
    submit_info.setWaitDstStageMask(wait_stage);
    device.resetFences({fence.get()});
    try {
        // The empty submission consumes a successful acquire signal even when
        // rendering was abandoned before any command buffer could be queued.
        queue.submit({submit_info}, fence.get());
    } catch (...) {
        try {
            vk::FenceCreateInfo fence_info;
            fence_info.flags = vk::FenceCreateFlagBits::eSignaled;
            fence = device.createFenceUnique(fence_info);
        } catch (...) {
        }
        throw;
    }

    const auto completion_result =
        device.waitForFences({fence.get()}, VK_TRUE, UINT64_MAX);
    if (completion_result != vk::Result::eSuccess) {
        throw std::runtime_error(
            "failed to complete semaphore-consumption submission: " +
            vk::to_string(completion_result));
    }
}

void CommandBufWrapper::abortRecording() const noexcept {
    try {
        cmd_buf->reset();
    } catch (...) {
        // Recovery callers are already unwinding the original render error.
    }
}

} // namespace Pelican
