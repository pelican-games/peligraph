#pragma once

#include "renderingpass.hpp"
#include "../../project/logicalrendergraph.hpp"
#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Pelican {

enum class FramePlanNodeKind {
    render,
    compute,
    acceleration_structure,
    anchor,
    snapshot_copy,
    output_transform,
};

enum class FrameGraphAttachmentAspect {
    color,
    depth,
};

enum class FrameGraphAttachmentLoadOp {
    load,
    clear,
    discard,
};

enum class FrameGraphAttachmentStoreOp {
    store,
    discard,
};

struct FrameGraphAttachmentDefinition {
    std::string resource;
    std::optional<ImageSubresourceRange> subresource;
    FrameGraphAttachmentAspect aspect =
        FrameGraphAttachmentAspect::color;
    FrameGraphAttachmentLoadOp load_op =
        FrameGraphAttachmentLoadOp::clear;
    FrameGraphAttachmentStoreOp store_op =
        FrameGraphAttachmentStoreOp::store;

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
    std::string ray_query_acceleration_structure = "scene";
    RenderResolutionDomain resolution_domain =
        RenderResolutionDomain::unclassified;
    std::optional<MaterialDrawTagFilter> material_filter;
    std::optional<std::string> material_variant;
    std::string view_family{
        mainRenderViewFamilyId};
    RenderViewSelect view_select;
    // Optional dialect selection carried into FrameExecutionPlan. Empty
    // preserves the legacy kind-based compatibility mapping.
    std::string semantic_dialect;
    std::string execution_implementation;
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
    size_t level = 0;
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
    std::vector<std::vector<std::string>> levels;
    std::vector<FramePlanBarrier> barriers;
};

std::string framePlanNodeKindName(FramePlanNodeKind kind);

FrameGraphDefinition makeFrameGraphDefinition(const RenderingPassDefinition &definition);
FrameGraphDefinition parseFrameGraphDefinitionFromJson(const nlohmann::json &graph_json);
std::vector<FrameGraphDefinition> parseFrameGraphDefinitionsFromConfigJson(const nlohmann::json &config_json);

FramePlan planFrameGraph(const FrameGraphDefinition &definition);
void validateFramePlanPassInputsProduced(
    const FramePlan &plan, const nlohmann::json &pass_set_json,
    const std::unordered_set<std::string> &buffer_names = {});
std::vector<std::string> framePlanOrder(const FramePlan &plan);
// GL-2a: a node a graph node made carries
//   "origin": {"graph", "node", "definition"?, "instance_path"?, "loop"?,
//              "loop_branch"?, "element"?}
// where `graph` is `graph_reference` (the rendering root document's
// reference, as the project names it), `node` the id authored in its scope,
// `definition` the definition whose body holds it (absent in the root),
// `instance_path` the instance that placed it (no trailing slash; absent in
// the root and in a loop body), and for a loop's copy `loop`, `loop_branch`
// and `element` (the same element as loop_origin). A node that took the
// place of feature passes also carries "replaces": [{"feature", "pass"}].
// RS-6: a pass the engine made wait on the graph's compute tasks (it reads
// an image they write) carries "added_after": [<task>], the names the engine
// added to its `after` (RS-6b: a feature's compute tasks too), and a render
// target whose usage the engine widened for such a reader (SAMPLED) carries
// "added_usage": [<flag>] in `resources`. RS-13: a pass of the rendering section that names a
// graph.loop's output as `<loop>.<output>` carries "loop_outputs":
// [{"reference": "<loop>.<output>", "resource": <what it reads instead>}].
// With a pipeline, the plan also carries the pipeline's `diagnostics`
// ([{kind, subject, detail}]) and `deactivated_graph_nodes`
// ([{node, feature, reason}]). FG-1: a fold group that runs its open form
// is a diagnostic {kind: "fold_stale" | "fold_unverified", subject: <group>,
// detail: {verified_digest?, current_digest}}, and the target_planning
// entries of this graph the verified fold groups added are `fold_decisions`
// ([{group, kind: "node" | "resource", name, field}]; absent when none).
nlohmann::json framePlanToJson(
    const FramePlan &plan,
    const CompiledRenderPipeline *render_pipeline = nullptr,
    std::string_view graph_reference = {});
// GL-2a: adds `from_nodes` -- the frame plan node names each element
// concerns, in the plan's node order -- to the elements of
// `physical_target_plan` that name no node themselves:
//   resources[]                    the nodes that read, write or attach it
//   alias_groups[]                 the nodes of its resources
//   decisions[] (physical_target_plan, lowering_graph,
//     planning_opportunities, backend_selection)
//                                  by `subject`: a node (a lowering node:
//                                  its sources), a resource, an alias
//                                  group, a scope; [] for a graph-wide one
//   backend_selection.candidates[] the nodes its failures' and
//                                  diagnostics' subjects name; [] when none
//                                  names one (the candidate is graph-wide)
// A plan without `physical_target_plan` is left as it is.
// FG-1: each `fold_decisions` entry joins physical_target_plan.decisions as
// {id: "pelican.plan.fold_decision_applied@1", subject: <name>, selected:
// <field>, detail, from_nodes, from_fold: <group>}.
void appendFramePlanPhysicalNodeReferences(nlohmann::json &plan_json);
void applyMaterialDrawFilterResolutionToFramePlanJson(
    nlohmann::json &plan_json,
    std::string_view pass_name,
    MaterialDrawTagFilterId filter_id,
    std::size_t resolved_draw_count,
    const std::vector<std::string> &unmatched_include,
    const std::vector<std::string> &unmatched_exclude);

} // namespace Pelican
