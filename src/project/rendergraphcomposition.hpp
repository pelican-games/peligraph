#pragma once

#include <nlohmann/json.hpp>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "graphcomposition.hpp"
#include "graphloop.hpp"
#include "graphmodule.hpp"
#include "materialdrawloop.hpp"
#include "renderloopreport.hpp"

namespace Pelican {
// LP-0a: a render.target_ref node names a render target declared outside the
// graph (the rendering section, a preset or a feature). The lowering adds no
// target for it; every port connected to it uses `name` as the resource. Which
// targets exist is known only after the preset and the features are composed,
// so the lowering records each reference here and the resolution stage checks
// it (resolveGraphDocumentAfterComposition).
struct RenderGraphTargetReference {
    std::string node;
    std::string name;
    // The node's authored `type` ({semantic, arguments}), already canonicalized
    // by the port contract when the graph was partitioned.
    nlohmann::json type;
};

// A port that reads the previous frame of an image (`history: true`). The
// lowering names the resource `<target>@history`; the target it reads must
// keep a history, which only the composed declaration can say.
struct RenderGraphHistoryRead {
    std::string node;
    std::string port;
    std::string target;
};

// LP-0b: graph passes that land together at one anchor of one pass family.
// A graphics node that names `insert: {after|before: "<pass>"}` or
// `canonical_anchor: "<anchor>"` is not appended to its family by the
// lowering; the anchor may be a preset's or a feature's pass, which exist only
// after composition. `canonical_anchor: X` is `insert: {after: X}` for one of
// the canonical anchors (the end of that anchor's bucket). Nodes with the
// same family, side and anchor form one run, in the graph's dependency order
// (edges and `after` / `before`), and the run is placed as a whole: the first
// pass waits on the anchor (or the last is before it) and the rest follow it
// in order. A loop's expansion (LP-1a) is one such run.
struct RenderGraphPassRun {
    // The node a refusal names: the first node of the run.
    std::string node;
    // The pass family (`pass_set`, "main" when absent) the passes belong to.
    std::string family;
    bool after = true;
    // A canonical anchor name, or a pass name.
    std::string anchor;
    // Lowered pass declarations, in execution order.
    std::vector<nlohmann::json> passes;
    // LP-3b: `insert: {after|before: X, declared: true}`. The run is placed
    // among the passes the rendering section and the preset declare (and the
    // graph's appended passes), before any feature is composed
    // (placeDeclaredRenderGraphPassRuns), so the features see it where a
    // declared pass would stand: their insert may name it, their
    // pass_overrides reach it, an integration's requires_passes finds it.
    // decompose_material_pass writes its render.material_pass this way.
    bool declared = false;
};

// LP-1a: one branch body of a graph.loop, lowered once as a template. Every
// name the lowering gave it starts with `prefix` (`<loop>/$<branch>/`); the
// resolution stage copies it per element with the prefix replaced by
// `<loop>/<element>/`. None of it is in `config`.
struct RenderGraphLoopBody {
    std::string definition;
    std::string prefix;
    nlohmann::json render_targets = nlohmann::json::array();
    nlohmann::json buffers = nlohmann::json::array();
    nlohmann::json compute_tasks = nlohmann::json::array();
    // Graphics passes with their pass family, in the body's dependency order
    // (edges and after / before; ties in id order).
    std::vector<std::pair<std::string, nlohmann::json>> passes;
    std::vector<RenderGraphTargetReference> target_refs;
    std::vector<RenderGraphHistoryRead> history_reads;
    // (family, pass) the lowering isolates in target planning.
    std::vector<std::pair<std::string, std::string>> isolated;
    // Body output port -> the resource it names: a template name, or a
    // resource from outside the body (LP-1b: or a carried input's
    // placeholder, graphLoopCarryResource).
    std::map<std::string, std::string> outputs;
};

// LP-1b: the placeholder a body's declarations name a carried input `to` by.
// Each copy replaces it with the value the element receives: the loop's edge
// for the first, the previous element's `from` for the rest.
inline std::string graphLoopCarryResource(const std::string &loop, const std::string &to) {
    return loop + "/$carry/" + to;
}

// LP-1b: the first value of a carried input.
struct RenderGraphLoopCarriedInput {
    // The resource the loop's edge names.
    std::string resource;
    // Set when the edge reads another loop's output `port`: `resource` is then
    // that loop's template name, and the value is its last element's.
    std::string loop;
    std::string port;
};

// Where a loop with no insert lands in one pass family: right after (or,
// when no graph pass of the family comes before it, right before) the graph
// pass named here. The lowering declares the graph's appended passes and the
// loops in one dependency order, so the canonical order that chains each pass
// to the one declared before it agrees with the loop's edges.
struct RenderGraphLoopPosition {
    bool after = true;
    std::string pass;
};

struct RenderGraphLoop {
    GraphLoopShape shape;
    // One per branch; empty for `"body": null`.
    std::vector<std::optional<RenderGraphLoopBody>> branches;
    // By pass family; a family missing here is appended to.
    std::map<std::string, RenderGraphLoopPosition> positions;
    // LP-1b: by `to`, the carried inputs whose values are render resources.
    std::map<std::string, RenderGraphLoopCarriedInput> carried;
};

// A graph pass or task -- or a node of another loop's body template -- that
// reads a loop's output `port`. The lowering spelled the resource the way the
// first non-null branch's template names it; the resolution stage re-points
// it at the last element's copy and orders the reader after that copy.
// LP-1b: a read of one element of `p[]` carries the edge's `element` (an
// integer, or an expression object with its `$pointer`) and names the
// resource by a placeholder of its own (`<loop>/$element/<receiver>/<input>`),
// which each consumer -- each copy, when the consumer is in a loop body --
// replaces with the element's copy.
struct RenderGraphLoopOutputRead {
    std::string consumer;
    std::string loop;
    std::string port;
    std::string resource;
    // Null for `p`.
    nlohmann::json element;
    // The node and input port the edge enters, and whether that node is a
    // graph.loop (whose element the expression reads).
    std::string receiver;
    std::string input;
    bool receiver_loop = false;
};

// What the lowering leaves for the stage that runs after composition. The
// config is the one lowerRenderGraphDocument returns.
struct LoweredRenderGraphDocument {
    nlohmann::json config;
    std::vector<RenderGraphTargetReference> target_refs;
    std::vector<RenderGraphHistoryRead> history_reads;
    // LP-0b: graph passes waiting for their anchor. They are not in `config`.
    std::vector<RenderGraphPassRun> placed_runs;
    // LP-1a: every graph.loop, in id order, and whoever reads their outputs.
    std::vector<RenderGraphLoop> loops;
    std::vector<RenderGraphLoopOutputRead> loop_output_reads;
    // Passes and compute tasks the lowering made outside any loop: the
    // graph-wide expansion limit counts them with the loops' copies.
    std::size_t graph_node_count = 0;
    // LP-3a: every graph.loop over draw_items (not in `loops`: nothing of it
    // is expanded), with the render.material_pass its `draws` reach, in id
    // order. composeRenderFeatureConfig turns them into the pipeline's
    // material_draw_loops.
    std::vector<GraphDrawLoop> draw_loops;
    // LP-0d: their names (each is its node's id), appended or placed. A
    // feature instance's pass_replacements may name one of these or a loop.
    std::set<std::string> graph_declarations;
    // GL-2a: where the node that made each pass or compute task is authored
    // (the expansion's origin), by the name the lowering gave it: the
    // expanded id for a declaration of the root or an instance, the
    // template id `<loop>/$<branch>/<node>` for a loop body's.
    std::map<std::string, GraphNodeOrigin> origins;
};

// LP-0d: one entry of a feature instance's `pass_replacements`
// ({"<pass or compute task of the feature>": "<graph node id or graph.loop id>"}).
struct RenderFeaturePassReplacement {
    // The feature's name (its envelope's `name`).
    std::string feature;
    std::string pass;
    std::string node;
};

// LP-0d: what composition hands the resolution stage about the features.
struct RenderGraphFeatureContext {
    // The pass_replacements of the features that were composed.
    std::vector<RenderFeaturePassReplacement> replacements;
    // Pass / compute task name -> the feature that declared it (an
    // integration's passes count as its provider's).
    std::map<std::string, std::string> feature_passes;
    // The composed surface_resources declarations ({contract, resource,
    // producer, ...}); a replaced producer is re-pointed here.
    nlohmann::json *surface_resource_contracts = nullptr;
    // What the graph variant excluded: pass / compute task name and render
    // target / buffer name -> the excluded feature, and the pass_replacements
    // the excluded features' instances named.
    std::map<std::string, std::string> excluded_passes;
    std::map<std::string, std::string> excluded_resources;
    std::vector<RenderFeaturePassReplacement> excluded_replacements;
    // LP-Vb: the length of a view family's axis, for a graph.loop over
    // view_axis (the composer binds the engine's lookup to what it composed).
    // Empty: such a loop cannot be sized and is refused.
    std::function<RenderGraphViewAxisLength(std::string_view family, std::string_view axis)> view_axis_length;
};

// LP-1a: what the resolution stage expanded. The frame plan reports `loops`
// (get_frame_plan's top-level `loops[]`) and each expanded declaration's
// origin (`loop_origin`).
struct RenderGraphLoopResolution {
    std::vector<RenderGraphLoopReport> loops;
    // Passes and compute tasks a loop added, by name.
    std::map<std::string, RenderLoopOrigin> passes;
    // Render targets and buffers a loop added, by name.
    std::map<std::string, RenderLoopOrigin> resources;
    // GL-2a: each graph pass or task that took the place of feature passes
    // (applyRenderFeaturePassReplacements), with the entries it replaced:
    // those of its own pass family (tasks: the tasks), in the order named.
    std::map<std::string, std::vector<RenderFeaturePassReplacement>> replacements;
};

// LP-0b: places one run in the composed config (see RenderGraphPassRun). The
// anchor is looked for as a feature's insert looks for it, but only in the
// run's own family: found only in another family, it is refused as
// graph.insert_family_mismatch@1 {node, anchor}; found nowhere, as
// graph.insert_anchor_unknown@1 {node, anchor}. A pass whose name is already
// taken is refused. LP-1a hands each loop's expanded passes here as one run.
void placeRenderGraphPassRun(nlohmann::json &composed, const RenderGraphPassRun &run);

// Derives native render declarations from common authoring. The caller retains
// the original document; generated resource names and shader bindings are not saved.
// Operations come from registered modules; a rendering operation this backend
// cannot lower is rejected by name, not lowered as if it were a pass.
// LP-1e: a render node or loop body pass with no `pass_set` joins
// `default_pass_family` -- the composed config's first pass family, which
// composeRenderFeatureConfig works out (defaultGraphPassFamily); `main` only
// when the config has none.
LoweredRenderGraphDocument lowerRenderGraphDocumentForComposition(const nlohmann::json &document,
    const std::set<std::string> &inactive_scopes = {},
    const GraphModuleRegistry &modules = builtinGraphModuleRegistry(),
    const std::string &default_pass_family = "main");
// The config alone: a pass that waits for an anchor (placed_runs) is not in it.
nlohmann::json lowerRenderGraphDocument(const nlohmann::json &document,
    const std::set<std::string> &inactive_scopes = {},
    const GraphModuleRegistry &modules = builtinGraphModuleRegistry());

// LP-0a: the graph's resolution stage. composeRenderFeatureConfig calls it once,
// after the preset, every feature, the integrations, the surface resources and
// the snapshots are composed, and before the canonical pass order, the terminal
// ordering of compute tasks and the provenance are settled. It receives the
// composed config and what the lowering recorded -- already without the nodes a
// variant made inactive (withoutExcludedGraphDependents) -- and edits the
// config in place; a refusal throws GraphRegistryError.
//
// Today it checks every render.target_ref against the composed targets
// (graph.target_ref_unknown@1, graph.target_ref_type_mismatch@1) and every
// history read against its target (graph.history_unavailable@1). The runs of
// graph passes were placed earlier (placeRenderGraphPassRuns, LP-3a).
// LP-1a then expands every graph.loop (expandRenderGraphLoops below) and
// returns what it expanded. LP-1b: a refusal there whose `node` is one
// element's copy of a body node (`<loop>/<element>/<node>`) also carries
// `loop_origin` {loop, element, node}, the frame plan's shape.
// LP-3a: a graph.loop over draw_items is reported, and an after / before
// naming it names its render.material_pass.
// LP-0d, after the expansion: every pass_replacements entry of
// features.replacements is applied (applyRenderFeaturePassReplacements below).
// A replacement lands after the surface resources picked their consumers: a
// replaced producer is re-pointed, a replaced consumer leaves the consumer
// lists, and (LP-Vb, design 14.7) the composer picks the consumers among the
// loops' copies right after this returns, as it picked them among the
// composed passes (a node that is no loop was among those already).
// A target added here without a format_class or role is given both by the
// composer right after this returns, as every other target is.
RenderGraphLoopResolution resolveGraphDocumentAfterComposition(nlohmann::json &composed,
    const LoweredRenderGraphDocument &lowered, const RenderGraphFeatureContext &features = {});

// LP-0d: the first step of the graph's stages after composition, before the
// runs are placed (placeRenderGraphPassRuns) and so before the surface
// resources pick their consumers. A graph node that depends on a feature the
// variant excluded (features.excluded_*) is made inactive -- it is not
// declared, its references are not checked, and every after / before naming
// it is dropped, as for a node of an inactive scope -- and appended to
// `deactivated` (see RenderGraphDeactivatedNode for when). A loop made
// inactive takes whoever reads its outputs with it; a draw_loop (LP-3a) goes
// with its render.material_pass. Returns the lowered document without them;
// the later stages take that one.
LoweredRenderGraphDocument withoutExcludedGraphDependents(nlohmann::json &composed,
    const LoweredRenderGraphDocument &lowered, const RenderGraphFeatureContext &features,
    std::vector<RenderGraphDeactivatedNode> &deactivated);

// LP-0d: takes each replaced feature pass or compute task out of the composed
// config and puts the graph node (or the loop's expanded passes and tasks,
// in their declared order) in its place. For each replacement node, over
// every entry that names it:
//   - the pass must be one of the feature's own passes or tasks, or
//     render_feature.pass_replacement_unknown@1 {feature, pass}; the node a
//     graph pass / task or a graph.loop, or graph.node_missing@1
//     {node, feature, pass};
//   - what the replaced passes wrote, the node must write (a loop: any of its
//     copies), or it is refused as the surface_resources producer check
//     refuses a producer that "no longer writes resource" it;
//   - the node's passes of the replaced pass's family (its tasks, for a
//     task) move to where the first replaced one stood; with none there,
//     graph.insert_family_mismatch@1 {node, anchor: pass}, or a refusal when
//     it has no pass (task) at all;
//   - every after / before naming a replaced pass names the node instead:
//     `after` its last pass or task the holder may order against, `before`
//     its first; the replaced passes' own after / before go to the first /
//     last of the node's that took their place; snapshot_after,
//     frame_node_order (the node's passes and tasks, in order) and
//     surface_resources producers (the node's last pass that writes the
//     contract's resource) follow; the replaced names leave target_planning.
// `loop_passes` are the expanded copies (RenderGraphLoopResolution::passes).
// GL-2a: `replaced_by`, when given, receives each moved pass or task of a
// node with the entries of its family it took the place of.
void applyRenderFeaturePassReplacements(nlohmann::json &composed, const LoweredRenderGraphDocument &lowered,
    const std::map<std::string, RenderLoopOrigin> &loop_passes, const RenderGraphFeatureContext &features,
    std::map<std::string, std::vector<RenderFeaturePassReplacement>> *replaced_by = nullptr);

// LP-0b / LP-3a: places every run of graph passes at its anchor
// (placeRenderGraphPassRun); a run may name a pass another run places.
// composeRenderFeatureConfig calls it once every feature and integration has
// inserted its passes and BEFORE the surface resources pick their consumers,
// so a placed pass -- a render.material_pass in particular -- is picked like
// the preset's own (the shadow map reaches it). A placed pass is therefore
// in place before the snapshots are materialized and before any
// render.target_ref is checked. `lowered` is the document
// withoutExcludedGraphDependents returned (LP-0d).
void placeRenderGraphPassRuns(nlohmann::json &composed, const LoweredRenderGraphDocument &lowered);

// LP-3b: places the runs whose insert says `declared: true` in `config` --
// the preset resolved with the rendering section and the graph's appended
// passes, before any feature -- and takes them out of lowered.placed_runs;
// placeRenderGraphPassRuns places the rest later. The anchor must be there
// already (graph.insert_anchor_unknown@1 / graph.insert_family_mismatch@1
// otherwise, as for any run): a feature's pass cannot be one.
void placeDeclaredRenderGraphPassRuns(nlohmann::json &config, LoweredRenderGraphDocument &lowered);

// RS-3: where a run of graph passes with no place of its own (the graph's
// appended passes of one family, or a loop's copies) has to stand among one
// family's `passes`: right before the first of them that `scene_pass` admits
// (and that stands before `end`) which reads an image the run writes that no
// such pass before it writes, nor any compute task of `config`. "Reads" is
// LP-6's: an `input` or `reads` entry, a resource port that does not write, a
// snapshot copy's source; a history read is not one. Every other placement
// keeps the reader behind a producer already, so only a composition the
// frame planner refuses today (the reader before the writer) moves.
// std::nullopt when nothing reads the run so, or when the run waits on the
// reader or a later pass (its `after` names it, its `before` names the run
// -- `run_names` -- or it writes an image the run reads): the run then stays
// where it is, and the reader is refused as before.
// RS-6: a compute task of `config` that waits on the run (its `after` names a
// pass of the run or such a task, or their `before` names it) hands on what
// the run wrote: a scene pass that reads an image such a task writes (and no
// other task writes) reads the run through it, and the run stands before
// that reader too when it comes first -- unless the run or such a task waits
// on that reader or a later pass (what such a task reads and waits on counts
// for this reader only). The reader's wait on the task is
// waitOnGraphTasksInScenePasses's.
std::optional<std::size_t> graphPassRunReaderPosition(const nlohmann::json &config, const nlohmann::json &passes,
    const std::vector<const nlohmann::json *> &run, const std::set<std::string> &run_names,
    const std::function<bool(const nlohmann::json &)> &scene_pass, std::size_t end);

// RS-6: an `after` waitOnGraphTasksInScenePasses added.
struct GraphTaskReaderWait {
    std::string family;
    std::string pass;
    std::string task;
};

// RS-6: a scene pass that reads an image a graph compute task writes waits on
// that task. For each task named in `graph_tasks` and each render target it
// writes, the first pass of each family that `reader_candidate` admits, that
// stands before the family's first canonical anchor (or output_transform)
// and that reads the image this frame (LP-6's reads, as RS-3: no history
// read) gets `after: <task>` -- unless a pass before it writes the image, a
// writer of the image (the task included) already runs before it, or the
// task runs after it (an after would close a cycle). The task gets nothing:
// it is in every family's graph and may name no pass. When the reader lists
// the image as an `input` and the image is one of `graph_images` (a graph's
// render.image), SAMPLED joins its usage. Called on the composed config once
// the canonical order and the terminal's waits are explicit. Returns what it
// added.
std::vector<GraphTaskReaderWait> waitOnGraphTasksInScenePasses(nlohmann::json &composed,
    const std::set<std::string> &graph_tasks, const std::set<std::string> &graph_images,
    const std::function<bool(const nlohmann::json &)> &reader_candidate);

// LP-1a: the expand stage (docs/design_graph_loops.md 5.1, 5.2). For each
// loop, in dependency order:
//   1. the elements: the count (LP-1b: an expression is evaluated with the
//      composed targets' mip_levels_of / layers_of), where -> order -> limit,
//      each element's branch (graphLoopElements); graph.loop_expansion_limit@1
//      past 64 elements, or past 128 passes and tasks in the whole graph;
//   2. a copy of the branch body per element, every template name renamed to
//      `<loop>/<element>/<node>`. LP-1b: every `$expr` / `$format` in it is
//      evaluated with the element, its index and the count; a carried input
//      becomes the value the element receives (the loop's edge first, then
//      the previous element's `from`; an element with no body passes it on).
//      Its render.target_ref / history reads are checked under the copy's
//      names;
//   3. elements that touch a resource from outside their body (a
//      render.target_ref, a loop input, an outside image or buffer, a carried
//      value, another loop's element), reads included, wait on the previous
//      such element with explicit `after` edges, in element order -- never
//      the name order the run is sorted in;
//   4. the loop's own `after` / `before` go to every copy;
//   5. the run is placed: at the loop's insert / canonical_anchor
//      (placeRenderGraphPassRun), or else right before the first pass that
//      depends on the loop, or at the end of its family;
// then `<loop>` in any after / before becomes the loop's copies (nothing for a
// loop with no copy), and every reader of a loop output is re-pointed and
// ordered after what writes it: `p` at the last element that has a body
// (graph.loop_output_empty@1 when there is none, unless `p` is a carry's
// `from`: then the carried value, the loop's edge when no element ran);
// LP-1b: `p[]` at the element its edge's `element` names, evaluated per
// receiving element (graph.loop_element_unknown@1 {node, port, element} --
// the receiver and its input -- when no element with a body has that value).
// LP-Vb, step 1 for a loop over view_axis: the count is the axis's length
// (features.view_axis_length); an axis no provider of the family declares is
// graph.view_select_axis_unknown@1 {pass: <loop>, family, axis}, an axis
// known only per frame graph.loop_tier_unsupported@1 {node, reason:
// set_unsupported}.
RenderGraphLoopResolution expandRenderGraphLoops(nlohmann::json &composed,
    const LoweredRenderGraphDocument &lowered, const RenderGraphFeatureContext &features = {});
}
