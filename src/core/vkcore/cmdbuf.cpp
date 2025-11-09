#include "cmdbuf.hpp"

namespace Pelican {

CommandBufWrapper::CommandBufWrapper(vk::Device _device, vk::Queue _queue, vk::UniqueCommandBuffer &&_cmd_buf,
                                     vk::UniqueFence &&_fence)
    : device{_device}, queue{_queue}, cmd_buf{std::move(_cmd_buf)}, fence{std::move(_fence)} {}
void CommandBufWrapper::recordBegin() const {
    cmd_buf->reset();
    device.resetFences({fence.get()});

    vk::CommandBufferBeginInfo begin_info;
    begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    cmd_buf->begin(begin_info);
}
void CommandBufWrapper::recordEndSubmit(std::initializer_list<vk::Semaphore> signal_semaphores,
                                        std::initializer_list<vk::Semaphore> wait_semaphores,
                                        std::initializer_list<vk::PipelineStageFlags> wait_stages) const {
    assert(wait_semaphores.size() == wait_stages.size());

    cmd_buf->end();
    vk::SubmitInfo submit_info;
    submit_info.setCommandBuffers(cmd_buf.get());
    submit_info.setSignalSemaphores(signal_semaphores);
    submit_info.setWaitSemaphores(wait_semaphores);
    submit_info.setWaitDstStageMask(wait_stages);
    queue.submit({submit_info}, fence.get());
}

} // namespace Pelican
