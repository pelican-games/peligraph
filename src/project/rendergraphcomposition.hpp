#pragma once

#include <nlohmann/json.hpp>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "graphloop.hpp"
#include "graphmodule.hpp"
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
    // resource from outside the body.
    std::map<std::string, std::string> outputs;
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
};

// A graph pass or task -- or a node of another loop's body template -- that
// reads a loop's output `port`. The lowering spelled the resource the way the
// first non-null branch's template names it; the resolution stage re-points
// it at the last element's copy and orders the reader after that copy.
struct RenderGraphLoopOutputRead {
    std::string consumer;
    std::string loop;
    std::string port;
    std::string resource;
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
LoweredRenderGraphDocument lowerRenderGraphDocumentForComposition(const nlohmann::json &document,
    const std::set<std::string> &inactive_scopes = {},
    const GraphModuleRegistry &modules = builtinGraphModuleRegistry());
// The config alone: a pass that waits for an anchor (placed_runs) is not in it.
nlohmann::json lowerRenderGraphDocument(const nlohmann::json &document,
    const std::set<std::string> &inactive_scopes = {},
    const GraphModuleRegistry &modules = builtinGraphModuleRegistry());

// LP-0a: the graph's resolution stage. composeRenderFeatureConfig calls it once,
// after the preset, every feature, the integrations, the surface resources and
// the snapshots are composed, and before the canonical pass order, the terminal
// ordering of compute tasks and the provenance are settled. It receives the
// composed config and what the lowering recorded, and edits the config in
// place; a refusal throws GraphRegistryError.
//
// Today it checks every render.target_ref against the composed targets
// (graph.target_ref_unknown@1, graph.target_ref_type_mismatch@1) and every
// history read against its target (graph.history_unavailable@1), then
// places every run of graph passes at its anchor (LP-0b,
// placeRenderGraphPassRun). A run may name a pass another run places.
// LP-1a then expands every graph.loop (expandRenderGraphLoops below) and
// returns what it expanded. Later stages add their work here, on the same
// entry: LP-0d applies pass_replacements and drops what a variant removed.
// A target added here without a format_class or role is given both by the
// composer right after this returns, as every other target is.
RenderGraphLoopResolution resolveGraphDocumentAfterComposition(nlohmann::json &composed,
    const LoweredRenderGraphDocument &lowered);

// LP-1a: the expand stage (docs/design_graph_loops.md 5.1, 5.2). For each
// loop, in dependency order:
//   1. the elements: where -> order -> limit, each element's branch
//      (graphLoopElements); graph.loop_expansion_limit@1 past 64 elements, or
//      past 128 passes and tasks in the whole graph;
//   2. a copy of the branch body per element, every template name renamed to
//      `<loop>/<element>/<node>`; its render.target_ref / history reads are
//      checked under the copy's names;
//   3. elements that touch a resource from outside their body (a
//      render.target_ref, a loop input, an outside image or buffer), reads
//      included, wait on the previous such element with explicit `after`
//      edges, in element order -- never the name order the run is sorted in;
//   4. the loop's own `after` / `before` go to every copy;
//   5. the run is placed: at the loop's insert / canonical_anchor
//      (placeRenderGraphPassRun), or else right before the first pass that
//      depends on the loop, or at the end of its family;
// then `<loop>` in any after / before becomes the loop's copies (nothing for a
// loop with no copy), and every reader of a loop output is re-pointed at the
// last element that has a body and ordered after it
// (graph.loop_output_empty@1 when there is none).
// LP-1b plugs in at step 1 (a count from an expression) and step 2 (the
// per-element values of `$expr` / `$format`, carry, per-element inputs and
// `p[]`); LP-Vb at step 1 (a view axis's length).
RenderGraphLoopResolution expandRenderGraphLoops(nlohmann::json &composed,
    const LoweredRenderGraphDocument &lowered);
}
