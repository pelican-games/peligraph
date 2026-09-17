#pragma once

#include "inspectormodel.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
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
    std::string definition;
    bool instance = false;
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
};

// A scope is either the root document or one definition body. The root scope
// carries the empty id so a definition named "root" stays addressable.
struct GraphCanvasScope {
    std::string id;
    std::string label;
    bool root = false;
};

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

// Reads the common graph for a read-only node canvas. Qt-independent, in the
// same request/read two-slot shape as GraphAttachmentModel: the caller drains
// takeRpcRequests(), sends them over whatever transport it has, and feeds the
// answers back.
//
// It only ever asks for the three reads the canvas can justify:
// get_graph_instances, get_graph_catalog and get_graph_values.
class GraphCanvasModel {
    enum class RequestKind : std::uint8_t { Context, Catalog, Values, Submit, Result };
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
    std::optional<PendingRequest> read_;

    std::string digest_;
    nlohmann::json nodes_ = nlohmann::json::array();
    nlohmann::json definitions_ = nlohmann::json::object();
    bool definitions_reported_ = false;
    bool context_received_ = false;

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

    InspectorNotice failure_;
    // What the last edit is doing or did. Kept apart from failure_ so a failed
    // report never reads as a failed edit, and the other way round.
    InspectorNotice edit_notice_;
    std::string ticket_;
    std::uint64_t content_revision_ = 0;
    std::uint64_t report_revision_ = 0;

    // The current scope and the path taken to reach it. The first entry is
    // always the root scope.
    std::vector<std::string> breadcrumb_{std::string{}};

    void request(RequestKind kind, std::string method, nlohmann::json params);
    void read(RequestKind kind, std::string method, nlohmann::json params);
    void clearContext();
    void requestValues();
    const nlohmann::json *scopeNodes(std::string_view scope) const;
    const nlohmann::json *operation(std::string_view op) const;
    // The type leaving one port of a definition, followed through whatever the
    // definition's outputs map names. Bounded: a definition whose output is
    // another instance's output can nest, and a malformed chain must not spin.
    std::string definitionOutputType(std::string_view definition,
                                     std::string_view port, int depth) const;
    const nlohmann::json *findNode(std::string_view scope,
                                   std::string_view id) const;
    // One gesture is one op list, one ticket and one CAS: two edits in the same
    // frame cannot both land.
    bool submitOps(nlohmann::json ops);
    // The scope token the engine understands: the root document is "root", a
    // definition body is its own name.
    [[nodiscard]] std::string scopeToken() const;

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

    bool catalogReceived() const noexcept { return catalog_received_; }
    // The catalog's schema for one semantic type, or null when the catalog
    // never carried it. The socket resolver reads its traits and its optional
    // display declaration through here.
    const nlohmann::json *typeSchema(std::string_view id) const;
    bool definitionsReported() const noexcept { return definitions_reported_; }
    bool contextReceived() const noexcept { return context_received_; }
    const std::string &digest() const noexcept { return digest_; }

    // One line for the panel. An error the user has to see outranks the two
    // "this view is incomplete, and here is why" notices, which outrank the
    // report of an unpublished graph.
    InspectorNotice notice() const;

    auto contentRevision() const noexcept { return content_revision_; }
    auto reportRevision() const noexcept { return report_revision_; }
    bool busy() const noexcept { return pending_.has_value() || !ticket_.empty(); }
    bool canRefresh() const noexcept { return connected_ && !busy(); }
    // An edit needs a digest to compare against: without one there is nothing
    // to hold the document still between the read and the write.
    bool canEdit() const noexcept { return canRefresh() && !digest_.empty(); }

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
};

} // namespace PelicanStudio
