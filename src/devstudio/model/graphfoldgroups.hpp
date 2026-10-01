#pragma once

#include "graphcanvasmodel.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <functional>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// GL-3a (docs/design_graph_layers.md 3.4 - 3.6, 5.2). A fold group is a set of
// root nodes the document declares folded into one node, with the decisions
// the folded form asks of the planner -- serial, isolate, no_alias -- written
// in the group's own names. The engine maps them onto the expanded names on
// every composition, but only while the group's open form is the one that was
// verified; until then it runs the open form and the decisions are requests.
//
// Qt-free: what the Graph tab reads (get_fold_groups, the root document's
// `fold_groups`), and the fold_nodes it sends.
namespace PelicanStudio {

enum class GraphFoldField : std::uint8_t { serial, isolate, no_alias };
inline constexpr std::array<GraphFoldField, 3> graphFoldFields{
    GraphFoldField::serial, GraphFoldField::isolate, GraphFoldField::no_alias};
// "serial", "isolate", "no_alias": the document's key.
std::string_view graphFoldFieldKey(GraphFoldField field);
// "Serial", "Isolate", "No alias": what the Decisions section says.
std::string_view graphFoldFieldTitle(GraphFoldField field);

// One subject of a decision: a root node of the group, one element of a
// loop, or (no_alias only) a writer's input port.
struct GraphFoldSubject {
    std::string node;
    std::optional<std::int64_t> element;
    std::string port;

    [[nodiscard]] nlohmann::json toJson() const;
    // "blur_loop", "blur_loop [2]", "post.color".
    [[nodiscard]] std::string text() const;
    static std::optional<GraphFoldSubject> fromJson(const nlohmann::json &value);
    bool operator==(const GraphFoldSubject &) const = default;
};

struct GraphFoldGroup {
    std::string group;
    std::vector<std::string> nodes;
    std::string kind = "decisions";
    // get_fold_groups: the open form's digest now, the one verified (empty
    // without a mark), and verified / stale / unverified.
    std::string digest;
    std::string verified_digest;
    std::string state = "unverified";
    // The decisions as the root document holds them (read with
    // get_render_documents; get_fold_groups does not carry them).
    std::vector<GraphFoldSubject> serial;
    std::vector<GraphFoldSubject> isolate;
    std::vector<GraphFoldSubject> no_alias;

