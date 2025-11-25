#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include <vulkan/vulkan.hpp>

namespace Pelican {

DECLARE_MODULE(FullscreenPassContainer) {
    // TODO

  public:
    void registerFullscreenPass(/* TODO */);
    void bindResource(vk::CommandBuffer cmd_buf, PassId pass_id);
};

} // namespace Pelican
