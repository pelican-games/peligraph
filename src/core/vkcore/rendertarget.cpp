#include "rendertarget.hpp"

#include "../launchconfig.hpp"
#include "offscreenframetarget.hpp"
#include "swapchainframetarget.hpp"
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <stb_image_write.h>

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

void RenderTarget::recordOutputTransformCopy(vk::CommandBuffer cmd_buf, vk::Image source,
                                             vk::Format source_format, vk::Extent2D source_extent) {
    impl->recordOutputTransformCopy(cmd_buf, source, source_format, source_extent);
}

void RenderTarget::render_end() { impl->render_end(); }

vk::Format RenderTarget::getSwapchainFormat() const { return impl->caps().color_format; }

vk::Extent2D RenderTarget::getExtent() const { return impl->caps().extent; }

bool RenderTarget::consumeExtentChanged() { return impl->consumeExtentChanged(); }

FrameTargetCaps RenderTarget::caps() const { return impl->caps(); }

std::vector<uint8_t> RenderTarget::readbackLastFrameRGBA8() {
    auto pixels = impl->readbackLastFrameRGBA8();
    const auto format = impl->caps().color_format;
    if (format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb) {
        for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
            std::swap(pixels[i], pixels[i + 2]);
        }
    }
    return pixels;
}

void RenderTarget::captureLastFrameToPng(const std::filesystem::path &path) {
    const auto caps = impl->caps();
    auto pixels = readbackLastFrameRGBA8();

    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    const auto path_string = path.string();
    const int stride = static_cast<int>(caps.extent.width * 4);
    if (stbi_write_png(path_string.c_str(), static_cast<int>(caps.extent.width),
                       static_cast<int>(caps.extent.height), 4, pixels.data(), stride) == 0) {
        throw std::runtime_error("failed to write PNG: " + path_string);
    }
}

} // namespace Pelican
