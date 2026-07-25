#pragma once

#include "samplecountplanning.hpp"
#include "targetplanning.hpp"
#include "vulkanviewplanning.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace Pelican {

enum class ResourcePatternFallback : std::uint8_t {
    materialize,
    reject,
};

std::string_view resourcePatternFallbackName(
    ResourcePatternFallback fallback);

struct ResourceFormatCandidate {
    std::string format;
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
};

enum class ResourceExtentKind : std::uint8_t {
    output_relative,
    fixed,
};

std::string_view resourceExtentKindName(ResourceExtentKind kind);

// Typed physical-size contract carried beside a resource pattern. It is
// intentionally binding-specific: two resources using the same format and
// materialization pattern may live at different resolutions.
struct ResourceExtentPlan {
    ResourceExtentKind kind =
        ResourceExtentKind::output_relative;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool operator==(const ResourceExtentPlan &) const = default;
};

struct ResourcePatternBinding {
    std::string resource;
    ResourcePattern pattern;
    std::optional<ResourceExtentPlan> extent;
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
    std::span<const std::string> node_order = {});
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
};

std::string_view vulkanResourceViewLayoutName(
    VulkanResourceViewLayout layout);

struct VulkanPhysicalResourcePlan {
    std::string logical_resource;
    std::string pattern;
    std::string format;
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
    std::uint32_t array_layers = 1;
    std::optional<ResourceExtentPlan> extent;
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
    std::vector<std::string> local_reads;
    std::vector<std::string> region_tags;
    std::uint32_t rasterization_samples = 1;
    VulkanScopeViewExecution view_execution =
        VulkanScopeViewExecution::single_view;
    std::uint32_t view_count = 1;
    std::uint32_t execution_count = 1;
    std::uint32_t view_mask = 0;
};

struct VulkanAliasGroupPlan {
    std::string id;
    std::vector<std::string> resources;
};

struct VulkanSampleCountPlanRequest {
    SampleCountPolicy policy;
    std::vector<SampleCountResourceCapability> capabilities;
    std::vector<std::string> geometry_nodes;
};

struct VulkanTargetPlanRequest {
    std::string endpoint;
    std::string provider;
    std::vector<ResourcePatternBinding> pattern_bindings;
    PlanningProfile profile;
    std::vector<PlanningNodeConstraint> node_constraints;
    std::vector<PlanningResourceConstraint> resource_constraints;
    PlanningDiagnosticPolicy diagnostic_policy;
    std::optional<VulkanSampleCountPlanRequest> sample_count;
    std::optional<VulkanViewExecutionPlanRequest> view_execution;
};

struct VulkanTargetPlan {
    std::string graph;
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
    std::vector<VulkanAliasGroupPlan> alias_groups;
    std::vector<std::string> required_physical_features;
    std::vector<PlanningDecision> decisions;
    std::optional<ResolvedSampleCountPlan> sample_count_plan;
    VulkanViewExecutionPlan view_execution_plan;
    std::optional<VulkanRenderResolutionPlan>
        resolution_plan;
};

void validateVulkanPhysicalFeatureClosure(
    const BackendProbeResult &selected_probe,
    std::span<const std::string> lowered_required_features);
VulkanTargetPlan compileVulkanTargetPlan(
    const LogicalTypeRegistry &types,
    const CompiledLogicalRenderGraph &canonical_graph,
    const TargetTopologySnapshot &topology,
    const CompilerProviderRegistrySnapshot &providers,
    VulkanTargetPlanRequest request);
nlohmann::ordered_json vulkanTargetPlanToJson(
    const VulkanTargetPlan &plan);

} // namespace Pelican
