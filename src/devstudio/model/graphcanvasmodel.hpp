#pragma once

#include "inspectormodel.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PelicanStudio {

// One input port of one node as the canvas draws it. Where the port list came
// from is part of what is shown: a declared contract and a key read off the
// authored node are not the same claim, and the canvas must not pretend the
// second is the first.
struct GraphCanvasPort {
    std::string port;
    // The semantic type id the catalog says flows through this port, e.g.
    // "pelican.value.scalar@1". Empty when nothing declared one -- which the
    // canvas draws as the unknown socket rather than guessing a type.
    std::string type;
    // The operation catalog declared this port; otherwise it was read from the
    // authored document (the node's own inputs/parameters, or, for an
    // instance, the definition's graph.input nodes).
    bool declared = false;
    bool required = false;
    std::string accepted_type;
    // The authored edge into this port, when there is one.
    bool connected = false;
    std::string source_node;
    std::string source_port;
    // LP-4c: an edge that reads one element of a loop's `p[]` names the
    // element; this is that `element` as the document holds it (dumped), or
    // empty for an ordinary edge.
    std::string source_element;
    // The authored scalar stored on the node for this port.
    bool has_parameter = false;
    nlohmann::json parameter;
    // What the published generation actually fed the port. Only root-scope
    // graph.instance nodes can carry this: get_graph_values answers for an
    // instance, never for a node inside a definition (AG-3).
    bool has_value = false;
    double value = 0;
    // "connected" | "parameter" | "default" | "override"
    std::string value_source;
    std::string value_connected_node;
    std::string value_connected_port;
};

// One output port as the canvas draws it: the name the document connects to
// and, when the catalog declared it, the type that leaves it.
struct GraphCanvasOutput {
    std::string port;
    std::string type;
    // What the published generation's evaluation left on this port, read from
    // get_graph_node_values. Unlike the input side, this reaches every scope:
    // the node values answer for a definition body as well as for the root.
    bool has_value = false;
    double value = 0;
    // LP-4c: a loop's `p[]` -- one value per element, not one value. `type`
    // is then the type of each element.
    bool bundle = false;

    bool operator==(const GraphCanvasOutput &) const = default;
};

struct GraphCanvasNode {
    std::string id;
    std::string op;
    // The authored, human-chosen name for this node. Any node may carry a
    // `label`; when it does not, the heading falls back to the id.
    std::string label;
    // From the catalog. Empty when the catalog has not arrived or does not
    // know the operation -- which is a visible state, not a default.
    std::string domain;
    std::string evaluation;
    bool sink = false;
    bool contract_known = false;
    // Set for graph.instance nodes; names the definition scope to descend to.
    // For a graph.loop it is the body a descent lands in (the branch chosen
    // for it, else the first branch that has a body).
    std::string definition;
    bool instance = false;
    // LP-4c: a graph.loop. Its input ports are its first body's graph.input
    // nodes and its outputs the `p` / `p[]` it declares; its parameters are
    // the Loop fields, never ports.
    bool loop = false;
    // What the heading shows beside the name for a loop: the mark and the
    // tier with its count when the report has them ("expand x3"), or "loop".
    std::string badge;
    // LP-4c, decision 9: the last refused edit named this node.
    bool refused = false;
    std::string refusal;
    bool has_owner = false;
    std::string owner_scene;
    std::uint64_t owner_object = 0;
    nlohmann::json parameters = nlohmann::json::object();
    std::vector<GraphCanvasPort> inputs;
    std::vector<GraphCanvasOutput> outputs;

    // What the heading shows in bold. The id is the fallback, not a second
    // label: a node without one is named by its id.
    [[nodiscard]] const std::string &heading() const noexcept {
        return label.empty() ? id : label;
    }
};

struct GraphCanvasEdge {
    std::string from_node;
    std::string from_port;
    std::string to_node;
    std::string to_port;
    // LP-4d2, decision 4: an edge that reads one element of a loop's `p[]`
    // says which, as the label on its line shows it ("element", "2");
    // empty for an ordinary edge.
    std::string element = {};
};

// LP-4d1: one output of a definition body -- its name and the port inside
// that makes it -- which a loop zone's right edge draws.
struct GraphCanvasBodyOutput {
    std::string name;
    std::string node;
    std::string port;
};

// A scope is either the root document or one definition body. The root scope
// carries the empty id so a definition named "root" stays addressable.
struct GraphCanvasScope {
    std::string id;
    std::string label;
    bool root = false;
};

// Q-8. One node of the project's UI document, named the way a `ui.value` or
// `ui.visible` sink names it: the ids from the root joined by "/" (the same
// string the UI's emit calls stable_id). `type` is the widget type, which is
// what decides whether a value can be written to it at all.
struct GraphCanvasUiNode {
    std::string path;
    std::string type;

    bool operator==(const GraphCanvasUiNode &) const = default;
};

// Every node of one UI document, in document order. A node without a string
// id has no path and is skipped, with its children.
std::vector<GraphCanvasUiNode> graphCanvasUiDocumentNodes(const nlohmann::json &document);

struct GraphCanvasUiNodesRead {
    std::vector<GraphCanvasUiNode> nodes;
    // Why there are none, when there are none for a reason worth saying:
    // project.json names no UI document, or the one it names cannot be read.
    std::string error;
};

// The UI document project.json's basic_config.ui_config_json names, read the
// way the engine reads it (through the project path resolver), and walked.
// No engine RPC answers with the UI tree, and the document is the source of
// truth for the paths anyway: an unknown path is refused by the engine by name.
GraphCanvasUiNodesRead readProjectUiNodes(const std::filesystem::path &project_root);

