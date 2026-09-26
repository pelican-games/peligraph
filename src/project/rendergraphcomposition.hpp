#pragma once

#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

#include "graphmodule.hpp"

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

// What the lowering leaves for the stage that runs after composition. The
// config is the one lowerRenderGraphDocument returns.
struct LoweredRenderGraphDocument {
    nlohmann::json config;
    std::vector<RenderGraphTargetReference> target_refs;
    std::vector<RenderGraphHistoryRead> history_reads;
    // LP-0b: graph passes waiting for their anchor. They are not in `config`.
    std::vector<RenderGraphPassRun> placed_runs;
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
// Later stages add their work here, in this order, on the same entry:
//   LP-1a expands graph.loop bodies per element (passes, tasks, targets),
//   LP-0d applies pass_replacements and drops what a variant removed.
// Targets added here are not given a format_class or role by the composer;
// whatever adds one declares both.
void resolveGraphDocumentAfterComposition(nlohmann::json &composed,
    const LoweredRenderGraphDocument &lowered);
}
