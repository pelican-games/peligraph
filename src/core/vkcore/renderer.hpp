#pragma once

#include "../container.hpp"
#include "cmdbuf.hpp"
#include <array>
#include <vulkan/vulkan.hpp>

namespace Pelican {

DECLARE_MODULE(Renderer) {
    
    vk::Device device;

  public:
    Renderer();
    ~Renderer();
    void render();
};

} // namespace Pelican
