#pragma once

#include "computetaskdeclaration.hpp"

#include "attachmentoperations.hpp"
#include "imagesubresource.hpp"
#include "logicalrendergraph.hpp"
#include "materialdrawtag.hpp"
#include "renderresolutiondomain.hpp"
#include "viewfamilyrelation.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Pelican {

// RC-3c1a: the frame plan's types and the planner live in project (moved from
// core's frameplanner.hpp, which includes this header). They hold no Vulkan
// type. The planner's input is FrameGraphDefinition; RenderGraphIR reaches it
// through frameGraphDefinitionOf (rendergraphir.hpp).

enum class FramePlanNodeKind {
    render,
    compute,
    acceleration_structure,
    anchor,
    snapshot_copy,
    output_transform,
};

struct FrameGraphAttachmentDefinition {
    std::string resource;
    std::optional<ImageSubresourceRange> subresource;
    AttachmentAspect aspect = AttachmentAspect::color;
    // RC-3a2: resolved (attachment > pass > the pass type's defaults) and
    // never defaulted here; PassAttachmentOperations has no default.
    PassAttachmentOperations operations;

    bool operator==(
        const FrameGraphAttachmentDefinition &) const = default;
};

// A shader-visible read can state how far from the current invocation it
// accesses an image. The resource list remains separate so the legacy frame
// scheduler does not depend on target-planning semantics; this typed contract
// is consumed by the logical shadow graph and physical target compiler.
struct FrameGraphReadFootprintDefinition {
    std::string resource;
    LogicalReadFootprint footprint{
        LogicalReadFootprintKind::arbitrary,
        std::nullopt,
    };

    bool operator==(
        const FrameGraphReadFootprintDefinition &) const =
        default;
};

struct FrameGraphResourceAccessDefinition {
    // History reads retain the authored @history suffix so a current and
    // previous-epoch view of the same resource can carry distinct ports.
    std::string resource;
    LogicalAccessIntent intent =
        LogicalAccessIntent::automatic;

    bool operator==(
        const FrameGraphResourceAccessDefinition &) const =
        default;
};

struct FrameGraphNodeDefinition {
    std::string name;
    FramePlanNodeKind kind = FramePlanNodeKind::render;
    size_t declaration_index = 0;
    std::vector<std::string> reads;
    std::vector<std::string> reads_history;
    std::vector<FrameGraphReadFootprintDefinition>
        read_footprints;
    // Resource reads whose concrete pass implementation can switch between
    // a sampled descriptor and an input attachment. This prevents a future
    // same-pixel read on shadow/velocity/feature-owned inputs from being
    // fused before its shader ABI exists.
    std::vector<std::string> local_read_shader_inputs;
    std::vector<FrameGraphResourceAccessDefinition>
        resource_accesses;
    std::vector<std::string> writes;
    std::vector<std::string> after;
    std::vector<std::string> before;
    std::vector<std::string> region_tags;
    std::vector<FrameGraphAttachmentDefinition>
        attachments;
    std::string snapshot_after;
    std::size_t byte_size = 0;
    bool raster_geometry = false;
    // Material/fullscreen/generic passes declare queries before physical planning.
    // nullopt is reserved for nodes without an explicit raster query policy.
    std::optional<bool> ray_query;
    std::string ray_query_acceleration_structure{defaultRayTracingAccelerationStructure};
    RenderResolutionDomain resolution_domain =
        RenderResolutionDomain::unclassified;
    std::optional<MaterialDrawTagFilter> material_filter;
    std::optional<std::string> material_variant;
    std::string view_family{
        mainRenderViewFamilyId};
    RenderViewSelect view_select;
    // PS-1: `schedule: "once"` (a fullscreen / raster pass, a compute task).
    // Target planning isolates the node in its own physical scope; the
    // renderer records it only on a frame it is due.
    bool once = false;
};

struct FrameGraphDefinition {
    std::string name;
    std::vector<std::string> declared_resources;
    std::vector<std::string> history_resources;
    std::vector<FrameGraphNodeDefinition> nodes;
    std::vector<LogicalGraphTransformSelection>
        graph_transforms;
    std::vector<LogicalSubgraphReplacementSelection>
        subgraph_replacements;
    std::optional<RenderStrategySelection>
        render_strategy;
};

struct FramePlanNode {
    std::string name;
    FramePlanNodeKind kind = FramePlanNodeKind::render;
    size_t declaration_index = 0;
    size_t order = 0;
    std::vector<std::string> reads;
    std::vector<std::string> reads_history;
    std::vector<std::string> writes;
    std::string snapshot_after;
    std::size_t byte_size = 0;
    std::optional<MaterialDrawTagFilter> material_filter;
    std::optional<std::string> material_variant;
    std::string view_family{
        mainRenderViewFamilyId};
    RenderViewSelect view_select;
    // PS-1: FrameGraphNodeDefinition::once.
    bool once = false;
};

struct FramePlanBarrier {
    std::string kind;
    std::string resource;
    std::string from;
    std::string to;
};

struct FramePlan {
    std::string name;
    std::vector<FramePlanNode> nodes;
    std::vector<FramePlanBarrier> barriers;
};

std::string framePlanNodeKindName(FramePlanNodeKind kind);

