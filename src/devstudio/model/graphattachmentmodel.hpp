#pragma once

#include "inspectormodel.hpp"
#include "rendereditticket.hpp"

#include <optional>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {

struct GraphAttachmentOwner {
    std::string scene;
    std::uint64_t object = 0;
    bool operator==(const GraphAttachmentOwner &) const = default;
};

// One input port of an attached instance, as the Inspector shows it. The mode
// is read from the authoring document -- an edge leaves no node behind in the
// expansion, so it cannot be read back from there -- while `value` is what the
// last drawn generation actually fed the port, when the engine has published
// one.
struct GraphInstanceInputRow {
    std::string port;
    // "connected" | "parameter" | "default"
    std::string mode;
    // The stored scalar the port returns to when an edge is removed, or the
    // definition's default. Absent only when the definition declares neither.
    bool has_stored = false;
    double stored = 0;
    bool has_parameter = false; // The document stores a value of its own.
    // Producer of the edge, when the mode is "connected".
    std::string source_node;
    std::string source_port;
    // The published snapshot, when one covers this port.
    bool has_value = false;
    double value = 0;
    // "connected" | "parameter" | "default" | "override"
    std::string value_source;
};

// One candidate producer for "connect to another output".
struct GraphOutputCandidate {
    std::string node;
    std::string port;
};

// Keeps authored graph edits and their tickets independent of Inspector refreshes.
// Object IDs come from the Inspector snapshot, never the outliner row index.
class GraphAttachmentModel {
    enum class RequestKind { Context, Catalog, Values };
    struct PendingRequest {
        std::uint64_t id;
        std::uint64_t selection_revision;
        RequestKind kind;
    };
    std::optional<GraphAttachmentOwner> owner_;
    bool connected_ = false;
    std::uint64_t selection_revision_ = 0;
    std::uint64_t next_request_ = 1;
    std::optional<PendingRequest> pending_;
    std::vector<InspectorRpcRequest> requests_;
    RenderEditTicket edit_;
    std::function<void()> scheduled_poll_;
    std::uint64_t edit_selection_revision_ = 0;
    std::string digest_;
    nlohmann::json definitions_ = nlohmann::json::object();
    nlohmann::json instances_ = nlohmann::json::array();
    nlohmann::json all_instances_ = nlohmann::json::array();
    nlohmann::json nodes_ = nlohmann::json::array();
    InspectorNotice notice_;
    std::uint64_t content_revision_ = 0;
    // Bumped by reports (published values, catalog). They change what the
    // panel shows, never what the document holds.
    std::uint64_t report_revision_ = 0;
    // Reads that report rather than edit. They run on their own slot so
    // selecting a row never blocks an edit, and never joins the edit's CAS.
    std::optional<PendingRequest> read_;
    bool catalog_requested_ = false;
    nlohmann::json catalog_ = nlohmann::json::object();
    std::string values_instance_;
    nlohmann::json values_ = nlohmann::json::array();
    // RSX: the line of a commit that asked for an engine restart, until the
    // owner takes it.
    std::optional<std::string> engine_restart_;

    void edited(const RenderEditTicket::Outcome &outcome);
    void request(RequestKind kind, std::string method, nlohmann::json params);
    void read(RequestKind kind, std::string method, nlohmann::json params);
    void requestValues();
    bool submit(nlohmann::json attach, nlohmann::json detach);
    bool submitOps(nlohmann::json ops);
    void fail(std::string message, std::uint64_t selection_revision);
    void clearContext();
    const nlohmann::json *findInstance(std::string_view id) const;

  public:
    GraphAttachmentModel();
    // The Qt owner supplies its timer and digest hub; model tests use the
    // explicit poll clock and the same queue of RPC requests.
    std::function<void(int, std::function<void()>)> scheduleEdit;
    std::function<void(const std::string &)> publishDigest;
    void setConnected(bool connected);
    void selectOwner(std::optional<GraphAttachmentOwner> owner);
    void refresh();
    void poll();
    // True while a poll would actually ask the engine something: an edit
    // ticket is outstanding and its slot is free. False the rest of the time,
    // which is most of the time -- and then nobody has to wake up for it.
    [[nodiscard]] bool pollWanted() const noexcept {
        return connected_ && edit_.busy() && !edit_.awaitingResponse();
    }
    std::vector<InspectorRpcRequest> takeRpcRequests();
    void receiveRpcResult(std::uint64_t id, std::string_view json);
    void receiveRpcFailure(std::uint64_t id, std::string message);
    bool attach(std::string id, std::string definition, nlohmann::json parameters);
    bool detach(std::string id);
    nlohmann::json parameterDefaults(std::string_view definition) const;
    std::string suggestInstanceId(std::string_view definition) const;

    // The input ports of one attached instance, in declaration order of the
    // definition's graph.input nodes.
    std::vector<GraphInstanceInputRow> instanceInputs(std::string_view instance) const;
    // Ask the engine what the drawn generation fed this instance, and what
    // operations exist. Both are reads: they never join an edit's CAS and never
    // make the panel busy.
    void watchInstance(std::string instance);
    void requestCatalog();
    const auto &catalog() const noexcept { return catalog_; }
    // Edits. Each is one edit_graph_document call, so one Inspector action is
    // one ticket and one CAS -- two calls in the same frame cannot both land.
    bool setInstanceParameter(const std::string &instance, const std::string &port, double value);
    bool controlWithCurve(const std::string &instance, const std::string &port);
    bool connectInstanceInput(const std::string &instance, const std::string &port,
                              const GraphOutputCandidate &source);
    bool disconnectInstanceInput(const std::string &instance, const std::string &port);
    // Producers an input could be connected to. Scalar-only once the catalog
    // has arrived; before that, everything the document declares.
    std::vector<GraphOutputCandidate> outputCandidates(std::string_view instance) const;
    // The ops "control with a curve" sends, so the same list can be replayed
    // over the RPC and compared with what the panel produced.
    nlohmann::json curveControlOps(const std::string &instance, const std::string &port) const;

    const auto &owner() const noexcept { return owner_; }
    const auto &definitions() const noexcept { return definitions_; }
    const auto &instances() const noexcept { return instances_; }
    const auto &notice() const noexcept { return notice_; }
    auto contentRevision() const noexcept { return content_revision_; }
    auto reportRevision() const noexcept { return report_revision_; }
    bool busy() const noexcept { return pending_.has_value() || edit_.busy(); }
    // RSX, decision 1. An edit that committed with restart_required: the
    // line to restart the engine with ("Restarting the engine to apply this
    // change (<code>)..."), once.
    [[nodiscard]] std::optional<std::string> takeEngineRestart() {
        return std::exchange(engine_restart_, std::nullopt);
    }
    bool canRefresh() const noexcept { return connected_ && owner_ && !busy(); }
    bool canEdit() const noexcept { return canRefresh() && !digest_.empty(); }
};

} // namespace PelicanStudio