// Q-10. The sounds a project holds, as the `asset` of an audio.playback node
// names them ("project://sounds/hit.wav"). No engine RPC lists them -- they
// are files the node names directly, not asset_data entries -- so the Studio
// walks the project for the two extensions AU-2 plays (.wav and .ogg, in any
// case), skipping dot directories (.pelican and the like). Sorted.
struct GraphCanvasAudioAssetsRead {
    std::vector<std::string> assets;
    // Why there are none, when there are none for a reason worth saying.
    std::string error;
};
GraphCanvasAudioAssetsRead readProjectAudioAssets(const std::filesystem::path &project_root);

// One operation the "Add node..." palette can offer in the current scope.
// `structural` marks graph.input and graph.instance: the catalog does not
// declare their ports, and each belongs to one side of the scope boundary.
struct GraphCanvasCatalogEntry {
    std::string op;
    std::string domain;
    std::string evaluation;
    bool sink = false;
    bool structural = false;

    bool operator==(const GraphCanvasCatalogEntry &) const = default;
};

// LP-4e. A material's tags as a draw_loop's `where` and a branch's `when`
// test them ({"tags": {include, exclude, require_all}}); empty lists are left
// out of the document.
struct GraphCanvasTagFilter {
    std::vector<std::string> include;
    std::vector<std::string> exclude;
    std::vector<std::string> require_all;

    [[nodiscard]] bool empty() const noexcept {
        return include.empty() && exclude.empty() && require_all.empty();
    }
    bool operator==(const GraphCanvasTagFilter &) const = default;
};

// LP-4e. A draw_loop branch's condition: any of the material's attributes,
// all of them to hold (AND). An empty condition is the else.
struct GraphCanvasDrawWhen {
    // The material's matching key (`<document>#<name>`, `<asset>#material[i]`).
    std::optional<std::string> material;
    std::optional<GraphCanvasTagFilter> tags;
    // The material's shader contract: gbuffer_v1, forward_scene_color_v1 or
    // legacy_gbuffer_v1.
    std::optional<std::string> contract;
    std::optional<bool> skinned;
    std::optional<bool> has_variant;

    [[nodiscard]] bool empty() const noexcept {
        return !material && !tags && !contract && !skinned && !has_variant;
    }
    bool operator==(const GraphCanvasDrawWhen &) const = default;
};

// LP-4c. One branch of a graph.loop as the branch table shows it.
struct GraphCanvasLoopBranch {
    // "else" (no `when`), "element", "element_in", "element_not_in", or
    // "other" for a condition the table has no form for (an expression).
    // LP-4e: "draw" for a draw_loop's condition, read into `draw_when`.
    std::string when = "else";
    std::vector<std::int64_t> values;
    GraphCanvasDrawWhen draw_when;
    // The definition the branch runs; empty with `nothing` for `"body": null`.
    std::string body;
    bool nothing = false;
    // From get_frame_plan's loops[]: the elements that took this branch, and
    // whether none did (`unmatched_branches`). Empty without a report.
    std::vector<std::int64_t> matched;
    // LP-4e: a draw_loop's elements are materials, by their matching key.
    std::vector<std::string> matched_materials;
    bool unmatched = false;
};

struct GraphCanvasLoopCarry {
    std::string from;
    std::string to;
    // The carried input has no loop input edge, which the engine refuses:
    // its first element would have nothing to start from.
    bool to_unbound = false;
};

// LP-4c. What the Loop fields show for one root graph.loop: its parameters
// read into the fields' own terms, its body's boundary, and the report.
struct GraphCanvasLoop {
    std::string id;
    // over.range.count: an integer, or an expression ({"$expr": ...}).
    bool count_expression = false;
    std::int64_t count = 0;
    std::string count_text;
    bool descending = false;
    std::optional<std::int64_t> limit;
    // "" (automatic), "expand" or "draw_loop".
    std::string tier;
    // "" (none), "element_in" or "element_not_in".
    std::string where;
    std::vector<std::int64_t> where_values;
    // Where the copies land among the passes (LP-0b): a canonical anchor's
    // name, or "" for none. `insert` names a pass instead, shown as it is.
    std::string anchor;
    std::string insert;
    // A body with a fullscreen or raster pass: only such a loop takes a place
    // (graph.insert_on_compute@1 otherwise).
    bool body_draws = false;
    std::vector<GraphCanvasLoopBranch> branches;
    std::vector<GraphCanvasLoopCarry> carry;
    // The loop node's own `outputs`: which `p` and `p[]` it declares.
    std::vector<std::string> outputs;
    // The boundary every branch shares, read off the first branch that has a
    // body (its graph.input nodes, its outputs).
    std::string body;
    std::vector<std::string> body_inputs;
    std::vector<std::string> body_outputs;
    // get_frame_plan's loops[] entry for this loop, when one arrived.
    bool reported = false;
    std::string reported_tier;
    std::int64_t reported_count = 0;

    // LP-4e: a loop over the draws of one material pass (over.draw_items):
    // one element per material, drawn inside that pass (draw_loop). Its
    // contract is the pass's, read only; its `where` tests tags; it takes no
    // count, order, limit or carry.
    bool draw_items = false;
    std::string contract;
    GraphCanvasTagFilter where_tags;
    // The render.material_pass whose `draws` this loop's output feeds, from
    // the document (or the report's `pass`).
    std::string pass;
    // Every material the report names, all pages read (the branch table's
    // `material` choices), and how many the report says there are.
    std::vector<std::string> materials;
    std::size_t reported_total = 0;
};

// LP-4c, decision 9: where a refused edit points. A loop refused by name
// marks the loop; a node of a loop's copy (`loop_origin`) marks the loop and
// the body node in its own scope.
struct GraphCanvasRefusalMark {
    std::string scope;
    std::string node;
    std::string reason;

    bool operator==(const GraphCanvasRefusalMark &) const = default;
};

// One animation sink `get_status.animation` reported for the owner the watched
// instance names. Read-only: the Timeline draws it beside the value traces so
// "what the pose side is doing" and "what the values partition produced" are
// on one axis without either pretending to be the other.
struct GraphCanvasAnimationSink {
    struct Cursor {
        std::string clip;
        double time_seconds = 0;
        double normalized_phase = 0;
    };

