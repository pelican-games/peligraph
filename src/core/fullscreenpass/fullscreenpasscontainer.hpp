#pragma once

#include "../container.hpp"
#include <vulkan/vulkan.hpp>

namespace Pelican {

DECLARE_MODULE(FullscreenPassContainer) {
  public:
    void registerFullscreenPass(/* TODO */);
    void bindResource(vk::CommandBuffer cmd_buf, int pass_id);
};

} // namespace Pelican
