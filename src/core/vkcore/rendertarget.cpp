#include "rendertarget.hpp"

#include "../launchconfig.hpp"
#include "offscreenframetarget.hpp"
#include "swapchainframetarget.hpp"

namespace Pelican {

namespace {

std::unique_ptr<IFrameTarget> createFrameTarget() {
    if (GET_MODULE(EngineLaunchConfig).headless) {
        return std::make_unique<OffscreenFrameTarget>();
    }
    return std::make_unique<SwapchainFrameTarget>();
}

} // namespace

RenderTarget::RenderTarget() : impl{createFrameTarget()} {}

RenderTarget::~RenderTarget() {}

FrameRenderContext RenderTarget::render_begin() { return impl->render_begin(); }

void RenderTarget::render_end() { impl->render_end(); }

vk::Format RenderTarget::getSwapchainFormat() const { return impl->caps().color_format; }

} // namespace Pelican
