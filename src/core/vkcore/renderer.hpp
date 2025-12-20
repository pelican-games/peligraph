#pragma once

#include "../container.hpp"
#include "cmdbuf.hpp"
#include <array>
#include <vulkan/vulkan.hpp>
#include "../renderingpass/renderingpass.hpp" // Added for GlobalRenderTargetId

namespace Pelican {

DECLARE_MODULE(Renderer) {
    
    vk::Device device;
    GlobalRenderTargetId m_scene_color_rt_id; // Added for offscreen scene color output

  public:
    Renderer();
    ~Renderer();
    void render();
};

} // namespace Pelican