    std::string node;
    // "pose_graph_clip" | "pose_graph_state_machine" | "component_graph" |
    // "component_clip".
    std::string source;
    std::string state;
    bool transition_active = false;
    std::string transition_target;
    double transition_progress = 0;
    std::vector<Cursor> cursors;
};

// One reading the Timeline kept. The buffer is the client's: AG-3a publishes a
// snapshot of the current generation and no history at all, so a strip over
// time can only be what this session watched go by.
struct GraphCanvasTimelineSample {
    // get_status.time -- scene time, which is the Timeline's x axis.
    double time = 0;
    // The published generation the values came from, so a sample taken while
    // the engine was paused is still distinguishable from a fresh one.
    std::uint64_t frame = 0;
    // One per port of timelinePorts(), in that order.
    std::vector<double> values;
    bool has_phase = false;
    double phase = 0;
    bool has_progress = false;
    double progress = 0;
};

// Reads the common graph for a read-only node canvas. Qt-independent, in the
// same request/read two-slot shape as GraphAttachmentModel: the caller drains
// takeRpcRequests(), sends them over whatever transport it has, and feeds the
// answers back.
//
// It only ever asks for the three reads the canvas can justify:
// get_graph_instances, get_graph_catalog and get_graph_values.
class GraphCanvasModel {
    enum class RequestKind : std::uint8_t {
        Context,
        Catalog,
        Values,
        NodeValues,
        Status,
        Submit,
        Result,
        // LP-4c: get_frame_plan, for its loops[] only.
        Loops,
        // LP-4e: get_frame_plan {loop_elements}, one more page of a
        // draw_loop's materials.
        LoopPage,
    };
    struct PendingRequest {
        std::uint64_t id = 0;
        RequestKind kind = RequestKind::Context;
    };

    bool connected_ = false;
    std::uint64_t next_request_ = 1;
    std::vector<InspectorRpcRequest> requests_;
    // The document read and the reports run on separate slots: asking for
    // values never delays the next refresh, and a stale answer is dropped.
    std::optional<PendingRequest> pending_;
    // One outstanding read per kind. They are separate slots for the same
    // reason the document and the reports are: a Live tick asks two questions
    // at once, and neither may cancel the other's answer. Re-asking a kind
    // replaces its slot, which is how a stale reply is dropped.
    std::vector<PendingRequest> reads_;

    std::string digest_;
    // N-8: what the root rendering document actually is, and which file it is.
    // A legacy rendering root has nowhere to put a `layout`, and a panel that
    // keeps one has to be able to say that in those words -- naming the file --
    // rather than letting the user read a refused write as a bug.
    std::string root_schema_;
    std::string source_reference_;
    nlohmann::json nodes_ = nlohmann::json::array();
    nlohmann::json definitions_ = nlohmann::json::object();
    // N-6: the root scope's own placement, as the document carries it. A
    // definition's travels inside its body, so only this one needs a field.
    nlohmann::json layout_ = nlohmann::json::object();
    bool definitions_reported_ = false;
    bool context_received_ = false;

    std::vector<GraphCanvasUiNode> ui_nodes_;
    std::vector<std::string> audio_assets_;

    bool catalog_requested_ = false;
    bool catalog_received_ = false;
    // op -> catalog entry, flattened across modules.
    nlohmann::json operations_ = nlohmann::json::object();
    // semantic type id -> type schema, flattened the same way. The sockets are
    // drawn from these: the traits, and a module's own display declaration.
    nlohmann::json types_ = nlohmann::json::object();

    std::string values_instance_;
    bool values_received_ = false;
    bool values_published_ = false;
    nlohmann::json values_ = nlohmann::json::array();

    // AG-3b. The node values overlay: the same question in both scopes, with
    // the scope deciding which instance it is asked about.
    bool live_ = false;
    // The instance the reply in hand answers for. The root scope's question is
    // the empty one, so this is compared against nodeValuesInstance() rather
    // than against the watched instance.
    std::string node_values_instance_;
    bool node_values_received_ = false;
    bool node_values_published_ = false;
    nlohmann::json node_values_ = nlohmann::json::array();

    bool status_received_ = false;
    double status_time_ = 0;
    bool animation_available_ = false;
    nlohmann::json animation_sinks_ = nlohmann::json::array();

    // The Timeline's ring buffer. Nothing in the engine keeps a history, so a
    // strip over time is exactly what this session saw pass.
    std::string timeline_node_;
    std::vector<std::string> timeline_ports_;
    std::vector<GraphCanvasTimelineSample> timeline_;
    // One scrub at a time: the second set_time of a drag would race the first
    // render_frame, and the value read back would belong to neither. The
    // render is what ends it, so its id is the one that has to be watched for.
    bool scrubbing_ = false;
    std::uint64_t scrub_render_ = 0;

    InspectorNotice failure_;
    // What the last edit is doing or did. Kept apart from failure_ so a failed
    // report never reads as a failed edit, and the other way round.
    InspectorNotice edit_notice_;
    std::string ticket_;
    std::uint64_t content_revision_ = 0;
    std::uint64_t report_revision_ = 0;
    std::uint64_t placement_revision_ = 0;

    struct HistoryEntry {
        nlohmann::json forward;
        nlohmann::json inverse;
    };
    enum class HistoryAction { Edit, Undo, Redo };
    struct PendingHistory {
        HistoryAction action;
        HistoryEntry entry;
    };
    std::vector<HistoryEntry> undo_;
    std::vector<HistoryEntry> redo_;
    std::optional<PendingHistory> pending_history_;
    // Q-13. A placement refused because another panel's commit moved the
    // document is written again, once, against the document read next. The
    // placement is the canvas's own (the document only keeps it), so the same
    // op list is what it would build again; any other gesture is dropped as
    // before, because it was built from the nodes the old revision held.
    std::optional<nlohmann::json> layout_retry_;
    bool layout_retrying_ = false;
    // History belongs to exactly one committed document. A refresh showing
    // another writer's revision invalidates it instead of overwriting them.
    std::string history_digest_;
    static constexpr std::size_t historyCapacity = 100;
    // LP-4c, decision 7: each op is inverted against the document the ops
    // before it produced, which a copy of the document is walked through.
    nlohmann::json inverseOps(const nlohmann::json &ops) const;
    bool submitHistory(HistoryAction action, HistoryEntry entry);
    void commitHistory(const nlohmann::json &response);
    void clearHistory();

