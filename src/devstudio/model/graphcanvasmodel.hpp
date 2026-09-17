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

struct GraphCanvasNode {
    std::string id;
    std::string op;
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
    std::vector<std::string> outputs;
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

// Reads the common graph for a read-only node canvas. Qt-independent, in the
// same request/read two-slot shape as GraphAttachmentModel: the caller drains
// takeRpcRequests(), sends them over whatever transport it has, and feeds the
// answers back.
//
// It only ever asks for the three reads the canvas can justify:
// get_graph_instances, get_graph_catalog and get_graph_values.
class GraphCanvasModel {
    enum class RequestKind : std::uint8_t { Context, Catalog, Values };
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

    std::string values_instance_;
    bool values_received_ = false;
    bool values_published_ = false;
    nlohmann::json values_ = nlohmann::json::array();

    InspectorNotice failure_;
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
    const nlohmann::json *findNode(std::string_view scope,
                                   std::string_view id) const;

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
    bool definitionsReported() const noexcept { return definitions_reported_; }
    bool contextReceived() const noexcept { return context_received_; }
    const std::string &digest() const noexcept { return digest_; }

    // One line for the panel. An error the user has to see outranks the two
    // "this view is incomplete, and here is why" notices, which outrank the
    // report of an unpublished graph.
    InspectorNotice notice() const;

    auto contentRevision() const noexcept { return content_revision_; }
    auto reportRevision() const noexcept { return report_revision_; }
    bool busy() const noexcept { return pending_.has_value(); }
    bool canRefresh() const noexcept { return connected_ && !busy(); }
};

} // namespace PelicanStudio
