#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include <vulkan/vulkan.hpp>

namespace Pelican {

DECLARE_MODULE(MaterialRenderer) {
  public:
    MaterialRenderer();
    void render(vk::CommandBuffer cmd_buf, PassId pass_id) const;

    void renderWithMaterialRange(vk::CommandBuffer cmd_buf, PassId pass_id,
                                uint32_t material_start, uint32_t material_count) const;
};

} // namespace Pelican
