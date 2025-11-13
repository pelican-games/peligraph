#pragma once

#include "../container.hpp"
#include <vulkan/vulkan.hpp>

namespace Pelican {

class MaterialRenderer {
    DependencyContainer &con;

  public:
    MaterialRenderer(DependencyContainer &con);
    void render(vk::CommandBuffer cmd_buf) const;
};

} // namespace Pelican
