#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include <vulkan/vulkan.hpp>

namespace Pelican {

DECLARE_MODULE(FullscreenPassRenderer) {
  public:
    FullscreenPassRenderer();
    ~FullscreenPassRenderer();

    void render(vk::CommandBuffer cmd_buf, PassId pass_id, const PassDefinition& pass_def) const;
};

} // namespace Pelican
