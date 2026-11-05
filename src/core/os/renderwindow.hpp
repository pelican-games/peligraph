#pragma once

// MD-1a0: the little part of a platform window the surface and frame-target
// code needs. The engine's main Window implements it; the diagnostic output
// window also implements it, so a second SwapchainFrameTarget can own its own
// surface without consuming the main window's bootstrap surface.

#include "framebuffersnapshot.hpp"

#include <vulkan/vulkan.hpp>

namespace Pelican {

struct NativeWindow;

class RenderWindow {
  public:
    virtual ~RenderWindow() = default;
    virtual FramebufferExtentSnapshot
    framebufferSnapshot() const = 0;
    virtual NativeWindow *nativeHandle() const noexcept = 0;
};

} // namespace Pelican