    [[nodiscard]] std::vector<GraphFoldSubject> &subjects(GraphFoldField field);
    [[nodiscard]] const std::vector<GraphFoldSubject> &subjects(GraphFoldField field) const;
    [[nodiscard]] bool contains(std::string_view node) const;
    // `folded` as fold_nodes takes it: {kind: "decisions", decisions: {...}}
    // with the fields that hold a subject (never `verified`).
    [[nodiscard]] nlohmann::json folded() const;
    // Decision 3: the heading's words beside the name. Unverified and stale
    // say so; verified says nothing (rule 5: marks only where it differs).
    [[nodiscard]] std::string stateLine() const;
    // The stale mark's tooltip: the two digests.
    [[nodiscard]] std::string stateTooltip() const;
    [[nodiscard]] bool stale() const noexcept { return state == "stale"; }
    bool operator==(const GraphFoldGroup &) const = default;
};

inline constexpr std::string_view graphFoldUnverifiedLine =
    "not verified \xE2\x80\x94 decisions are not applied yet";
inline constexpr std::string_view graphFoldStaleLine =
    "changed since verified \xE2\x80\x94 runs the open form";
// Decision 4: the line over the Decisions section.
inline constexpr std::string_view graphFoldDecisionsNote =
    "Decisions are requests: the engine applies them once the group is verified (Verify), "
    "and leaves them out while its nodes differ from what was verified.";

// get_fold_groups {source_digest, fold_groups: [{group, nodes, kind, digest,
// verified?, state}]}, in its order. Malformed entries are skipped.
std::vector<GraphFoldGroup> parseGraphFoldGroupsReport(const nlohmann::json &report);
// The root document's fold_groups.<group>.folded.decisions laid over the
// groups get_fold_groups named (by name). A group the document lacks keeps
// no decisions.
void readGraphFoldDecisions(std::vector<GraphFoldGroup> &groups, const nlohmann::json &root);

// Decision 1 (rule 8): the name Studio proposes. `<loop>_fold` when the
// nodes hold a loop, else fold_1, fold_2, ... -- the first that no group has.
std::string suggestGraphFoldGroupName(const std::vector<GraphFoldGroup> &groups,
                                      const std::vector<std::string> &nodes,
                                      const std::vector<GraphCanvasNode> &root);

// fold_nodes' params. `folded` null deletes the group (no nodes then).
nlohmann::json graphFoldNodesParams(const std::string &base_source_digest,
                                    const std::string &group,
                                    const std::vector<std::string> &nodes,
                                    const nlohmann::json &folded);
// A new group: no decisions yet.
nlohmann::json graphFoldNewFolded();

// Decision 4: what the Add row offers for one field of one group.
struct GraphFoldNodeChoice {
    std::string node;
    std::string op;
    // A graph.loop: its elements can be named one by one (0 .. count-1 when
    // the count is known).
    bool loop = false;
    std::optional<std::int64_t> count;
    // No alias on a writer: the input ports that take an image, a buffer or
    // a target (the resource the port is connected to).
    std::vector<std::string> ports;
    // The writer itself is not a subject (no_alias names a resource): a port
    // has to be chosen.
    bool needs_port = false;
};
// serial / isolate: every node of the group. no_alias: render.image,
// render.buffer, graph.loop, graph.instance, and the render nodes whose
// inputs read an image, a buffer or a target_ref.
std::vector<GraphFoldNodeChoice> graphFoldNodeChoices(
    const GraphFoldGroup &group, GraphFoldField field, const std::vector<GraphCanvasNode> &root,
    const std::map<std::string, std::int64_t, std::less<>> &loop_counts);

// Decision 1: why these canvas picks cannot be folded (empty: they can). A
// node of a loop's zone or a folded feature is no root node of the document.
struct GraphFoldPick {
    std::string node;
    // The zone's definition for a body node, empty at the root.
    std::string zone_scope;
    bool derived = false;
};
std::string graphFoldPickRefusal(const std::vector<GraphFoldPick> &picks);

// --- GL-4a: Verify (FG-2's verify_fold) and what it found.

// verify_fold's params. The engine's defaults (3 frames after a 30 s
// warm-up) unless `quick`: 2 frames after 1 s. The tolerance is never sent
// (0: byte equal).
nlohmann::json graphFoldVerifyParams(const std::string &base_source_digest,
                                     const std::string &group, bool quick);
inline constexpr std::string_view graphFoldQuickCheckTip =
    "Compare 2 frames after a 1 s warm-up instead of 3 after 30 s. It compares before the "
    "NVIDIA driver swaps in its optimized shaders, so a picture that still moves can read as "
    "unstable.";
// Decision 4: why a stale group runs open, and what makes it stale.
inline constexpr std::string_view graphFoldVerifyAgainTip =
    "The group's nodes changed since it was verified (a loop's Count is part of what is "
    "compared), so its decisions are left out until it is verified again.";

// What one finished verify_fold (its last get_edit_result) says.
struct GraphFoldVerifyOutcome {
    enum class Kind : std::uint8_t {
        // Committed: the group is verified, its decisions apply.
        passed,
        // graph.fold_mismatch@1: the decided form differs (red mark).
        mismatch,
        // graph.fold_verify_unstable@1: the open form differs from itself
        // (yellow mark).
        unstable,
        // Anything else: one line on the notice, no mark.
        failed,
    };
    Kind kind = Kind::failed;
    std::string group;
    // The heading's sentence (mismatch, unstable); empty otherwise.
    std::string heading;
    // The notice's line.
    std::string notice;
};
// `group` is the group the verify was asked for (a refusal names it too).
GraphFoldVerifyOutcome readGraphFoldVerifyOutcome(const nlohmann::json &response,
                                                  const std::string &group);
// Decision 2: the heading while it runs.
std::string graphFoldVerifyingLine(std::int64_t seconds);

// Decision 5: the expanded names one subject of a verified group's decision
// landed on -- the plan's fold_decisions of that group and field whose name
// came from the subject. `passes` tells which root node (and loop element)
// made a pass; `port_resource` names the resource a writer's input port is
// connected to.
std::vector<std::string> graphFoldAppliedNames(
    const std::string &group, GraphFoldField field, const GraphFoldSubject &subject,
    const std::vector<GraphCanvasFoldDecision> &decisions,
    const std::vector<GraphCanvasPlanPass> &passes,
    const std::function<std::string(const std::string &node, const std::string &port)>
        &port_resource);
inline constexpr std::string_view graphFoldNoMatchLine = "no longer matches anything";

// --- Decision 8 (EL-2 from the Frame Plan): the graph-wide decisions of the
// rendering section's target_planning, by expanded name.

struct GraphTargetPlanning {
    // target_planning.graphs.<graph>.nodes / .resources, as authored.
    nlohmann::json nodes = nlohmann::json::object();
    nlohmann::json resources = nlohmann::json::object();
    bool read = false;

    // The root document's content (its JSON object).
    static GraphTargetPlanning fromRoot(const nlohmann::json &root, const std::string &graph);
    // "node" + serial/isolate, "resource" + no_alias.
    [[nodiscard]] bool flag(std::string_view kind, std::string_view name,
                            std::string_view field) const;
};

// edit_target_planning's params: the field set true, or erased (null).
nlohmann::json graphTargetPlanningParams(const std::string &base_source_digest,
                                         const std::string &graph, std::string_view kind,
                                         const std::string &name, const std::string &field,
                                         bool on);
// A loop's copy (`<loop>/<n>/...`) cannot be named graph-wide (LP-1a): the
// line the menu shows instead.
inline constexpr std::string_view graphTargetPlanningLoopLine =
    "Fold the loop to set this per element";

} // namespace PelicanStudio
