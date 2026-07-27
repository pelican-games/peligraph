#pragma once

#include "frametarget.hpp"

#include <memory>

namespace Pelican {

class SwapchainFrameTarget final : public IFrameTarget {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    SwapchainFrameTarget();
    ~SwapchainFrameTarget() override;

    SwapchainFrameTarget(
        const SwapchainFrameTarget &) = delete;
    SwapchainFrameTarget &operator=(
        const SwapchainFrameTarget &) = delete;

    FrameBeginResult beginFrame(
        std::shared_ptr<const RendererRuntimeGeneration>
            runtime_generation,
        GpuSubmissionLease submission_lease,
        FrameBeginMode mode) override;
    void recordOutputTransformCopy(
        vk::CommandBuffer cmd_buf, vk::Image source,
        vk::Format source_format,
        vk::Extent2D source_extent) override;
    FrameSubmitResult submit(
        FrameTargetFrame frame) override;
    void abandon(
        FrameTargetFrame frame) noexcept override;
    FrameTargetCaps caps() const override;
    FrameTargetStatus status() const override;
    std::vector<std::uint8_t>
    readbackLastFrameRGBA8() override;
};

} // namespace Pelican
