#pragma once

#include "../container.hpp"
#include "../material/materialrender.hpp"
#include "cmdbuf.hpp"
#include "rendertarget.hpp"
#include <array>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class Renderer {
    RenderTarget &rt;
    MaterialRenderer &mat_renderer;

    vk::Device device;

  public:
    Renderer(DependencyContainer &container);
    ~Renderer();
    void render();
};

} // namespace Pelican
