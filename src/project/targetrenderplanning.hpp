#pragma once

#include "attachmentoperations.hpp"
#include "imagesubresource.hpp"
#include "imageformat.hpp"
#include "resourceextent.hpp"
#include "samplecountplanning.hpp"
#include "targetplanning.hpp"
#include "vulkanviewplanning.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace Pelican {

struct RenderPassProvenance;

enum class ResourcePatternFallback : std::uint8_t {
    materialize,
    reject,
};

std::string_view resourcePatternFallbackName(
    ResourcePatternFallback fallback);

struct ResourceFormatCandidate {
    ImageFormat format = ImageFormat::r8g8b8a8_unorm;
    std::vector<std::string> required_capabilities;

    bool operator==(const ResourceFormatCandidate &) const = default;
};

// A ResourcePattern is a copyable target-planning policy, not a logical type
// and not a Vulkan object. The binding to an individual logical resource is
// separate so one pattern can describe any number of G-buffer attachments.
struct ResourcePattern {
    std::string id;
    LogicalTypePattern applicable_type;
    std::vector<ResourceFormatCandidate> format_candidates;
    bool prefer_transient = true;
    bool allow_tile_local = true;
    bool allow_alias = true;
    bool require_store = false;
    ResourcePatternFallback local_read_fallback =
        ResourcePatternFallback::materialize;
    std::uint64_t estimated_bytes = 0;
    std::string provenance;
    // PO-1: why the binding's producer keeps the resource out of aliasing
    // (which usage left the alias-safe set, ...). Explains the plan's reason
    // for a required or stored materialization; planning never reads it.
    std::string store_reason;
};

struct ResourcePatternBinding {
    std::string resource;
    ResourcePattern pattern;
    std::optional<ResourceExtentPlan> extent;
    ImageMipLevelCount mip_levels;
    std::uint32_t array_layers = 1;
    ImageResourceDimension dimension =
        ImageResourceDimension::two_d;
};

enum class TargetIrDialect : std::uint8_t {
    logical,
    execution_gpu,
    physical_vulkan,
};

std::string_view targetIrDialectName(TargetIrDialect dialect);

enum class TargetLoweringStage : std::uint8_t {
    canonical_workspace,
    target_execution_complete,
    vulkan_physical_complete,
};

std::string_view targetLoweringStageName(TargetLoweringStage stage);

struct TargetResourceLifetime {
    bool used = false;
    std::size_t first_use = 0;
    std::size_t last_use = 0;

    bool operator==(const TargetResourceLifetime &) const = default;
};

struct TargetResourceUseSummary {
    bool read = false;
    bool written = false;
    bool attachment_access = false;
    bool sampled_access = false;
    bool storage_access = false;
    bool transfer_access = false;
    bool host_access = false;
    bool non_render_access = false;
    bool produced_by_snapshot = false;
    LogicalReadFootprintKind widest_read =
        LogicalReadFootprintKind::none;

    bool operator==(const TargetResourceUseSummary &) const = default;
};

struct TargetLoweringNode {
    LogicalGraphNode logical;
    TargetIrDialect dialect = TargetIrDialect::logical;
    std::vector<std::string> source_nodes;
    std::vector<std::string> required_physical_features;
};

struct TargetLoweringResource {
    LogicalResourceDesc logical;
    ResourcePattern pattern;
    std::optional<ResourceExtentPlan> extent;
    ImageMipLevelCount mip_levels;
    std::uint32_t array_layers = 1;
    ImageResourceDimension dimension =
        ImageResourceDimension::two_d;
    TargetIrDialect dialect = TargetIrDialect::logical;
    TargetResourceUseSummary uses;
    TargetResourceLifetime lifetime;
    std::vector<std::string> required_physical_features;
};

// This graph is an owned disposable workspace. It copies canonical logical
// data and may be destructively lowered without modifying the source graph.
struct TargetLoweringGraph {
    std::string name;
    TargetLoweringStage stage =
        TargetLoweringStage::canonical_workspace;
    std::vector<TargetLoweringNode> nodes;
    std::vector<TargetLoweringResource> resources;
    std::vector<PlanningDecision> decisions;
};

TargetLoweringGraph makeTargetLoweringGraph(
    const LogicalTypeRegistry &types,
    const CompiledLogicalRenderGraph &canonical_graph,
    std::span<const ResourcePatternBinding> pattern_bindings,
    const FramePlanNodeOrder &node_order);
void lowerTargetExecutionDialect(TargetLoweringGraph &graph);
void lowerVulkanPhysicalDialect(TargetLoweringGraph &graph);
void validateTargetLoweringGraphDialect(
    const TargetLoweringGraph &graph, TargetLoweringStage expected_stage);
