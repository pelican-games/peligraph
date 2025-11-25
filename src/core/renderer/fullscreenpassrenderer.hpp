#pragma once

#include "../container.hpp"
#include <vulkan/vulkan.hpp>

namespace Pelican {

DECLARE_MODULE(FullscreenPassRenderer) {
  public:
    FullscreenPassRenderer();
    ~FullscreenPassRenderer();

    void render(vk::CommandBuffer cmd_buf, int pass_id) const;
};

} // namespace Pelican
