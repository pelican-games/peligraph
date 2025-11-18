#pragma once

#include "../container.hpp"
#include <vulkan/vulkan.hpp>

namespace Pelican {

DECLARE_MODULE(MaterialRenderer) {
    

  public:
    MaterialRenderer();
    void render(vk::CommandBuffer cmd_buf) const;
};

} // namespace Pelican
