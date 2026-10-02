#pragma once

// UX-12b: refusals the engine names with what the document lacks (FR-1),
// turned into the edit that supplies it. Pure functions over the engine's
// JSON; the panels send the requests and say the lines.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace PelicanStudio {

// --- Decision 1. A feature's required_targets entry (list_render_features):
// a target the pipeline has to declare for the feature to compose.
struct FeatureRequiredTarget {
    std::string name;
    nlohmann::json type;
    std::vector<std::string> usage;
    // What the panel has to add: the render target, and the render.target_ref
    // the Graph reads it by.
    bool declare = false;
    bool reference = false;
};

// The catalog entry's required_targets, in its order. Entries without a name
// or a type object are left out.
std::vector<FeatureRequiredTarget> featureRequiredTargets(const nlohmann::json &catalog_entry);

// Whether `type` is a depth image (pelican.render.depth@1).
bool requiredTargetIsDepth(const nlohmann::json &type);

// The required targets the document lacks: those `declared` (the authoring
// context's render_targets names) does not name. Each is to be declared, and
// referenced unless a render.target_ref among `graph_nodes`
// (get_graph_instances' root nodes) already names it.
std::vector<FeatureRequiredTarget> missingRequiredTargets(
    const std::vector<FeatureRequiredTarget> &required, const std::vector<std::string> &declared,
    const nlohmann::json &graph_nodes);

// "sky_ambient needs scene_depth (depth). Add it with the feature?";
// two or more: "... needs lit_color (color) and scene_depth (depth). Add them
// with the feature?"
std::string requiredTargetsQuestion(const std::string &feature,
                                    const std::vector<FeatureRequiredTarget> &missing);

// edit_render_targets' ops adding the targets to declare (empty when none),
// and the ops removing them again.
nlohmann::json requiredTargetAddOps(const std::vector<FeatureRequiredTarget> &missing);
nlohmann::json requiredTargetRemoveOps(const std::vector<FeatureRequiredTarget> &missing);

// edit_graph_document's ops adding a render.target_ref of each target's type
// to the root (`<name>_ref`, or `<name>_ref_2`... beside the ids in
// `graph_nodes`), and the ops removing those nodes again.
nlohmann::json requiredTargetReferenceOps(const std::vector<FeatureRequiredTarget> &missing,
                                          const nlohmann::json &graph_nodes);
nlohmann::json removeAddedNodeOps(const nlohmann::json &add_ops);

// feature.required_target_missing@1 {feature, target, type, document}: the
// one target it names (declared and referenced both).
std::optional<FeatureRequiredTarget> requiredTargetFromRefusal(const nlohmann::json &error);

// --- Decision 2. A feature pass whose insertion point the pipeline does not
// have: feature.insertion_anchor_missing@1 {feature, anchor, candidates,
// document} (engine FR-2), or, from an engine without it, the composer's
// sentence "render feature pass insert anchor was not found: <anchor>".
struct FeatureInsertionAnchorMissing {
    std::string feature;
    std::string anchor;
    std::vector<std::string> candidates;
};
std::optional<FeatureInsertionAnchorMissing> insertionAnchorMissing(const nlohmann::json &error);

// FR-2's list_render_features insertion entries [{pass, anchor, position}]:
// the anchors none of `passes` names.
std::vector<std::string> missingInsertionAnchors(const nlohmann::json &catalog_entry,
                                                 const std::vector<std::string> &passes);

// "sky_ambient goes after deferred_lighting, which this pipeline does not
// have. Place Deferred lighting first." `label` names the pass a person
// places (the shader template's name when there is one, else the anchor).
std::string insertionAnchorLine(const std::string &feature, const std::string &anchor,
                                const std::string &label);

// --- Decision 4. graph.target_ref_type_mismatch@1 with `expected` {type,
// layers?}: the target_ref and the type to write in it.
struct TargetRefTypeFix {
    std::string node;
    std::string name;
    nlohmann::json type;
    std::optional<std::int64_t> layers;
};
std::optional<TargetRefTypeFix> targetRefTypeFix(const nlohmann::json &error);

// "Fix: make shadow_map_ref 2D array depth (3 layers)".
std::string targetRefTypeFixLabel(const TargetRefTypeFix &fix);

// One edit_graph_document op: the node, as get_graph_instances gives it
// (root nodes, then each definition's), with parameters.type replaced.
// Empty when the document holds no such render.target_ref.
nlohmann::json targetRefTypeFixOps(const TargetRefTypeFix &fix, const nlohmann::json &instances);

// --- Decision 3. snapshot.after_unknown@1 {snapshot, after, candidates}.
std::optional<std::string> snapshotAfterUnknownLine(const nlohmann::json &error);

// --- Decision 5. material_route_unserved@1 on set_material_route_default.
std::optional<std::string> materialRouteUnservedLine(const nlohmann::json &error);

} // namespace PelicanStudio