nlohmann::ordered_json targetLoweringGraphToJson(
    const TargetLoweringGraph &graph);

enum class VulkanResourceRepresentation : std::uint8_t {
    materialized_image,
    materialized_buffer,
    transient_attachment,
    tile_local_attachment,
    external,
};

std::string_view vulkanResourceRepresentationName(
    VulkanResourceRepresentation representation);

enum class VulkanResourceViewLayout : std::uint8_t {
    shared_2d,
    sequential_2d,
    layered_2d_array,
    family_2d_array,
};

std::string_view vulkanResourceViewLayoutName(
    VulkanResourceViewLayout layout);

struct VulkanPhysicalResourcePlan {
    std::string logical_resource;
    std::string pattern;
    // nullopt for a buffer (its dump and fingerprint spell it "").
    std::optional<ImageFormat> format;
    VulkanResourceRepresentation representation =
        VulkanResourceRepresentation::materialized_image;
    LogicalReadFootprintKind widest_read =
        LogicalReadFootprintKind::none;
    TargetResourceLifetime lifetime;
    bool stored = false;
    bool aliasable = false;
    std::vector<std::string> required_physical_features;
    std::string reason;
    std::uint32_t rasterization_samples = 1;
    bool resolve_required = false;
    VulkanResourceViewLayout view_layout =
        VulkanResourceViewLayout::shared_2d;
    ImageMipLevelCount mip_levels;
    std::uint32_t array_layers = 1;
    ImageResourceDimension dimension =
        ImageResourceDimension::two_d;
    std::optional<ResourceExtentPlan> extent;

    bool operator==(
        const VulkanPhysicalResourcePlan &) const = default;
};

struct VulkanRenderResolutionPlan {
    std::string render_source_resource;
    ResourceExtentPlan render_extent;
    std::string output_source_resource = "swapchain";
    ResourceExtentPlan output_extent;
    std::vector<std::string> scene_resources;
};

enum class VulkanPhysicalScopeKind : std::uint8_t {
    rendering,
    compute,
    transfer,
    output,
    marker,
};

std::string_view vulkanPhysicalScopeKindName(
    VulkanPhysicalScopeKind kind);

struct VulkanPhysicalScopePlan {
    std::string id;
    VulkanPhysicalScopeKind kind =
        VulkanPhysicalScopeKind::rendering;
    std::vector<std::string> nodes;
    // True when the nodes are verified to execute inside one Vulkan dynamic
    // rendering instance. A multi-node scope may otherwise remain a logical
    // scheduling group whose nodes each open their own rendering instance.
    bool single_rendering_instance = false;
    std::vector<std::string> local_reads;
    std::vector<std::string> region_tags;
    std::uint32_t rasterization_samples = 1;
    VulkanScopeViewExecution view_execution =
        VulkanScopeViewExecution::single_view;
    std::uint32_t view_count = 1;
    std::uint32_t execution_count = 1;
    std::uint32_t view_mask = 0;
    // LP-Va: the family views every node of this scope is restricted to.
    // A scope never mixes selections, so the scheduler expands a
    // secondary-family scope only for the matching view indices.
    RenderViewSelect view_select;

    bool operator==(
        const VulkanPhysicalScopePlan &) const = default;
};

// Per-node attachment state belongs to the physical plan rather than the
// logical resource type. A resource can be cleared in one raster scope and
// loaded in another, so the node/resource pair is the stable identity.
struct VulkanPhysicalAttachmentPlan {
    std::string node;
    std::string logical_resource;
    std::optional<ImageSubresourceRange> subresource;
    AttachmentAspect aspect = AttachmentAspect::color;
    // RC-3a2: what the plan's executor does, always stated (no default; the
    // planner copies the node's resolved operations).
    PassAttachmentOperations operations;

    bool operator==(
        const VulkanPhysicalAttachmentPlan &) const = default;
};

struct VulkanAliasGroupPlan {
    std::string id;
    std::vector<std::string> resources;

    bool operator==(
        const VulkanAliasGroupPlan &) const = default;
};

struct VulkanSampleCountPlanRequest {
    SampleCountPolicy policy;
    std::vector<SampleCountResourceCapability> capabilities;
    std::vector<std::string> geometry_nodes;
};

// Requests a device/projection-depth image that remains materialized after
// logical graph execution so an external compositor can consume it.  An
// omitted source lets the target compiler infer the camera depth from typed
// graph structure; an explicit source is the escape hatch for custom graphs.
struct VulkanExternalDepthExportRequest {
    std::optional<std::string> source_resource;
    // Null means the embedding target did not constrain formats. A present
    // (possibly empty) list is the exact set whose physical images support
    // transfer-source use on the selected device.
    std::optional<std::vector<std::string>>
        compatible_source_resources;
    bool required = false;
};