// PO-1: a resource a node writes that nothing consumes: no later node of the
// frame reads it or writes it again (a later write may load it), and no node
// reads it as history in the next frame.
struct FramePlanUnreadOutput {
    std::string node;
    std::string resource;

    bool operator==(const FramePlanUnreadOutput &) const = default;
};

// `image_usage` is each declared image's usage (the names a render target
// declares). Only those resources are judged: a buffer, the swapchain or an
// undeclared resource may be consumed outside the plan. Also never reported:
// `display` (it reaches the screen), an image whose usage has TRANSFER_SRC
// (it may be read back or captured), and a depth image a node writes beside
// other outputs (its own depth test uses it). One entry per node and
// resource, in node order.
std::vector<FramePlanUnreadOutput> findFramePlanUnreadOutputs(
    const FramePlan &plan,
    const std::unordered_map<std::string, std::vector<std::string>>
        &image_usage);

// Orders the nodes (reads after the last earlier writer, after / before,
// declaration order among the ready ones) and states the barriers. Refuses
// by name (GraphRegistryError, "<code> <detail>"):
//   render_graph.node_name_empty@1        {graph}
//   render_graph.duplicate_node@1         {graph, node}
//   render_graph.unknown_resource@1       {graph, node, resource}
//   render_graph.unknown_history_resource@1 {graph, node, resource}
//   render_graph.order_reference_unknown@1 {graph, node, field, reference}
//   render_graph.self_dependency@1        {graph, node}
//   render_graph.ambiguous_writes@1       {graph, resource, first, second}
//   render_graph.cycle@1                  {graph}
struct FramePlanningOptions {
    // Unset preserves declaration order; zero is a valid stress seed.
    std::optional<std::uint64_t> hazard_stress_seed;
};
FramePlan planFrameGraph(const FrameGraphDefinition &definition,
                        FramePlanningOptions options = {});
std::vector<std::string> framePlanOrder(const FramePlan &plan);

// An explicit order, including the empty order of an empty graph. There is
// no default/missing state. Logical-only callers must validate their named
// sequence against the graph; production takes it from the FramePlan.
class FramePlanNodeOrder {
  public:
    explicit FramePlanNodeOrder(const FramePlan &plan);
    FramePlanNodeOrder(const CompiledLogicalRenderGraph &graph, std::vector<std::string> nodes);
    const std::string &graph() const noexcept { return graph_; }
    const std::vector<std::string> &nodes() const noexcept { return nodes_; }
    void requireGraph(const CompiledLogicalRenderGraph &graph) const;

  private:
    std::string graph_;
    std::vector<std::string> nodes_;
};

struct PhysicalNodeOrder {
    struct ScopeRange {
        std::size_t first = 0;
        std::size_t count = 0;
    };
    std::vector<std::string> nodes;
    std::vector<ScopeRange> scopes;
};

// One refusal shared by compiler completion, standalone complete-package
// verification, and direct runtime registration. Duplicate coverage wins
// over missing/extra coverage, which wins over a positional disagreement.
void requirePhysicalNodeOrderMatchesFramePlan(
    const FramePlanNodeOrder &expected, std::span<const std::string> actual);

// RC-3c1b: the refusals above that do not depend on the final order (all
// but render_graph.ambiguous_writes@1, which frame_node_order may resolve),
// for a revision before finalizeRenderGraph (a provider's, the editor's).
void requireFrameGraphStructure(const FrameGraphDefinition &definition);

// RC-2b: the structure every published FramePlan keeps, whoever made it
// (planFrameGraph, a render compiler program, a hand-built preparation).
// The runtime runs the nodes in array order and Studio shows them by
// `order`, so the two must be the same sequence. Named refusals, thrown as
// "<code>: <JSON fields>" (the view_select refusals' form):
//   {graph, node}           two nodes share a name
//   {graph, node, order, node_count}
//                           the orders are not exactly 0..node_count-1
//                           (a value repeats or reaches past the end)
//   {graph, node, order, index}
//                           the orders are 0..n-1 but the node at array
//                           position `index` carries another `order`
inline constexpr std::string_view framePlanDuplicateNodeCode =
    "frame_plan.duplicate_node@1";
inline constexpr std::string_view framePlanOrderGapCode =
    "frame_plan.order_gap@1";
inline constexpr std::string_view framePlanOrderMismatchCode =
    "frame_plan.order_mismatch@1";
void validateFramePlanStructure(const FramePlan &plan);

// FO-1 (moved from core's compiler program with RC-3c1a): refuses
// frame_node_order.conflicts_dependency@1 (GraphRegistryError) when the
// listed node indices put `first` before `second` but `second` must run
// before `first`:
//   {graph, first, second, reason: "reads" | "after" | "before" | "canonical_order",
//    resource? (reads), via? (the nodes between, when the wait is not direct),
//    anchor? (canonical_order: the canonical anchor between them)}
// `reason` (and `resource`) say why the first node after `second` on the
// shortest such path waits on it; "canonical_order" is a path through a
// canonical anchor or the output transform (the buckets' fixed order).
void requireFrameNodeOrderAgreesWithDependencies(
    const FrameGraphDefinition &graph,
    const std::vector<std::size_t> &listed);

} // namespace Pelican
