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

// What the lowering leaves for the stage that runs after composition. The
// config is the one lowerRenderGraphDocument returns.
struct LoweredRenderGraphDocument {
    nlohmann::json config;
    std::vector<RenderGraphTargetReference> target_refs;
    std::vector<RenderGraphHistoryRead> history_reads;
};

// Derives native render declarations from common authoring. The caller retains
// the original document; generated resource names and shader bindings are not saved.
// Operations come from registered modules; a rendering operation this backend
// cannot lower is rejected by name, not lowered as if it were a pass.
LoweredRenderGraphDocument lowerRenderGraphDocumentForComposition(const nlohmann::json &document,
    const std::set<std::string> &inactive_scopes = {},
    const GraphModuleRegistry &modules = builtinGraphModuleRegistry());
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
// history read against its target (graph.history_unavailable@1).
// Later stages add their work here, in this order, on the same entry:
//   LP-0b places graph passes at their `insert` anchors among feature passes,
//   LP-1a expands graph.loop bodies per element (passes, tasks, targets),
//   LP-0d applies pass_replacements and drops what a variant removed.
// Targets added here are not given a format_class or role by the composer;
// whatever adds one declares both.
void resolveGraphDocumentAfterComposition(nlohmann::json &composed,
    const LoweredRenderGraphDocument &lowered);
}
