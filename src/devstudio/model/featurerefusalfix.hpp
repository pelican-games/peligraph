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
    // UX-12d (engine FR-3): the feature reads the target and none of its
    // passes writes it -- a pass of the pipeline has to draw it.
    bool needs_writer = false;
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

// edit_render_targets' ops adding the targets to declare (empty when none).
// UX-12d: nothing removes them again -- a refused edit_batch wrote nothing.
nlohmann::json requiredTargetAddOps(const std::vector<FeatureRequiredTarget> &missing);

// edit_graph_document's ops adding a render.target_ref of each target's type
// to the root (`<name>_ref`, or `<name>_ref_2`... beside the ids in
// `graph_nodes`).
nlohmann::json requiredTargetReferenceOps(const std::vector<FeatureRequiredTarget> &missing,
                                          const nlohmann::json &graph_nodes);

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
// UX-12d, decision 3: and, in the same ops, set_node_parameters of each node
// of its scope that reads it through a port whose parameters.ports declare a
// type: the port takes the same type (a lighting pass's 2D depth input reads
// the 2D array map).
nlohmann::json targetRefTypeFixOps(const TargetRefTypeFix &fix, const nlohmann::json &instances);

// --- Decision 3. snapshot.after_unknown@1 {snapshot, after, candidates}.
std::optional<std::string> snapshotAfterUnknownLine(const nlohmann::json &error);

// --- Decision 5. material_route_unserved@1 on set_material_route_default.
std::optional<std::string> materialRouteUnservedLine(const nlohmann::json &error);

// --- UX-12d (engine FR-3).

// Decision 1. An output that could write a required target: a root graph
// node's port (`node` empty for a pass of the rendering section, which a
// graph edit cannot rewire), the frame pass, and what it writes now.
struct WriterCandidate {
    std::string node;
    std::string pass;
    std::string port;
    std::string resource;
};

// "render_material_pass.depth" (the pass and its port without a node).
std::string writerCandidateLabel(const WriterCandidate &candidate);

// feature.required_target_unwritten@1 {feature, target, type,
// writer_candidates: [{node?, pass, port, resource}], document}.
struct RequiredTargetUnwritten {
    std::string feature;
    FeatureRequiredTarget target;
    std::vector<WriterCandidate> candidates;
};
std::optional<RequiredTargetUnwritten> requiredTargetUnwritten(const nlohmann::json &error);

// "sky_ambient reads scene_depth, which no pass writes before it. Write it
// from render_material_pass.depth." (or "... from one of the passes below."
// for two or more, nothing more for none).
std::string requiredTargetUnwrittenLine(const RequiredTargetUnwritten &unwritten);

// Before a feature is added: the root nodes of `graph_nodes`
// (get_graph_instances) that draw an image of the kind of `type` (depth, or
// colour) into a render.image of their own -- a port bound to the image, not
// one its parameters.ports declare read. Connecting one of them to the
// target's render.target_ref makes it the target's writer; the engine judges
// the order.
std::vector<WriterCandidate> graphWriterCandidates(const nlohmann::json &type,
                                                   const nlohmann::json &graph_nodes);

// requiredTargetsQuestion with the writer named: "... Add it with the
// feature, and write it from render_material_pass.depth?" (two targets: "and
// write scene_depth from ...").
std::string requiredTargetsQuestion(const std::string &feature,
                                    const std::vector<FeatureRequiredTarget> &missing,
                                    const std::string &written, const WriterCandidate &writer);

// The edit_graph_document ops that make `writer` write `target`: a
// render.target_ref of it first when neither `graph_nodes` nor `added_ops`
// (ops of the same edit, before these) has one; then set_input of the
// writer's port from its resource. UX-13d (engine FR-4): the target_ref keeps
// the catalog's type; only a legacy image (an engine before FR-4) takes the
// type the writer's port declares, since a connection is matched exactly.
nlohmann::json writerConnectOps(const FeatureRequiredTarget &target, const WriterCandidate &writer,
                                const nlohmann::json &graph_nodes, const nlohmann::json &added_ops);

// Decision 2. edit_batch's refusal names the edit it met by `batch_index`
// (none: the composed candidate was refused).
std::optional<std::size_t> batchIndexOf(const nlohmann::json &error);

// Decision 4. A colour target a feature's pass draws that no pass reads, and
// the edits that show it: present (the pass of the same graph that draws to
// the display) reads it in place of its first input, and the pass the
// feature goes after (its insertion anchor), when a root graph node made it,
// draws its colour into the target first. UX-13d (engine FR-4): both go as
// one edit_batch, the graph edit first.
struct ShowTargetPlan {
    std::string feature;
    std::string target;
    std::string graph;
    std::string present;
    std::vector<std::string> present_inputs;
    // edit_graph_document's ops (empty: the anchor is not a graph node's).
    nlohmann::json graph_ops = nlohmann::json::array();
};
// `context` is get_render_authoring_context, `feature_reference` the
// feature's document; nullopt when every target its passes draw is read.
std::optional<ShowTargetPlan> showTargetPlan(const nlohmann::json &catalog_entry,
                                             const std::string &feature_reference,
                                             const nlohmann::json &context,
                                             const nlohmann::json &graph_nodes);
// "sky_ambient draws into lit_color, which nothing reads. Connect it to
// present to see it."
std::string showTargetLine(const ShowTargetPlan &plan);

} // namespace PelicanStudio
