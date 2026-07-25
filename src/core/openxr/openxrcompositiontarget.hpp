#pragma once

#include "openxrsession.hpp"

#include "../vkcore/renderer.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace Pelican {

class VulkanManageCore;

namespace OpenXr {

inline constexpr std::uint32_t xr_stereo_view_count = 2;

enum class XrSwapchainState {
    idle,
    acquired,
    waited,
    submitted,
    released,
};

struct XrCompositionGraphicsConfig {
    PFN_xrEnumerateSwapchainImages enumerate_swapchain_images = nullptr;
    XrSwapchain color_swapchain = XR_NULL_HANDLE;
    XrSwapchain depth_swapchain = XR_NULL_HANDLE;
    std::array<XrViewConfigurationView, xr_stereo_view_count> views{};
    vk::Extent2D extent{};
    vk::Format color_format = vk::Format::eUndefined;
    vk::Format depth_format = vk::Format::eUndefined;
    vk::ImageLayout release_layout = vk::ImageLayout::eColorAttachmentOptimal;
};

struct XrCompositionAcquiredImages {
    std::uint32_t color = 0;
    std::optional<std::uint32_t> depth;
};

// Vulkan work is deliberately behind this seam.  Protocol fixtures implement
// it without manufacturing VkImage handles, while the production bridge owns
// the real runtime-provided images, views, command buffers, and fences.
class IXrCompositionGraphics {
  public:
    virtual ~IXrCompositionGraphics() = default;
    virtual void initialize(const XrCompositionGraphicsConfig &config) = 0;
    virtual FrameRenderContext beginView(std::uint32_t view_index,
                                         XrCompositionAcquiredImages images,
                                         std::uint32_t in_flight_frame_index) = 0;
    virtual FrameRenderContext beginViewFamily(
        XrCompositionAcquiredImages images,
        std::uint32_t in_flight_frame_index) = 0;
    virtual void submitView(std::uint32_t view_index,
                            std::uint32_t in_flight_frame_index) = 0;
    virtual void submitViewFamily(
        std::uint32_t in_flight_frame_index) = 0;
    virtual void waitForSubmission(std::uint32_t view_index,
                                   std::uint32_t in_flight_frame_index) = 0;
    virtual void waitForViewFamilySubmission(
        std::uint32_t in_flight_frame_index) = 0;
};

struct XrCompositionDependencies {
    PFN_xrGetInstanceProcAddr get_instance_proc_addr = nullptr;
    SessionRuntime *session_runtime = nullptr;
    VulkanManageCore *vulkan = nullptr;
    vk::Format renderer_color_format = vk::Format::eUndefined;
    vk::Format renderer_depth_format = vk::Format::eUndefined;
    XrDuration image_wait_timeout = XR_INFINITE_DURATION;
    bool composition_layer_depth_enabled = false;
};

struct XrCompositionCapabilities {
    bool array_color_swapchain = false;
    bool view_family_execution = false;
    bool depth_submission = false;
    vk::Format depth_format = vk::Format::eUndefined;
    vk::Extent2D extent{};
    std::string depth_reason;
};

class IXrCompositionTarget : public ILogicalFrameTarget {
  public:
    ~IXrCompositionTarget() override = default;
    virtual void prepareFrame(const XrDisplayTiming &display_timing,
                              const XrLocatedViews &located_views,
                              float near_z = 0.05F,
                              float far_z = 1000.0F) = 0;
    virtual void endFrameWithoutLayers(const XrDisplayTiming &display_timing) = 0;
    virtual bool generationTeardownRequired() const noexcept = 0;
    virtual XrSwapchainState swapchainState(std::uint32_t view_index) const = 0;
    virtual XrSwapchainState depthSwapchainState() const = 0;
    virtual XrCompositionCapabilities compositionCapabilities() const = 0;
};

class XrCompositionTarget final : public IXrCompositionTarget {
    class Impl;
    std::unique_ptr<Impl> impl;

  public:
    explicit XrCompositionTarget(const XrCompositionDependencies &dependencies);
    XrCompositionTarget(const XrCompositionDependencies &dependencies,
                        std::unique_ptr<IXrCompositionGraphics> graphics);
    ~XrCompositionTarget() override;

    XrCompositionTarget(const XrCompositionTarget &) = delete;
    XrCompositionTarget &operator=(const XrCompositionTarget &) = delete;

    void prepareFrame(const XrDisplayTiming &display_timing,
                      const XrLocatedViews &located_views,
                      float near_z = 0.05F,
                      float far_z = 1000.0F) override;
    void endFrameWithoutLayers(const XrDisplayTiming &display_timing) override;
    bool configureExternalDepthSubmission(
        vk::Format source_format,
        vk::Extent2D source_extent) override;
    void beginLogicalFrame(std::uint32_t view_count) override;
    FrameRenderContext beginView(std::uint32_t view_index) override;
    bool supportsViewFamilyExecution() const noexcept override;
    FrameRenderContext beginViewFamily(
        std::uint32_t view_count) override;
    void endView(
        std::uint32_t view_index,
        GpuSubmissionLease lease = {}) override;
    void endViewFamily(
        GpuSubmissionLease lease = {}) override;
    void endLogicalFrame(GpuSubmissionLease lease = {}) override;
    vk::Format colorFormat(std::uint32_t view_index) const override;
    bool consumeExtentChanged() override;
    bool generationTeardownRequired() const noexcept override;
    XrSwapchainState swapchainState(std::uint32_t view_index) const override;
    XrSwapchainState depthSwapchainState() const override;
    XrCompositionCapabilities compositionCapabilities() const override;
};

} // namespace OpenXr
} // namespace Pelican
