#pragma once

#include "inspectormodel.hpp"

#include <optional>
#include <string>
#include <vector>

namespace PelicanStudio {

struct GraphAttachmentOwner {
    std::string scene;
    std::uint64_t object = 0;
    bool operator==(const GraphAttachmentOwner &) const = default;
};

// Keeps authored graph edits and their tickets independent of Inspector refreshes.
// Object IDs come from the Inspector snapshot, never the outliner row index.
class GraphAttachmentModel {
    enum class RequestKind { Context, Submit, Result };
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
    std::string ticket_;
    std::uint64_t edit_selection_revision_ = 0;
    std::string digest_;
    nlohmann::json definitions_ = nlohmann::json::object();
    nlohmann::json instances_ = nlohmann::json::array();
    nlohmann::json all_instances_ = nlohmann::json::array();
    InspectorNotice notice_;
    std::uint64_t content_revision_ = 0;

    void request(RequestKind kind, std::string method, nlohmann::json params);
    bool submit(nlohmann::json attach, nlohmann::json detach);
    void fail(std::string message, std::uint64_t selection_revision);
    void clearContext();

  public:
    void setConnected(bool connected);
    void selectOwner(std::optional<GraphAttachmentOwner> owner);
    void refresh();
    void poll();
    std::vector<InspectorRpcRequest> takeRpcRequests();
    void receiveRpcResult(std::uint64_t id, std::string_view json);
    void receiveRpcFailure(std::uint64_t id, std::string message);
    bool attach(std::string id, std::string definition, nlohmann::json parameters);
    bool detach(std::string id);
    nlohmann::json parameterDefaults(std::string_view definition) const;
    std::string suggestInstanceId(std::string_view definition) const;

    const auto &owner() const noexcept { return owner_; }
    const auto &definitions() const noexcept { return definitions_; }
    const auto &instances() const noexcept { return instances_; }
    const auto &notice() const noexcept { return notice_; }
    auto contentRevision() const noexcept { return content_revision_; }
    bool busy() const noexcept { return pending_.has_value() || !ticket_.empty(); }
    bool canRefresh() const noexcept { return connected_ && owner_ && !busy(); }
    bool canEdit() const noexcept { return canRefresh() && !digest_.empty(); }
};

} // namespace PelicanStudio
