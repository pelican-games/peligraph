#pragma once

#include "../container.hpp"
#include "cmdbuf.hpp"
#include "frametarget.hpp"
#include "image.hpp"
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

constexpr size_t in_flight_frames_num = 2;

DECLARE_MODULE(RenderTarget) {
    std::unique_ptr<IFrameTarget> impl;

  public:
    RenderTarget();
    ~RenderTarget();

    FrameBeginResult beginFrame(
        std::shared_ptr<const RendererRuntimeGeneration>
            runtime_generation,
        GpuSubmissionLease submission_lease = {},
        FrameBeginMode mode = FrameBeginMode::blocking);
    void recordOutputTransformCopy(vk::CommandBuffer cmd_buf, vk::Image source,
                                   vk::Format source_format, vk::Extent2D source_extent);
    FrameSubmitResult submit(FrameTargetFrame frame);
    void abandon(FrameTargetFrame frame) noexcept;

    vk::Format getSwapchainFormat() const;
    vk::Extent2D getExtent() const;
    FrameTargetCaps caps() const;
    // Color contract 2: encoded-sRGB RGBA8 bytes with straight, untransferred alpha.
    std::vector<uint8_t> readbackLastFrameRGBA8();
    // PNGs contain no color chunk; contract 2 requires consumers to interpret them as sRGB.
    void captureLastFrameToPng(const std::filesystem::path &path);
};

} // namespace Pelican
