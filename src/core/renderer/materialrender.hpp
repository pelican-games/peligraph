#pragma once

#include "../container.hpp"
#include <map>
#include <unordered_map>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct Material {
    uint32_t pipeline_id;
};

class MaterialRenderer {
    DependencyContainer &con;
  public:
    MaterialRenderer(DependencyContainer &con);
    void render(vk::CommandBuffer cmd_buf) const;
};

} // namespace Pelican
