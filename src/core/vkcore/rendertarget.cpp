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

FrameBeginResult RenderTarget::beginFrame(
    std::shared_ptr<const RendererRuntimeGeneration>
        runtime_generation,
    GpuSubmissionLease submission_lease,
    FrameBeginMode mode) {
    return impl->beginFrame(
        std::move(runtime_generation),
        std::move(submission_lease), mode);
}

void RenderTarget::recordOutputTransformCopy(vk::CommandBuffer cmd_buf, vk::Image source,
                                             vk::Format source_format, vk::Extent2D source_extent) {
    impl->recordOutputTransformCopy(cmd_buf, source, source_format, source_extent);
}

FrameSubmitResult RenderTarget::submit(
    FrameTargetFrame frame) {
    return impl->submit(std::move(frame));
}

void RenderTarget::abandon(
    FrameTargetFrame frame) noexcept {
    impl->abandon(std::move(frame));
}

vk::Format RenderTarget::getSwapchainFormat() const {
    return impl->caps().compile_facts.color_format;
}

vk::Extent2D RenderTarget::getExtent() const {
    return impl->caps().compile_facts.extent;
}

FrameTargetCaps RenderTarget::caps() const { return impl->caps(); }

FrameTargetStatus RenderTarget::status() const {
    return impl->status();
}

std::vector<uint8_t> RenderTarget::readbackLastFrameRGBA8() {
    if (GET_MODULE(EngineLaunchConfig).xr_active) {
        throw std::runtime_error(
            "legacy capture is unavailable while OpenXR is active; source=flat is required");
    }
    auto pixels = impl->readbackLastFrameRGBA8();
    const auto format =
        impl->caps().compile_facts.color_format;
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
    const auto extent = caps.compile_facts.extent;
    const int stride = static_cast<int>(extent.width * 4);
    if (stbi_write_png(path_string.c_str(), static_cast<int>(extent.width),
                       static_cast<int>(extent.height), 4, pixels.data(), stride) == 0) {
        throw std::runtime_error("failed to write PNG: " + path_string);
    }
}

} // namespace Pelican
