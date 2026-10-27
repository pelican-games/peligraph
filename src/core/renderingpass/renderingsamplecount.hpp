#pragma once

#include "frameplanner.hpp"
#include "rendertargetdefinition.hpp"
#include "../vkcore/runtimecapabilities.hpp"
#include "../../project/rendergraphir.hpp"
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
    std::uint32_t max_mip_levels = 1;
    std::uint32_t max_array_layers = 1;
    bool external_depth_export_supported = false;
    bool transient_attachment_supported = false;
    bool local_read_attachment_supported = false;

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
    ImageFormat format = ImageFormat::r8g8b8a8_unorm;

    bool operator==(
        const RenderingTargetFormatAssignment &) const =
        default;
};

struct RenderingTargetRepresentationAssignment {
    std::string resource;
    VulkanResourceRepresentation representation =
        VulkanResourceRepresentation::materialized_image;

    bool operator==(
        const RenderingTargetRepresentationAssignment &) const =
        default;
};

struct RenderingTargetAliasGroupAssignment {
    std::string id;
    std::vector<std::string> resources;

    bool operator==(
        const RenderingTargetAliasGroupAssignment &) const =
        default;
};

struct RenderingTargetPlanDeviceFacts {
    std::uint32_t max_color_attachments = 8;
    bool multiview = false;
    std::uint32_t max_multiview_view_count = 0;
    bool ray_query = false;
    bool ray_tracing_pipeline = false;
    bool ray_material_bindless = false;
    XrMultiviewDeviceIdentity device_identity;
    AttachmentSampleCapabilityQuery query_attachment_samples;
    ExternalDepthTransferCapabilityQuery
        supports_external_depth_transfer;
    // Required for selecting a non-automatic format candidate. Legacy
    // sample/depth callbacks remain the compatibility source for the
    // automatic format until their callers migrate to this complete query.
    ImageFormatCapabilityQuery
        query_image_format_capability;
    bool transient_attachments = false;
    bool dynamic_rendering_local_read = false;
};

// Canonical endpoint projection used by target planning and engine tooling
// that must answer whether a feature's required capabilities are executable
// on the selected runtime device.
TargetEndpoint renderingTargetRuntimeEndpoint(
    const RenderingTargetPlanDeviceFacts &facts,
    bool enable_tile_local);

// Exact source inputs retained beside an automatic target plan so a
// delegating backend compiler can verify and install a complete physical
// replacement without reconstructing private runtime-device facts.
struct RenderingTargetPlanVerificationContext {
    FramePlanNodeOrder node_order;
    std::shared_ptr<const CompiledLogicalRenderGraph>
        logical_graph;
    TargetTopologySnapshot topology;
    std::shared_ptr<const VulkanTargetPlan>
        automatic_plan;
    // The plan published by this compilation. It is the automatic plan
    // itself unless a verified physical fragment was linked. Keeping both
    // identities prevents validation from treating a linked plan as the
    // automatic fingerprint source.
    std::shared_ptr<const VulkanTargetPlan>
        compiled_plan;
    std::vector<VulkanPhysicalResourceFormatCapability>
        format_capabilities;
    // Filled by the Vulkan compiler layer, which owns the actual logical
    // device contract rather than the target planner.
    std::vector<std::string>
        enabled_device_extensions;
    // Runtime implementations which begin/end their own rendering instance.
    std::vector<std::string> scope_owning_nodes;
};

struct RenderingTargetPlanCompilation {
    std::vector<std::shared_ptr<const VulkanTargetPlan>> plans;
    std::vector<RenderingTargetPlanVerificationContext>
        verification_contexts;
    std::vector<RenderingSampleCountAssignment> assignments;
    std::vector<RenderingTargetArrayLayerAssignment>
        array_layer_assignments;
    std::vector<RenderingTargetFormatAssignment>
        format_assignments;
    std::vector<RenderingTargetRepresentationAssignment>
        representation_assignments;
    std::vector<RenderingTargetAliasGroupAssignment>
        alias_group_assignments;
};

// Plans every graph of the finalized render graph (finalizeRenderGraph) on
// the target: each graph's logical graph is project's toLogicalGraph with
// this device's tile-local facts. The returned VulkanTargetPlan is
// authoritative for the concrete image format, representation, and
// rasterization sample count consumed by the adaptive render-target runtime.
// `render_targets` are the execution declarations of the IR's config images.
RenderingTargetPlanCompilation compileRenderingTargetPlans(
    const RenderGraphIR &render_graph,
    const std::unordered_map<std::string, FramePlan> &frame_plans,
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
        physical_fragments = {},
    std::span<const RenderPassProvenance> provenance = {});
RenderingTargetPlanCompilation compileRenderingTargetPlansForVulkanDevice(
    const RenderGraphIR &render_graph,
    const std::unordered_map<std::string, FramePlan> &frame_plans,
    std::span<const RenderTargetDefinition> render_targets,
    const SampleCountPolicy &policy,
    vk::Format swapchain_format,
    vk::PhysicalDevice physical_device,
    const VulkanRuntimeCapabilities &runtime_capabilities,
    std::optional<VulkanViewExecutionPlanRequest> view_execution =
        std::nullopt,
    std::optional<VulkanExternalDepthExportRequest>
        external_depth_export = std::nullopt,
    TargetPlanningPolicy target_planning = {},
    std::span<const VulkanTargetPlanPinPackage>
        plan_pins = {},
    std::span<const VulkanPhysicalFragmentPackage>
        physical_fragments = {},
    std::span<const RenderPassProvenance> provenance = {});

// RC-3c2a: what the physical plan placed of each render target (one variant,
// in `render_targets`' order): its sample count, layers, format, storage and
// alias group, and whether the plan reads it as a local-read input attachment
// or exports it as the external depth. Checks each assignment against the
// definition (an authored format candidate, a transient / tile-local
// candidate, an alias group of eligible members of one contract). The
// generation's resource description merges the variants' placements and
// applies the engine's usage (resolveGenerationResources).
std::vector<ImagePlacement> imagePlacementsOf(
    std::span<const RenderTargetDefinition> render_targets,
    const RenderingTargetPlanCompilation &compilation);

// RC-3a1: by the format table's numeric type.
vk::ResolveModeFlagBits colorAttachmentResolveMode(ImageFormat format);

} // namespace Pelican
