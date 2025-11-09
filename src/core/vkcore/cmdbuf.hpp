#pragma once

#include <initializer_list>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class CommandBufWrapper {
    vk::Device device;
    vk::Queue queue;
    vk::UniqueCommandBuffer cmd_buf;
    vk::UniqueFence fence;

  public:
    CommandBufWrapper() {}
    CommandBufWrapper(vk::Device _device, vk::Queue _queue, vk::UniqueCommandBuffer &&_cmd_buf,
                      vk::UniqueFence &&_fence);
    const vk::CommandBuffer *operator->() const { return &cmd_buf.get(); }
    const vk::CommandBuffer operator*() const { return cmd_buf.get(); }
    void recordBegin() const;
    void recordEndSubmit(std::initializer_list<vk::Semaphore> signal_semaphores = {},
                         std::initializer_list<vk::Semaphore> wait_semaphores = {},
                         std::initializer_list<vk::PipelineStageFlags> wait_stages = {}) const;
    const vk::Fence &getFence() const { return fence.get(); }
};

} // namespace Pelican
