#pragma once

#include "frametarget.hpp"
#include "windowsurface.hpp"

#include <cstdint>
#include <memory>

namespace Pelican {

class RenderWindow;

// MD-1a0: a second SwapchainFrameTarget over a window that is not the engine's
// Window module. The caller supplies the window and a fresh surface it owns;
// the target then consumes neither the main window's bootstrap surface nor its
// camera extent. Recovery preparation still uses the same WSI machinery.
struct ExternalWindowTargetConfig {
    RenderWindow *window = nullptr;
    PreparedWindowSurface surface;
    std::uint32_t presentation_queue_family =
        VK_QUEUE_FAMILY_IGNORED;
    vk::Fence slot_fence_override_for_testing;
};

class SwapchainFrameTarget final : public IFrameTarget {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    SwapchainFrameTarget();
    explicit SwapchainFrameTarget(
        ExternalWindowTargetConfig config);
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
    void submitFrame(FrameTargetFrame frame) override;
    FrameSubmitResult publishFrame() override;
    void recoverSubmittedFrame() noexcept override;
    void abandon(
        FrameTargetFrame frame) noexcept override;
    FrameTargetCaps caps() const override;
    FrameTargetStatus status() const override;
    std::vector<std::uint8_t>
    readbackLastFrameRGBA8() override;
};

} // namespace Pelican
