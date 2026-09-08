#include "graphattachmentmodel.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace PelicanStudio {
namespace {
using Json = nlohmann::json;

std::string diagnostic(const Json &response) {
    const auto &error = response.at("error");
    if (error.is_string()) return error.get<std::string>();
    auto message = error.value("message", std::string{"Graph edit rejected."});
    if (error.contains("code")) {
        const auto &code = error.at("code");
        message += " (" + (code.is_string() ? code.get<std::string>() : code.dump()) + ")";
    }
    return message;
}
} // namespace

void GraphAttachmentModel::clearContext() {
    digest_.clear();
    definitions_ = Json::object();
    instances_ = all_instances_ = Json::array();
    ++content_revision_;
}

void GraphAttachmentModel::setConnected(bool connected) {
    if (connected_ == connected) return;
    connected_ = connected;
    pending_.reset();
    requests_.clear();
    ticket_.clear();
    clearContext();
    notice_ = {};
    refresh();
}

void GraphAttachmentModel::selectOwner(std::optional<GraphAttachmentOwner> owner) {
    if (owner == owner_) return;
    owner_ = std::move(owner);
    ++selection_revision_;
    clearContext();
    notice_ = {};
    // Reads can be discarded; an accepted edit must still be followed to completion.
    if (pending_ && pending_->kind == RequestKind::Context) {
        pending_.reset();
        requests_.clear();
    }
    refresh();
}

void GraphAttachmentModel::request(RequestKind kind, std::string method, Json params) {
    const auto id = next_request_++;
    pending_ = PendingRequest{id, selection_revision_, kind};
    requests_.push_back({id, std::move(method), std::move(params)});
}

void GraphAttachmentModel::refresh() {
    if (canRefresh()) request(RequestKind::Context, "get_graph_instances", Json::object());
}

void GraphAttachmentModel::poll() {
    if (connected_ && !pending_ && !ticket_.empty())
        request(RequestKind::Result, "get_edit_result", {{"ticket", ticket_}});
}

std::vector<InspectorRpcRequest> GraphAttachmentModel::takeRpcRequests() {
    return std::exchange(requests_, {});
}

void GraphAttachmentModel::fail(std::string message, std::uint64_t revision) {
    ticket_.clear();
    if (revision == selection_revision_)
        notice_ = {InspectorNoticeKind::Error, std::move(message)};
    else
        refresh();
}

void GraphAttachmentModel::receiveRpcFailure(std::uint64_t id, std::string message) {
    if (!pending_ || pending_->id != id) return;
    const auto revision = pending_->kind == RequestKind::Context
        ? pending_->selection_revision : edit_selection_revision_;
    const auto kind = pending_->kind;
    pending_.reset();
    if (kind == RequestKind::Result) {
        // A failed poll does not cancel the engine ticket or permit another edit.
        if (revision == selection_revision_)
            notice_ = {InspectorNoticeKind::Error, std::move(message)};
        return;
    }
    fail(std::move(message), revision);
}

void GraphAttachmentModel::receiveRpcResult(std::uint64_t id, std::string_view text) {
    if (!pending_ || pending_->id != id) return;
    const auto pending = *pending_;
    pending_.reset();
    const auto revision = pending.kind == RequestKind::Context
        ? pending.selection_revision : edit_selection_revision_;
    try {
        const auto response = Json::parse(text);
        if (response.contains("error")) {
            if (pending.kind == RequestKind::Result && response.value("status", std::string{}) != "rejected")
                throw std::runtime_error(diagnostic(response));
            fail(diagnostic(response), revision);
            return;
        }
        if (pending.kind == RequestKind::Context) {
            auto definitions = response.at("definitions");
            auto instances = response.at("instances");
            auto digest = response.at("source_digest").at("hex").get<std::string>();
            if (!definitions.is_object() || !instances.is_array() || digest.empty())
                throw std::invalid_argument("Invalid graph attachment context.");
            auto selected = Json::array();
            for (const auto &instance : instances) {
                const auto found = instance.find("owner");
                if (found != instance.end() && owner_ &&
                    found->at("scene") == owner_->scene && found->at("object") == owner_->object)
                    selected.push_back(instance);
            }
            definitions_ = std::move(definitions);
            all_instances_ = std::move(instances);
            instances_ = std::move(selected);
            digest_ = std::move(digest);
            ++content_revision_;
        } else if (pending.kind == RequestKind::Submit) {
            if (response.at("status") != "accepted")
                throw std::invalid_argument("The engine did not accept the graph edit.");
            ticket_ = response.at("ticket").get<std::string>();
            if (ticket_.empty()) throw std::invalid_argument("The engine returned no graph edit ticket.");
            poll();
        } else {
            const auto status = response.at("status").get<std::string>();
            if (status == "pending") return;
            if (status == "rejected") {
                fail("The engine rejected the graph edit.", revision);
                return;
            }
            if (status != "committed")
                throw std::invalid_argument("Graph edit did not commit: " + status);
            ticket_.clear();
            if (revision == selection_revision_)
                notice_ = {InspectorNoticeKind::Success, "Graph changes saved."};
            refresh();
        }
    } catch (const std::exception &error) {
        if (pending.kind == RequestKind::Result && !ticket_.empty()) {
            if (revision == selection_revision_)
                notice_ = {InspectorNoticeKind::Error, error.what()};
        } else {
            fail(error.what(), revision);
        }
    }
}