struct VulkanExternalDepthExportPlan {
    std::string source_resource;
    ImageFormat format = ImageFormat::d32_sfloat;
    VulkanResourceViewLayout view_layout =
        VulkanResourceViewLayout::shared_2d;
    std::uint32_t array_layers = 1;
    std::string reason;

    bool operator==(
        const VulkanExternalDepthExportPlan &) const = default;
};

// Target-specific evidence for selecting a non-automatic image format from
// a ResourcePattern. The target bridge snapshots these facts from the actual
// device. Support cannot be inferred from a format name alone.
struct VulkanPhysicalResourceFormatCapability {
    std::string logical_resource;
    ImageFormat format = ImageFormat::r8g8b8a8_unorm;
    bool image_usage_supported = false;
    std::vector<std::uint32_t> supported_samples;
    std::uint32_t max_mip_levels = 1;
    std::uint32_t max_array_layers = 1;
    bool external_depth_export_supported = false;

    bool operator==(
        const VulkanPhysicalResourceFormatCapability &) const =
        default;
};

struct VulkanTargetPlanRequest {
    std::vector<PlanningPolicyEntry> policy;
    FramePlanNodeOrder node_order;
    std::string endpoint;
    std::string provider;
    std::vector<std::string> required_endpoint_capabilities;
    std::vector<ResourcePatternBinding> pattern_bindings;
    PlanningProfile profile;
    std::vector<PlanningNodeConstraint> node_constraints;
    std::vector<PlanningResourceConstraint> resource_constraints;
    PlanningDiagnosticPolicy diagnostic_policy;
    std::optional<VulkanSampleCountPlanRequest> sample_count;
    std::optional<VulkanViewExecutionPlanRequest> view_execution;
    std::optional<VulkanExternalDepthExportRequest>
        external_depth_export;
    std::vector<VulkanPhysicalAttachmentPlan>
        automatic_attachments;
};

struct VulkanTargetPlan {
    bool legacy_complete_install = false; // Runtime metadata; never serialized.
    std::vector<PlanningPolicyEntry> policy;
    std::string graph;
    std::uint64_t logical_graph_fingerprint = 0;
    std::uint64_t automatic_plan_fingerprint = 0;
    std::vector<LogicalGraphTransformSelection>
        graph_transforms;
    std::vector<LogicalSubgraphReplacementSelection>
        subgraph_replacements;
    std::optional<RenderStrategySelection>
        render_strategy;
    BackendSelection backend_selection;
    LogicalPlanningOpportunityReport opportunities;
    TargetLoweringGraph lowering_graph;
    std::vector<VulkanPhysicalResourcePlan> resources;
    std::vector<VulkanPhysicalScopePlan> scopes;
    std::vector<VulkanPhysicalAttachmentPlan> attachments;
    std::vector<VulkanAliasGroupPlan> alias_groups;
    std::vector<std::string> required_physical_features;
    std::vector<PlanningDecision> decisions;
    std::optional<ResolvedSampleCountPlan> sample_count_plan;
    VulkanViewExecutionPlan view_execution_plan;
    std::optional<VulkanRenderResolutionPlan>
        resolution_plan;
    std::optional<VulkanExternalDepthExportPlan>
        external_depth_export;
};

void validateVulkanPhysicalFeatureClosure(
    const BackendProbeResult &selected_probe,
    std::span<const std::string> lowered_required_features);
PhysicalNodeOrder physicalNodeOrderOf(std::span<const VulkanPhysicalScopePlan> scopes);
// Also serves as the test seam: an automatic plan with an unapplied policy
// must pass exactly the same final verification as the selected plan.
void validateVulkanTargetPlanRequirements(
    const CompiledLogicalRenderGraph &graph, VulkanTargetPlan &plan);

VulkanTargetPlan compileVulkanTargetPlan(
    const LogicalTypeRegistry &types,
    const CompiledLogicalRenderGraph &canonical_graph,
    const TargetTopologySnapshot &topology,
    const CompilerProviderRegistrySnapshot &providers,
    VulkanTargetPlanRequest request);
std::uint64_t vulkanTargetPlanLogicalGraphFingerprint(
    const CompiledLogicalRenderGraph &graph);
std::uint64_t vulkanAutomaticTargetPlanFingerprint(
    const TargetTopologySnapshot &topology,
    const VulkanTargetPlan &automatic_plan);
void validateVulkanPhysicalAttachmentPlans(
    const CompiledLogicalRenderGraph &canonical_graph,
    std::span<const VulkanPhysicalResourcePlan> resources,
    std::span<const VulkanPhysicalAttachmentPlan> attachments);
nlohmann::ordered_json vulkanTargetPlanToJson(
    const VulkanTargetPlan &plan);

} // namespace Pelican