    // The current scope and the path taken to reach it. The first entry is
    // always the root scope.
    std::vector<std::string> breadcrumb_{std::string{}};
    // LP-4c, decision 5: the loop the current body was entered through, and
    // which branch of it each loop opens (the branch combo's choice).
    std::string entered_loop_;
    std::map<std::string, std::size_t, std::less<>> loop_branch_;

    // LP-4c: get_frame_plan's loops[], by loop id. Read whenever a document
    // with a root graph.loop arrives.
    nlohmann::json loop_reports_ = nlohmann::json::object();
    bool loop_reports_received_ = false;
    void requestLoopReports();
    // LP-4e: a draw_loop's report holds its first page of materials. The
    // rest are asked for a page at a time, one loop after another, until each
    // holds its `total` (or a page brings nothing new).
    std::string loop_page_node_;
    std::set<std::string, std::less<>> loop_pages_done_;
    void requestNextLoopPage();
    void receiveLoopPage(const nlohmann::json &plan);
    const nlohmann::json *loopReport(std::string_view id) const;
    // Decision 9: what the last refusal pointed at.
    std::vector<GraphCanvasRefusalMark> refusal_marks_;
    void markRefusal(const nlohmann::json &error, const std::string &reason);
    // The branch a loop runs for one element: the report's answer when there
    // is one, else the first branch whose integer condition holds.
    std::optional<std::size_t> branchForElement(std::string_view loop,
                                                std::int64_t element) const;
    // The body a descent into the loop lands in: the chosen branch's, else
    // the first branch that has one. Empty when no branch has a body.
    std::string loopEntryBody(const nlohmann::json &node) const;
    // One gesture on a loop's parameters: the whole object, one key changed.
    bool setLoopParameters(const std::string &id, nlohmann::json parameters);
    [[nodiscard]] nlohmann::json loopParameters(std::string_view id) const;

    void request(RequestKind kind, std::string method, nlohmann::json params);
    void read(RequestKind kind, std::string method, nlohmann::json params);
    // Take the pending read this id belongs to, if any. A reply whose slot has
    // already been replaced by a newer question of the same kind is dropped.
    std::optional<PendingRequest> takeRead(std::uint64_t id);
    void clearContext();
    void requestValues();
    // The pair of reads the observation overlay runs on: the node values for
    // the scope on screen, and the status the Timeline's axis and its
    // animation band come from.
    void requestObservation();
    void clearObservation();
    // Point the overlay at an instance without asking anything yet: descending
    // into one changes both the watch and the scope, and asking in between
    // would send a question about the scope being left.
    void setWatchedInstance(std::string instance);
    // The scope on screen changed: the overlay in hand is about the one that
    // was left, and the question has to be asked again.
    void enteredScope();
    void appendTimelineSample(std::uint64_t frame);
    // The `instance` the current scope's node values have to be asked for: the
    // empty question for the root document, the watched instance for a
    // definition body.
    [[nodiscard]] std::string nodeValuesInstance() const;
    // The outputs object get_graph_node_values reported for one node of the
    // scope on screen, or null.
    const nlohmann::json *nodeOutputs(std::string_view scope,
                                      std::string_view id) const;
    const nlohmann::json *scopeNodes(std::string_view scope) const;
    const nlohmann::json *operation(std::string_view op) const;
    // The type leaving one port of a definition, followed through whatever the
    // definition's outputs map names. Bounded: a definition whose output is
    // another instance's output can nest, and a malformed chain must not spin.
    std::string definitionOutputType(std::string_view definition,
                                     std::string_view port, int depth) const;
    const nlohmann::json *findNode(std::string_view scope,
                                   std::string_view id) const;
    // Q-11. The id of the scope's first value.input reading scene_time, or
    // empty. A placed audio.playback reads its time from it.
    [[nodiscard]] std::string sceneTimeNode(std::string_view scope) const;
    // One gesture is one op list, one ticket and one CAS: two edits in the same
    // frame cannot both land.
    bool submitOps(nlohmann::json ops);
    // The scope token the engine understands: the root document is "root", a
    // definition body is its own name.
    [[nodiscard]] std::string scopeToken() const;
    [[nodiscard]] static std::string scopeToken(std::string_view scope);

  public:
    void setConnected(bool connected);
    bool connected() const noexcept { return connected_; }

    void refresh();
    void requestCatalog();
    // Which root instance the values overlay follows. Idempotent: reselecting
    // the same instance must not throw away what is already shown.
    void watchInstance(std::string instance);
    const std::string &watchedInstance() const noexcept {
        return values_instance_;
    }
    // Nothing here holds a ticket, so polling only picks up the one read the
    // canvas still owes: the catalog it could not ask for while disconnected.
    void poll();
    // True while a poll would actually ask the engine something: the catalog
    // has still to be fetched, Live is on, or an edit ticket is outstanding.
    // False the rest of the time -- and then nothing has to wake up for it.
    [[nodiscard]] bool pollWanted() const noexcept {
        if (!connected_) return false;
        return !catalog_requested_ || live_ || (!pending_ && !ticket_.empty());
    }

    std::vector<InspectorRpcRequest> takeRpcRequests();
    void receiveRpcResult(std::uint64_t id, std::string_view json);
    void receiveRpcFailure(std::uint64_t id, std::string message);

