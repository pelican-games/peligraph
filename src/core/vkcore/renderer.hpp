#pragma once

#include "../container.hpp"
#include "cmdbuf.hpp"
#include <array>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class Renderer {
    DependencyContainer &con;
    vk::Device device;

  public:
    Renderer(DependencyContainer &container);
    ~Renderer();
    void render();
};

} // namespace Pelican
