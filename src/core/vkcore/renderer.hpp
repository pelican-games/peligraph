#pragma once

#include "../container.hpp"
#include "cmdbuf.hpp"
#include "render_target_layout_tracker.hpp"
#include <array>
#include <vulkan/vulkan.hpp>
#include "../renderingpass/renderingpass.hpp" // Added for GlobalRenderTargetId

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