    std::vector<GraphCanvasScope> scopes() const;
    const std::string &currentScope() const noexcept {
        return breadcrumb_.back();
    }
    const std::vector<std::string> &breadcrumb() const noexcept {
        return breadcrumb_;
    }
    // Selecting a scope from the combo restarts the trail: the root scope, and
    // the definition under it when one was chosen.
    bool selectScope(std::string_view scope);
    // Descend into the definition a root-scope graph.instance names.
    bool enterInstance(std::string_view node_id);
    // Whether that descent would land anywhere, without taking it.
    bool canEnterInstance(std::string_view node_id) const;
    bool leaveScope();

    std::vector<GraphCanvasNode> nodes() const { return nodes(currentScope()); }
    std::vector<GraphCanvasEdge> edges() const { return edges(currentScope()); }
    std::vector<GraphCanvasNode> nodes(std::string_view scope) const;
    std::vector<GraphCanvasEdge> edges(std::string_view scope) const;
    // (scope, node id) for every node the document holds, in every scope. The
    // scene prunes the positions it kept for nodes that are no longer there.
    std::vector<std::pair<std::string, std::string>> allNodeKeys() const;

    // Root instances that own a scene object, which is what get_graph_values
    // can answer for.
    std::vector<std::string> valueInstances() const;
    bool valuesPublished() const noexcept {
        return values_received_ && values_published_;
    }
    bool valuesReceived() const noexcept { return values_received_; }

    // --- AG-3b: observation.

    // Live follows the running engine: while it is on, every poll asks the
    // node values and the status again. Off, the overlay is read when the
    // document moves and when the scope or the watched instance changes --
    // which is what it did before this switch existed.
    void setLive(bool live);
    [[nodiscard]] bool live() const noexcept { return live_; }

    [[nodiscard]] bool nodeValuesReceived() const noexcept {
        return node_values_received_;
    }
    [[nodiscard]] bool nodeValuesPublished() const noexcept {
        return node_values_received_ && node_values_published_;
    }
    // What the status said, for the Timeline's axis.
    [[nodiscard]] bool statusReceived() const noexcept { return status_received_; }
    [[nodiscard]] double statusTime() const noexcept { return status_time_; }
    [[nodiscard]] bool animationAvailable() const noexcept {
        return animation_available_;
    }
    // The sinks whose owner is the owner the watched instance names. An
    // instance with no owner, or a status with no matching sink, answers with
    // nothing: a band over the wrong object's animation is worse than no band.
    [[nodiscard]] std::vector<GraphCanvasAnimationSink> animationSinks() const;

    // Which node the Timeline follows. Its output ports are the traces, up to
    // four of them; naming a different node starts the buffer again, because a
    // strip whose lines changed meaning halfway along says nothing.
    void setTimelineNode(std::string id);
    [[nodiscard]] const std::string &timelineNode() const noexcept {
        return timeline_node_;
    }
    [[nodiscard]] const std::vector<std::string> &timelinePorts() const noexcept {
        return timeline_ports_;
    }
    [[nodiscard]] const std::vector<GraphCanvasTimelineSample> &
    timelineSamples() const noexcept {
        return timeline_;
    }
    void clearTimeline();
    // How many readings the strip keeps. At the Live rate (one poll, 250 ms)
    // this is ten minutes of scene time.
    static constexpr std::size_t timelineCapacity = 2400;
    // At most four lines: a strip with more of them is a wall, and the node
    // that needed five wanted a graph of its own.
    static constexpr std::size_t timelineTraceLimit = 4;

    // Scrub: O-1's reading of "go to a time" -- set the clock, draw one frame,
    // read the values back. Two writes and the pair of reads, in that order,
    // and never two scrubs at once.
    bool scrub(double time);
    [[nodiscard]] bool scrubbing() const noexcept { return scrubbing_; }

    // --- Q-8: the UI node a `ui.value` / `ui.visible` sink writes.

    // The parameter an operation names a UI node in ("node" for the two ui.*
    // sinks), or empty for every other operation.
    [[nodiscard]] static std::string_view uiNodeParameter(std::string_view op) noexcept;
    // What the Studio read from the project's UI document. Held here so the
    // palette can seed a new sink with a node that exists.
    void setUiNodes(std::vector<GraphCanvasUiNode> nodes) { ui_nodes_ = std::move(nodes); }
    [[nodiscard]] const std::vector<GraphCanvasUiNode> &uiNodes() const noexcept {
        return ui_nodes_;
    }
    // The paths the `node` field of this operation offers: for `ui.value` the
    // gauges (the only widget whose value can be written), for `ui.visible`
    // every node. Empty for an operation with no UI node parameter.
    [[nodiscard]] std::vector<std::string> uiNodeChoices(std::string_view op) const;

    // --- Q-10: the three parameters of `audio.playback` that are not ports.

    [[nodiscard]] static bool isAudioPlayback(std::string_view op) noexcept;
    // The buses a voice plays on. The engine takes exactly these two
    // ("unknown audio bus: ... (a voice plays on se or bgm)") and names them
    // nowhere a client can read, so the list is held here, beside the
    // engine's default first.
    [[nodiscard]] static const std::vector<std::string> &audioBusChoices();
    // How the integrated time map wraps, the engine's default first.
    [[nodiscard]] static const std::vector<std::string> &audioWrapChoices();
    // The project's sounds, read by the widget. The palette seeds a new node
    // with the first; the `asset` field lists them all.
    void setAudioAssets(std::vector<std::string> assets) { audio_assets_ = std::move(assets); }
    [[nodiscard]] const std::vector<std::string> &audioAssets() const noexcept {
        return audio_assets_;
    }

