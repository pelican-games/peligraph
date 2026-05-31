#pragma once

#include "../container.hpp"
#include "render_target_layout_tracker.hpp"
#include "../renderingpass/renderingpass.hpp"
#include <vulkan/vulkan.hpp>

namespace Pelican {

DECLARE_MODULE(Renderer) {
    
    vk::Device device;
    RenderingPassId current_rendering_pass_id;
    RenderTargetLayoutTracker render_target_layout_tracker;

  public:
    Renderer();
    ~Renderer();
    void render();
};

} // namespace Pelican
