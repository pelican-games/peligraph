#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// FG-1: fold groups (docs/design_graph_layers.md 3.5, 3.6). A rendering root
// graph declares, beside its nodes, fragments of its own root nodes that are
// folded into one node:
//
//   "fold_groups": {
//     "<group>": {
//       "nodes": ["<root node id>", ...],
//       "folded": {
//         "kind": "decisions",
//         "decisions": {"serial": [<subject>...], "isolate": [...], "no_alias": [...]},
//         "verified": {"digest": "<sha256 hex>", "at": "<time>", "frames"?: ...}
//       }
//     }
//   }
//
// A subject is {"node": "<root node id>", "element"?: N, "port"?: "<port>"}.
// A loop or an instance stands for everything it expands into (a loop's
// `element`: that element only); no_alias may also name a render.image /
// render.buffer node, or a writer's {node, port} (the image or buffer the
// writer's input `port` is connected to). The decisions are written in these
// local names only: every composition maps them onto the expanded names
// (applyGraphFoldGroups) and adds them to target_planning, so a document stays
// right when a loop's count changes.
//
// The decisions are used only while the group's open form is the one that
// was verified: graphFoldGroupDigest of the group's nodes equals
// verified.digest. Otherwise the composition runs the open form and reports
// fold_stale (or fold_unverified when there is no verified mark).
namespace Pelican {

struct RenderPassProvenance;
struct RenderResourceProvenance;

// Named refusals (GraphRegistryError codes):
//   kind other than "decisions" ("replacement" is FG-3): {group, kind}
inline constexpr std::string_view graphFoldKindUnsupportedCode = "graph.fold_kind_unsupported@1";
//   a subject whose node is not one of the group's nodes: {group, subject}
inline constexpr std::string_view graphFoldSubjectOutsideCode = "graph.fold_subject_outside@1";
//   a fold decision against the same name's explicit opposite (an authored
//   target_planning entry that says false): {group, name, field}
inline constexpr std::string_view graphFoldDecisionConflictCode = "graph.fold_decision_conflict@1";

struct GraphFoldSubject {
    std::string node;
    std::optional<std::int64_t> element;
    std::optional<std::string> port;

    bool operator==(const GraphFoldSubject &) const = default;
};

struct GraphFoldGroup {
    std::string group;
    std::vector<std::string> nodes;
    std::string kind;
    std::vector<GraphFoldSubject> serial;
    std::vector<GraphFoldSubject> isolate;
    std::vector<GraphFoldSubject> no_alias;
    // As authored ({digest, at, frames?}); absent when the group was never
    // verified.
    std::optional<nlohmann::json> verified;
};

// The document's fold_groups, in the document's key order (nlohmann::json
// sorts them). Shape errors (an unknown key, a node that is no string, a
// verified without a digest ...) throw std::invalid_argument; a kind other
// than "decisions" and a subject outside the group's nodes are the named
// refusals above. Whether a node exists, and what op it is, is not checked
// here: a group whose node went away is stale, not malformed.
std::vector<GraphFoldGroup> parseGraphFoldGroups(const nlohmann::json &document);

// The canonical digest of a group's open form: for each of `nodes` (sorted;
// null for one the document does not have) the root node with only the
// inputs that come from another of `nodes`, and every definition those nodes
// use (an instance's, a loop's branch bodies, transitively) without its
// `layout`. The root's `layout` is not part of it. sha256 hex of the sorted-
// key JSON dump, as the scene digests are made.
std::string graphFoldGroupDigest(const nlohmann::json &document, std::span<const std::string> nodes);

enum class GraphFoldState { verified, stale, unverified };
std::string_view graphFoldStateName(GraphFoldState state);

struct GraphFoldGroupReport {
    std::string group;
    std::vector<std::string> nodes;
    std::string kind;
    std::string digest;
    std::optional<nlohmann::json> verified;
    GraphFoldState state = GraphFoldState::unverified;

    bool operator==(const GraphFoldGroupReport &) const = default;
};

std::vector<GraphFoldGroupReport> reportGraphFoldGroups(const nlohmann::json &document,
                                                        std::span<const GraphFoldGroup> groups);
// get_fold_groups' entries: {group, nodes, kind, digest, verified?, state}.
nlohmann::ordered_json graphFoldGroupReportToJson(const GraphFoldGroupReport &report);

// One decision a verified group added to target_planning, by the expanded
// name: target_planning.graphs.<graph>.nodes.<name>.<field> (field "serial"
// or "isolate") or .resources.<name>.no_alias.
struct GraphFoldPlanningEntry {
    std::string group;
    std::string graph;
    // "node" or "resource".
    std::string kind;
    std::string name;
    std::string field;

    bool operator==(const GraphFoldPlanningEntry &) const = default;
};

struct GraphFoldApplication {
    std::vector<GraphFoldGroupReport> groups;
    std::vector<GraphFoldPlanningEntry> planning;
};

// The composition's step (composeRenderFeatureConfig, after the provenance
// knows which graph node made each pass and task -- GL-2a's graph_origin --
// and before the config is handed to the target planner). `document` is the
// authored root, `composed` the composed config. For every verified group,
// each subject is mapped through the provenance onto the passes, compute
// tasks, render targets and buffers it expanded into, and the decision is
// set true in composed target_planning: a pass in its family's graph, a
// compute task and a resource in every family's (they are in every frame
// graph). An entry the composed config already holds as false is refused
// (graph.fold_decision_conflict@1). A subject that maps to nothing (an
// element the loop no longer has) adds nothing. A document without
// fold_groups changes nothing.
GraphFoldApplication applyGraphFoldGroups(const nlohmann::json &document, nlohmann::json &composed,
                                          const std::vector<RenderPassProvenance> &passes,
                                          const std::vector<RenderResourceProvenance> &resources);

// EL-2's check of an explicit value against what the fold groups decided:
// the entry of `entries` that sets graph / kind / name / field true when
// `value` is false, or nullopt.
std::optional<GraphFoldPlanningEntry> graphFoldDecisionConflict(std::span<const GraphFoldPlanningEntry> entries,
                                                                std::string_view graph, std::string_view kind,
                                                                std::string_view name, std::string_view field,
                                                                bool value);

// The refusal of such a conflict, as GraphRegistryError's fields.
nlohmann::json graphFoldDecisionConflictDetail(const GraphFoldPlanningEntry &entry);

} // namespace Pelican
