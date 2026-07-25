#pragma once

#include "frameplanner.hpp"
#include "rendertargetdefinition.hpp"
#include "../../project/targetrenderplanning.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Pelican {

using AttachmentSampleCapabilityQuery =
    std::function<std::vector<std::uint32_t>(
        const RenderTargetDefinition &)>;
using ExternalDepthTransferCapabilityQuery =
    std::function<bool(
        const RenderTargetDefinition &)>;

struct RenderingImageFormatCapability {
    bool image_usage_supported = false;
    std::vector<std::uint32_t> supported_samples;
    std::uint32_t max_array_layers = 1;
    bool external_depth_export_supported = false;

    bool operator==(
        const RenderingImageFormatCapability &) const =
        default;
};

using ImageFormatCapabilityQuery =
    std::function<RenderingImageFormatCapability(
        const RenderTargetDefinition &)>;

struct RenderingSampleCountAssignment {
    std::string resource;
    std::uint32_t samples = 1;

    bool operator==(const RenderingSampleCountAssignment &) const = default;
};

struct RenderingTargetArrayLayerAssignment {
    std::string resource;
    std::uint32_t array_layers = 1;

    bool operator==(
        const RenderingTargetArrayLayerAssignment &) const =
        default;
};

struct RenderingTargetFormatAssignment {
    std::string resource;
    vk::Format format = vk::Format::eUndefined;

    bool operator==(
        const RenderingTargetFormatAssignment &) const =
        default;
};

struct RenderingTargetPlanDeviceFacts {
    std::uint32_t max_color_attachments = 8;
    bool multiview = false;
    std::uint32_t max_multiview_view_count = 0;
    XrMultiviewDeviceIdentity device_identity;
    AttachmentSampleCapabilityQuery query_attachment_samples;
    ExternalDepthTransferCapabilityQuery
        supports_external_depth_transfer;
    // Required for selecting a non-automatic format candidate. Legacy
    // sample/depth callbacks remain the compatibility source for the
    // automatic format until their callers migrate to this complete query.
    ImageFormatCapabilityQuery
        query_image_format_capability;
};

struct RenderingTargetPlanCompilation {
    std::vector<std::shared_ptr<const VulkanTargetPlan>> plans;
    std::vector<RenderingSampleCountAssignment> assignments;
    std::vector<RenderingTargetArrayLayerAssignment>
        array_layer_assignments;
    std::vector<RenderingTargetFormatAssignment>
        format_assignments;
};

std::vector<CompiledLogicalRenderGraph>
compileRenderingLogicalGraphs(
    std::span<const FrameGraphDefinition> frame_graphs,
    std::span<const RenderTargetDefinition> render_targets);

// Adapts the existing FrameGraphDefinition into the canonical logical/target
// planner. The returned VulkanTargetPlan is authoritative for the concrete
// image format, representation, and rasterization sample count consumed by
// the current materialized-image runtime.
RenderingTargetPlanCompilation compileRenderingTargetPlans(
    std::span<const FrameGraphDefinition> frame_graphs,
    std::span<const RenderTargetDefinition> render_targets,
    const SampleCountPolicy &policy,
    vk::Format swapchain_format,
    const RenderingTargetPlanDeviceFacts &device_facts,
    std::optional<VulkanViewExecutionPlanRequest> view_execution =
        std::nullopt,
    std::optional<VulkanExternalDepthExportRequest>
        external_depth_export = std::nullopt,
    TargetPlanningPolicy target_planning = {},
    std::span<const VulkanTargetPlanPinPackage>
        plan_pins = {},
    std::span<const VulkanPhysicalFragmentPackage>
        physical_fragments = {});
RenderingTargetPlanCompilation compileRenderingTargetPlansForVulkanDevice(
    std::span<const FrameGraphDefinition> frame_graphs,
    std::span<const RenderTargetDefinition> render_targets,
    const SampleCountPolicy &policy,
    vk::Format swapchain_format,
    vk::PhysicalDevice physical_device,
    std::optional<VulkanViewExecutionPlanRequest> view_execution =
        std::nullopt,
    std::optional<VulkanExternalDepthExportRequest>
        external_depth_export = std::nullopt,
    TargetPlanningPolicy target_planning = {},
    std::span<const VulkanTargetPlanPinPackage>
        plan_pins = {},
    std::span<const VulkanPhysicalFragmentPackage>
        physical_fragments = {});

void applyRenderingTargetPlan(
    std::span<RenderTargetDefinition> render_targets,
    const RenderingTargetPlanCompilation &compilation);

vk::ResolveModeFlagBits colorAttachmentResolveMode(vk::Format format);

} // namespace Pelican
