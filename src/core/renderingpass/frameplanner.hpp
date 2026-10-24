#pragma once

#include "renderingpass.hpp"
// RC-3c1a: the frame plan types and the planner moved to project.
#include "../../project/frameplan.hpp"
#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Pelican {

FrameGraphDefinition makeFrameGraphDefinition(const RenderingPassDefinition &definition);
FrameGraphDefinition parseFrameGraphDefinitionFromJson(const nlohmann::json &graph_json);
std::vector<FrameGraphDefinition> parseFrameGraphDefinitionsFromConfigJson(const nlohmann::json &config_json);

void validateFramePlanPassInputsProduced(
    const FramePlan &plan, const nlohmann::json &pass_set_json,
    const std::unordered_set<std::string> &buffer_names = {});
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
// PO-1: `diagnostics` also has
// {kind: "unread_output", subject: <node>, detail: {resource}} for each of
// findFramePlanUnreadOutputs, judged by the pipeline's resource usage.
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