    bool catalogReceived() const noexcept { return catalog_received_; }
    // The catalog's schema for one semantic type, or null when the catalog
    // never carried it. The socket resolver reads its traits and its optional
    // display declaration through here.
    const nlohmann::json *typeSchema(std::string_view id) const;
    // N-6. The placement the document holds for one scope, in the shape
    // `set_layout` writes: `{"nodes": {...}, "reroutes": [...], "ties": [...]}`,
    // or an empty object when the document says nothing about it. This is the
    // canvas's source of truth for where things are; the session table under it
    // only answers for a node the document has never placed.
    [[nodiscard]] nlohmann::json scopeLayout(std::string_view scope) const;
    [[nodiscard]] nlohmann::json scopeLayout() const {
        return scopeLayout(currentScope());
    }
    // One `set_layout`, naming the scope rather than taking the current one:
    // a debounced write can land after the canvas has already moved on, and it
    // must still be about the scope the gesture happened in.
    bool setLayout(const std::string &scope, const nlohmann::json &layout);

    bool definitionsReported() const noexcept { return definitions_reported_; }
    bool contextReceived() const noexcept { return context_received_; }
    const std::string &digest() const noexcept { return digest_; }
    // N-8. Empty until a document has arrived. "pelican.graph" is the root that
    // can carry a placement; anything else is a legacy rendering root.
    const std::string &rootSchema() const noexcept { return root_schema_; }
    const std::string &sourceReference() const noexcept {
        return source_reference_;
    }
    bool graphRoot() const noexcept { return root_schema_ == "pelican.graph"; }

    // One line for the panel. An error the user has to see outranks the two
    // "this view is incomplete, and here is why" notices, which outrank the
    // report of an unpublished graph.
    InspectorNotice notice() const;

    auto contentRevision() const noexcept { return content_revision_; }
    auto reportRevision() const noexcept { return report_revision_; }
    // Q-5. A read that differs from the one before only in where things are
    // placed -- the reply to a node drag, a moved control point, the Frame
    // Plan's member -- moves this one instead of contentRevision(): nothing
    // the canvas draws a node from has changed, so the values reported against
    // those nodes stay, and a canvas already drawing that placement has no
    // reason to throw its items away and draw them again.
    auto placementRevision() const noexcept { return placement_revision_; }
    bool busy() const noexcept { return pending_.has_value() || !ticket_.empty(); }
    bool canRefresh() const noexcept { return connected_ && !busy(); }
    // An edit needs a digest to compare against: without one there is nothing
    // to hold the document still between the read and the write.
    bool canEdit() const noexcept { return canRefresh() && !digest_.empty(); }
    bool canUndo() const noexcept {
        return canEdit() && !undo_.empty() && digest_ == history_digest_;
    }
    bool canRedo() const noexcept {
        return canEdit() && !redo_.empty() && digest_ == history_digest_;
    }
    bool undo();
    bool redo();

    // --- Editing. Each call is one gesture on the canvas and, when it returns
    // true, exactly one edit_graph_document carrying one op list. Every op
    // names the scope the canvas is showing, so a definition body is edited in
    // place rather than through its instances.

    // The authored parameters object of a node in the current scope. The pill
    // editor reads the whole of it and sends it back with one key replaced,
    // because set_node_parameters replaces rather than merges.
    nlohmann::json nodeParameters(std::string_view id) const;
    // Whether the canvas should let this drag land: a port that declares no
    // accepted type takes anything, and so does a producer whose type the
    // catalog never named. Two declared types have to be the same one.
    bool canConnect(std::string_view from_node, std::string_view from_port,
                    std::string_view to_node, std::string_view to_port) const;

    bool connectInput(const std::string &node, const std::string &port,
                      const std::string &source_node, const std::string &source_port);
    bool disconnectInput(const std::string &node, const std::string &port);
    // One key of one node's parameters. For an instance port this is the
    // stored value the port returns to; elsewhere it is the node's own setting.
    bool setNodeParameter(const std::string &node, const std::string &key,
                          const nlohmann::json &value);
    bool removeNode(const std::string &node);
    bool setNodeLabel(const std::string &node, const std::string &label);
    // Add one catalog operation to the current scope. Required inputs are
    // seeded with their own value.constant in the same op list, because a node
    // whose required input is unconnected is not a document the engine loads.
    bool addNode(const std::string &op, const std::string &definition = {});
    // The ops addNode would send, so the same list can be read back in a test
    // and replayed over the RPC.
    nlohmann::json addNodeOps(const std::string &op, const std::string &definition = {}) const;

    // LP-4d1: the same gestures, naming the scope they happen in rather than
    // taking the one on screen. The root canvas draws a loop's body inside
    // its zone, so a node there is edited in its definition while the canvas
    // shows the root; every op these send carries `scope: <definition>`.
    // The forms above are these with the current scope.
    [[nodiscard]] nlohmann::json nodeParameters(std::string_view scope,
                                                std::string_view id) const;
    [[nodiscard]] bool canConnect(std::string_view scope, std::string_view from_node,
                                  std::string_view from_port, std::string_view to_node,
                                  std::string_view to_port) const;
    bool connectInput(std::string_view scope, const std::string &node,
                      const std::string &port, const std::string &source_node,
                      const std::string &source_port);
    bool disconnectInput(std::string_view scope, const std::string &node,
                         const std::string &port);
    bool setNodeParameter(std::string_view scope, const std::string &node,
                          const std::string &key, const nlohmann::json &value);
    bool removeNode(std::string_view scope, const std::string &node);
    bool setNodeLabel(std::string_view scope, const std::string &node,
                      const std::string &label);
    bool addNodeIn(std::string_view scope, const std::string &op,
                   const std::string &definition = {});
    [[nodiscard]] nlohmann::json addNodeOpsIn(std::string_view scope, const std::string &op,
                                              const std::string &definition = {}) const;
    [[nodiscard]] std::vector<GraphCanvasCatalogEntry> paletteOperationsIn(
        std::string_view scope, std::string_view text = {}, std::string_view domain = {}) const;
    [[nodiscard]] std::vector<std::string> paletteDomainsIn(std::string_view scope) const;
    [[nodiscard]] std::string suggestNodeId(std::string_view scope, std::string_view op) const;

