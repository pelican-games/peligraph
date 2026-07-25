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

struct RenderingTargetPlanDeviceFacts {
    std::uint32_t max_color_attachments = 8;
    bool multiview = false;
    std::uint32_t max_multiview_view_count = 0;
    XrMultiviewDeviceIdentity device_identity;
    AttachmentSampleCapabilityQuery query_attachment_samples;
    ExternalDepthTransferCapabilityQuery
        supports_external_depth_transfer;
};

struct RenderingTargetPlanCompilation {
    std::vector<std::shared_ptr<const VulkanTargetPlan>> plans;
    std::vector<RenderingSampleCountAssignment> assignments;
    std::vector<RenderingTargetArrayLayerAssignment>
        array_layer_assignments;
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
    TargetPlanningPolicy target_planning = {});
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
    TargetPlanningPolicy target_planning = {});

void applyRenderingTargetPlan(
    std::span<RenderTargetDefinition> render_targets,
    const RenderingTargetPlanCompilation &compilation);

vk::ResolveModeFlagBits colorAttachmentResolveMode(vk::Format format);

} // namespace Pelican