Json GraphAttachmentModel::parameterDefaults(std::string_view definition) const {
    Json parameters = Json::object();
    for (const auto &node : definitions_.at(std::string{definition}).at("nodes")) {
        if (node.at("op") != "graph.input") continue;
        const auto value = node.value("default", Json(0.0));
        if (!value.is_number() || !std::isfinite(value.get<double>()))
            throw std::invalid_argument("Graph inputs must have finite scalar initial values.");
        parameters[node.at("id").get<std::string>()] = value;
    }
    return parameters;
}

std::string GraphAttachmentModel::suggestInstanceId(std::string_view definition) const {
    const auto prefix = std::string{definition} + "_" + std::to_string(owner_ ? owner_->object : 0);
    auto result = prefix;
    std::uint64_t suffix = 2;
    while (std::any_of(all_instances_.begin(), all_instances_.end(),
                      [&](const Json &node) { return node.at("id") == result; }))
        result = prefix + "_" + std::to_string(suffix++);
    return result;
}

bool GraphAttachmentModel::submit(Json attach, Json detach) {
    if (!canEdit()) return false;
    edit_selection_revision_ = selection_revision_;
    notice_ = {InspectorNoticeKind::Information, "Saving graph changes..."};
    request(RequestKind::Submit, "edit_graph_instances",
            {{"base_source_digest", digest_}, {"attach", std::move(attach)}, {"detach", std::move(detach)}});
    return true;
}

bool GraphAttachmentModel::attach(std::string id, std::string definition, Json parameters) {
    if (!canEdit()) return false;
    try {
        if (id.empty() || id.find('/') != std::string::npos)
            throw std::invalid_argument("Instance name must be nonempty and contain no slash.");
        if (std::any_of(all_instances_.begin(), all_instances_.end(),
                        [&](const Json &node) { return node.at("id") == id; }))
            throw std::invalid_argument("This graph instance name is already in use.");
        const auto defaults = parameterDefaults(definition);
        if (!parameters.is_object()) throw std::invalid_argument("Initial parameters must be an object.");
        for (const auto &[name, value] : parameters.items()) {
            if (!defaults.contains(name)) throw std::invalid_argument("Unknown graph parameter: " + name);
            if (!value.is_number() || !std::isfinite(value.get<double>()))
                throw std::invalid_argument("Initial parameters must be finite scalars.");
        }
        for (const auto &node : definitions_.at(definition).at("nodes"))
            if (node.at("op") == "graph.input" && !node.contains("default") &&
                !parameters.contains(node.at("id").get<std::string>()))
                throw std::invalid_argument("Missing initial parameter: " + node.at("id").get<std::string>());
        Json instance{{"id", std::move(id)}, {"op", "graph.instance"},
            {"definition", std::move(definition)}, {"parameters", std::move(parameters)},
            {"owner", {{"scene", owner_->scene}, {"object", owner_->object}}}};
        return submit(Json::array({std::move(instance)}), Json::array());
    } catch (const std::exception &error) {
        notice_ = {InspectorNoticeKind::Error, error.what()};
        return false;
    }
}

bool GraphAttachmentModel::detach(std::string id) {
    if (!canEdit()) return false;
    if (std::none_of(instances_.begin(), instances_.end(),
                     [&](const Json &node) { return node.at("id") == id; })) return false;
    return submit(Json::array(), Json::array({std::move(id)}));
}
} // namespace PelicanStudio