    // What the palette offers here, filtered by a case-insensitive substring of
    // the operation name and, when non-empty, by domain.
    std::vector<GraphCanvasCatalogEntry> paletteOperations(
        std::string_view text = {}, std::string_view domain = {}) const;
    std::vector<std::string> paletteDomains() const;
    // An id no node in this scope is using, derived from the operation name.
    std::string suggestNodeId(std::string_view op) const;
    // The stored values an instance of this definition has to start with: one
    // per graph.input, its declared default or zero.
    nlohmann::json definitionDefaults(std::string_view definition) const;

    const InspectorNotice &editNotice() const noexcept { return edit_notice_; }

    // --- LP-4c: graph.loop.

    // A root graph.loop read into the Loop fields' terms, or nothing.
    [[nodiscard]] std::optional<GraphCanvasLoop> loop(std::string_view id) const;
    [[nodiscard]] bool loopReportsReceived() const noexcept {
        return loop_reports_received_;
    }
    // A definition name no definition is using, from `base`.
    [[nodiscard]] std::string suggestDefinitionName(std::string_view base = "loop_body") const;
    // Decision 2. The ops "Add loop" sends: over one element, one branch
    // running `body`, and every body output declared as `p`. With
    // `new_definition` set, the body is a new, empty definition of that name,
    // added in the same list.
    [[nodiscard]] nlohmann::json addLoopOps(const std::string &body,
                                            const std::string &new_definition = {}) const;
    bool addLoop(const std::string &body, const std::string &new_definition = {});

    // Decision 3: the Loop fields. Each is one gesture: one set_node_parameters
    // carrying the whole parameters object with one member changed (or, for
    // the outputs, one replace_node).
    bool setLoopCount(const std::string &id, std::int64_t count);
    bool setLoopCountExpression(const std::string &id, const std::string &expression);
    bool setLoopDescending(const std::string &id, bool descending);
    bool setLoopLimit(const std::string &id, std::optional<std::int64_t> limit);
    // "" (automatic), "expand" or "draw_loop".
    bool setLoopTier(const std::string &id, const std::string &tier);
    // "" (none), "element_in" or "element_not_in".
    bool setLoopWhere(const std::string &id, const std::string &kind,
                      const std::vector<std::int64_t> &values);
    bool setLoopCarry(const std::string &id,
                      const std::vector<std::pair<std::string, std::string>> &carry);
    bool setLoopOutputs(const std::string &id, const std::vector<std::string> &outputs);
    // Where the copies land: one of loopAnchors() as canonical_anchor, or ""
    // for none (the end of the pass family). Either way an `insert` goes.
    bool setLoopAnchor(const std::string &id, const std::string &anchor);
    // The canonical anchors a loop can be placed at, in the frame's order.
    // A loop with none expands after the scene passes, before the first
    // anchor (LP-1e): Studio does not give one on its own.
    [[nodiscard]] static const std::vector<std::string> &loopAnchors();

    // Decision 4: the branch table. The last branch is the else, which is
    // never removed, moved or given a condition.
    bool addLoopBranch(const std::string &id);
    bool removeLoopBranch(const std::string &id, std::size_t branch);
    bool moveLoopBranch(const std::string &id, std::size_t branch, int delta);
    // "element", "element_in" or "element_not_in" with its integers.
    bool setLoopBranchWhen(const std::string &id, std::size_t branch,
                           const std::string &when, const std::vector<std::int64_t> &values);
    // An empty optional is `(nothing)`, the `null` body.
    bool setLoopBranchBody(const std::string &id, std::size_t branch,
                           const std::optional<std::string> &body);
    // "New definition...": a definition with the loop's boundary (its first
    // body's graph.input nodes and outputs) under `name`, and the branch
    // running it, in one list.
    bool newLoopBranchDefinition(const std::string &id, std::size_t branch,
                                 const std::string &name);
    // The definitions a branch of this loop may run: those whose boundary is
    // the loop's (the same graph.input ids and the same outputs).
    [[nodiscard]] std::vector<std::string> loopBodyChoices(std::string_view id) const;

    // One gesture's op list as built elsewhere -- the zone gestures of LP-4d
    // put a loop input and its body's graph.input in one list -- sent with
    // its inverse like every other gesture.
    bool submitEdit(nlohmann::json ops);

    // LP-0c's three definition ops, for the undo they need (decision 7).
    bool renameDefinition(const std::string &from, const std::string &to);
    bool removeDefinition(const std::string &name);

    // Decision 5. The loop the current scope was entered through (empty when
    // it was not), its branch the descent opened, and a switch to another
    // branch's body while inside.
    [[nodiscard]] const std::string &enteredLoop() const noexcept { return entered_loop_; }
    [[nodiscard]] std::size_t loopBranch(std::string_view id) const;
    bool selectLoopBranch(std::size_t branch);
    // How many root nodes (instances and loop branches) name a definition:
    // more than one is the "shared" mark.
    [[nodiscard]] std::size_t definitionUseCount(std::string_view definition) const;
    // LP-4d1: the body a root loop's zone shows -- the branch its tab chose,
    // else the first branch that has one (the body a descent opens). Empty
    // when no branch has a body. `showLoopBranch` is the tab: session state,
    // never written to the document. False for a branch with no body.
    [[nodiscard]] std::string loopZoneBody(std::string_view loop) const;
    bool showLoopBranch(std::string_view loop, std::size_t branch);
    [[nodiscard]] std::vector<GraphCanvasBodyOutput> bodyOutputs(std::string_view definition) const;

    // Decision 6. The other loops' `p[]` outputs a root loop's input can read
    // one element of, as (loop, port).
    [[nodiscard]] std::vector<std::pair<std::string, std::string>>
    perIterationSources(std::string_view receiver) const;
    bool connectPerIteration(const std::string &node, const std::string &port,
                             const std::string &source_node, const std::string &source_port,
                             const std::string &expression = "element");

    // --- LP-4d2: lines across a loop zone's edge.

