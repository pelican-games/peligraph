#include "rendertarget.hpp"

#include "swapchainframetarget.hpp"

namespace Pelican {

RenderTarget::RenderTarget() : impl{std::make_unique<SwapchainFrameTarget>()} {}

RenderTarget::~RenderTarget() {}

FrameRenderContext RenderTarget::render_begin() { return impl->render_begin(); }

void RenderTarget::render_end() { impl->render_end(); }

vk::Format RenderTarget::getSwapchainFormat() const { return impl->caps().color_format; }

} // namespace Pelican