    // The name a new loop input or body output is given, proposed from the
    // port whose line made it (dots and slashes become `_`, a `[]` goes):
    // an input's is no input of the loop and no node of any body its
    // branches run; an output's is no output of any of those bodies and
    // nothing the loop declares.
    [[nodiscard]] std::string suggestLoopInputName(std::string_view loop,
                                                   std::string_view base) const;
    [[nodiscard]] std::string suggestLoopOutputName(std::string_view loop,
                                                    std::string_view base) const;
    // Decision 1. A root node's output drawn into a root loop's zone: a new
    // loop input it feeds (root `set_input`), and a graph.input of that id in
    // every definition the branches run (each once), in one list. With a
    // target -- a node's input in `body`, one of those definitions -- that
    // input reads the new graph.input in the same list. Empty when it cannot
    // be sent.
    [[nodiscard]] nlohmann::json zoneInputOps(std::string_view loop,
                                              const std::string &source_node,
                                              const std::string &source_port,
                                              const std::string &body = {},
                                              const std::string &target_node = {},
                                              const std::string &target_port = {}) const;
    bool addZoneInput(const std::string &loop, const std::string &source_node,
                      const std::string &source_port, const std::string &body = {},
                      const std::string &target_node = {}, const std::string &target_port = {});
    // Decision 2. A body node's output drawn to the zone's right edge: the
    // output in every definition the branches run (DO-1's set_outputs, each
    // body's whole outputs), and `p` declared on the loop (replace_node), in
    // one list. Every other body has to give the same output from a node of
    // the same id and port -- a set_outputs endpoint names a node, so an
    // output cannot be left unconnected there -- and when one cannot, the
    // list is empty and `refusal` says which body, in one line.
    [[nodiscard]] nlohmann::json zoneOutputOps(std::string_view loop, const std::string &body,
                                               const std::string &node, const std::string &port,
                                               std::string *refusal = nullptr) const;
    // Sends it, or, refused, puts the line in the edit notice and sends
    // nothing.
    bool addZoneOutput(const std::string &loop, const std::string &body,
                       const std::string &node, const std::string &port);
    // Decision 3. One pair more (or less) in the loop's `carry`: the payload
    // the Loop fields' carry rows send.
    bool addLoopCarry(const std::string &loop, const std::string &from, const std::string &to);
    bool removeLoopCarry(const std::string &loop, const std::string &from, const std::string &to);
    // Decision 6. Show the body node a loop's copy came from on the root
    // canvas: the root scope, with the loop's tab on the branch that element
    // ran. Answers the definition the node stands in, or empty -- with
    // nothing changed -- when the loop, the branch or the node is not there.
    std::string showLoopOrigin(std::string_view loop, std::int64_t element, std::string_view node);

    // Decision 9. What the last refused edit pointed at; cleared by the next
    // edit that commits.
    [[nodiscard]] const std::vector<GraphCanvasRefusalMark> &refusalMarks() const noexcept {
        return refusal_marks_;
    }
    // Decision 10. Open the body node a loop copy came from: the scope of
    // the branch that element ran, entered through the loop. False when the
    // loop, the branch or the node is not in the document.
    bool enterLoopOrigin(std::string_view loop, std::int64_t element, std::string_view node);

    // --- LP-4e: the draw_loop stage, render.material_pass and draw.*.

    // The material contracts a render.material_pass is given (the palette's
    // combo), and a material's shader contracts (a draw_loop branch's
    // `contract`), in the order the combos list them.
    [[nodiscard]] static const std::vector<std::string> &materialPassContracts();
    [[nodiscard]] static const std::vector<std::string> &materialShaderContracts();
    // render.material_pass's attachment sockets, in location order: one per
    // material_outputs output when it declares them, else `color` for a
    // forward contract and the five G-buffer ones for a deferred one. Empty
    // for a contract the table does not know. (graphmodule.cpp's
    // renderMaterialPassColorPorts, restated: Studio does not link the graph
    // module's registry.)
    [[nodiscard]] static std::vector<std::string>
    materialPassColorSockets(const nlohmann::json &parameters);
    // A draw_loop body's nodes are draw.* only: the palette offers them in a
    // definition a draw_loop runs, or one that already holds a draw step.
    [[nodiscard]] bool drawBodyScope(std::string_view scope) const;

    // Decision 3: the draw_loop's own fields. `where` tests tags; an empty
    // filter removes it.
    bool setLoopWhereTags(const std::string &id, const GraphCanvasTagFilter &tags);
    // A branch's condition as a draw_loop reads it (AND of what is set). An
    // empty condition is refused: only the last row is the else.
    bool setLoopBranchDrawWhen(const std::string &id, std::size_t branch,
                               const GraphCanvasDrawWhen &when);
    // Every material a draw_loop's report named, all pages read. Empty
    // without a report.
    [[nodiscard]] std::vector<std::string> loopMaterials(std::string_view id) const;

    // Decision 4: a draw step's parameters, each one gesture.
    bool setDrawVariant(const std::string &node, const std::string &variant);
    // render_state: cull / depth_test / depth_write / blend / depth_compare,
    // an absent key keeping the material's own; defines one per entry. Both
    // empty is not an override, and is refused.
    bool setDrawOverride(const std::string &node, const nlohmann::json &render_state,
                         const std::vector<std::string> &defines);
    // LP-4d1: a draw step inside a draw_loop's zone, named by its body.
    bool setDrawVariant(std::string_view scope, const std::string &node,
                        const std::string &variant);
    bool setDrawOverride(std::string_view scope, const std::string &node,
                         const nlohmann::json &render_state,
                         const std::vector<std::string> &defines);
    [[nodiscard]] std::string suggestDrawVariant(std::string_view scope) const;
    bool setMaterialPassContract(const std::string &node, const std::string &contract);
    // The variant a new draw.variant is given: the material_variant of the
    // pass the scope's loop draws in, when it names one, else "variant".
    [[nodiscard]] std::string suggestDrawVariant() const;
};

} // namespace PelicanStudio
