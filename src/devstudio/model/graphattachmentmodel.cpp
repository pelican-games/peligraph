#include "graphattachmentmodel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
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
    nodes_ = Json::array();
    values_instance_.clear();
    values_ = Json::array();
    ++report_revision_;
    ++content_revision_;
}

const Json *GraphAttachmentModel::findInstance(std::string_view id) const {
    for (const auto &instance : instances_)
        if (std::string_view{instance.value("id", std::string{})} == id) return &instance;
    return nullptr;
}

void GraphAttachmentModel::setConnected(bool connected) {
    if (connected_ == connected) return;
    connected_ = connected;
    pending_.reset();
    read_.reset();
    catalog_requested_ = false;
    catalog_ = Json::object();
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
    read_.reset();
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

void GraphAttachmentModel::read(RequestKind kind, std::string method, Json params) {
    // A report is not an edit: it takes its own slot so selecting a row never
    // blocks Attach/Detach, and a stale answer is dropped rather than applied.
    const auto id = next_request_++;
    read_ = PendingRequest{id, selection_revision_, kind};
    requests_.push_back({id, std::move(method), std::move(params)});
}

void GraphAttachmentModel::requestValues() {
    if (connected_ && !values_instance_.empty())
        read(RequestKind::Values, "get_graph_values", {{"instance", values_instance_}});
}

void GraphAttachmentModel::watchInstance(std::string instance) {
    // Idempotent: a list rebuild that lands on the same attachment must not
    // throw away the values it is already showing.
    if (values_instance_ == instance) return;
    values_instance_ = std::move(instance);
    values_ = Json::array();
    ++report_revision_;
    requestValues();
}

void GraphAttachmentModel::requestCatalog() {
    if (!connected_ || catalog_requested_) return;
    catalog_requested_ = true;
    read(RequestKind::Catalog, "get_graph_catalog", Json::object());
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
    if (read_ && read_->id == id) {
        // A report that did not arrive leaves the panel showing the document's
        // own values, which is the truth it can still vouch for.
        const auto kind = read_->kind;
        read_.reset();
        if (kind == RequestKind::Catalog) catalog_requested_ = false;
        return;
    }
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
    if (read_ && read_->id == id) {
        const auto pending = *read_;
        read_.reset();
        if (pending.selection_revision != selection_revision_) return;
        try {
            const auto response = Json::parse(text);
            if (response.contains("error")) {
                if (pending.kind == RequestKind::Catalog) catalog_requested_ = false;
                return;
            }
            if (pending.kind == RequestKind::Catalog) catalog_ = response;
            else if (response.value("instance", std::string{}) == values_instance_)
                values_ = response.value("inputs", Json::array());
            // A report changes what the panel shows, never what the document
            // holds, so it must not make the attachment list rebuild itself.
            ++report_revision_;
        } catch (const std::exception &) {
            if (pending.kind == RequestKind::Catalog) catalog_requested_ = false;
        }
        return;
    }
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
            auto nodes = response.value("nodes", Json::array());
            auto digest = response.at("source_digest").at("hex").get<std::string>();
            if (!definitions.is_object() || !instances.is_array() || !nodes.is_array() || digest.empty())
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
            nodes_ = std::move(nodes);
            instances_ = std::move(selected);
            digest_ = std::move(digest);
            ++content_revision_;
            // A committed edit changed the document under the reported values;
            // ask again rather than showing the previous generation's numbers.
            values_ = Json::array();
            ++report_revision_;
            requestValues();
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

std::vector<GraphInstanceInputRow> GraphAttachmentModel::instanceInputs(std::string_view instance) const {
    std::vector<GraphInstanceInputRow> rows;
    const auto *node = findInstance(instance);
    if (!node) return rows;
    const auto definition = node->value("definition", std::string{});
    if (!definitions_.contains(definition) || !definitions_.at(definition).contains("nodes")) return rows;
    const auto inputs = node->value("inputs", Json::object());
    const auto parameters = node->value("parameters", Json::object());
    std::map<std::string, const Json *> reported;
    if (std::string_view{values_instance_} == instance)
        for (const auto &entry : values_)
            reported.emplace(entry.value("port", std::string{}), &entry);
    for (const auto &declared : definitions_.at(definition).at("nodes")) {
        if (declared.value("op", std::string{}) != "graph.input") continue;
        GraphInstanceInputRow row;
        row.port = declared.value("id", std::string{});
        if (row.port.empty()) continue;
        const auto stored = parameters.find(row.port);
        if (stored != parameters.end() && stored->is_number()) {
            row.has_parameter = row.has_stored = true;
            row.stored = stored->get<double>();
        } else if (declared.contains("default") && declared.at("default").is_number()) {
            row.has_stored = true;
            row.stored = declared.at("default").get<double>();
        }
        // The mode is the document's, not the expansion's: a connected port
        // leaves no node behind to read it from.
        if (const auto edge = inputs.find(row.port); edge != inputs.end() && edge->is_object()) {
            row.mode = "connected";
            row.source_node = edge->value("node", std::string{});
            row.source_port = edge->value("port", std::string{});
        } else {
            row.mode = row.has_parameter ? "parameter" : "default";
        }
        if (const auto found = reported.find(row.port);
            found != reported.end() && found->second->contains("value") &&
            found->second->at("value").is_number()) {
            row.has_value = true;
            row.value = found->second->at("value").get<double>();
            row.value_source = found->second->value("source", std::string{});
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<GraphOutputCandidate> GraphAttachmentModel::outputCandidates(std::string_view instance) const {
    // The catalog says which declared output ports carry a scalar. Until it
    // arrives the panel offers everything the document declares and lets the
    // engine refuse a mismatch by name.
    std::map<std::string, std::set<std::string>> scalar_ports;
    for (const auto &module : catalog_.value("modules", Json::array()))
        for (const auto &operation : module.value("operations", Json::array()))
            for (const auto &output : operation.value("outputs", Json::array()))
                if (output.value("type", Json::object()).value("semantic", std::string{}) ==
                    "pelican.value.scalar@1")
                    scalar_ports[operation.value("op", std::string{})]
                        .insert(output.value("port", std::string{}));
    const bool filter = !scalar_ports.empty();
    std::vector<GraphOutputCandidate> result;
    for (const auto &node : nodes_) {
        const auto id = node.value("id", std::string{});
        if (id.empty() || std::string_view{id} == instance) continue;
        const auto op = node.value("op", std::string{});
        if (op == "graph.instance") {
            const auto definition = node.value("definition", std::string{});
            if (!definitions_.contains(definition)) continue;
            // items() borrows its container, so it must not be a temporary.
            const auto outputs = definitions_.at(definition).value("outputs", Json::object());
            for (const auto &[port, endpoint] : outputs.items()) {
                (void)endpoint;
                result.push_back({id, port});
            }
            continue;
        }
        if (op == "graph.input") {
            result.push_back({id, "value"});
            continue;
        }
        const auto found = scalar_ports.find(op);
        for (const auto &port : node.value("outputs", Json::array())) {
            if (!port.is_string()) continue;
            const auto name = port.get<std::string>();
            if (filter && (found == scalar_ports.end() || !found->second.contains(name))) continue;
            result.push_back({id, name});
        }
    }
    return result;
}

Json GraphAttachmentModel::curveControlOps(const std::string &instance, const std::string &port) const {
    // Node IDs are the caller's, so the panel and a script sending the same
    // ops produce the same document bytes.
    std::set<std::string> taken;
    for (const auto &node : nodes_) taken.insert(node.value("id", std::string{}));
    const auto unique = [&taken](const std::string &base) {
        auto result = base;
        std::uint64_t suffix = 2;
        while (taken.contains(result)) result = base + "_" + std::to_string(suffix++);
        taken.insert(result);
        return result;
    };
    // The scene clock the engine already supplies. Reuse the document's own
    // when it has one, so two curves share one time source.
    std::string clock;
    for (const auto &node : nodes_)
        if (node.value("op", std::string{}) == "value.input" &&
            node.value("parameters", Json::object()).value("name", std::string{}) == "scene_time") {
            clock = node.value("id", std::string{});
            break;
        }
    const auto base = instance + "__" + port;
    auto ops = Json::array();
    const auto add = [&ops](Json node) {
        Json op = Json::object();
        op["op"] = "add_node";
        op["node"] = std::move(node);
        ops.push_back(std::move(op));
    };
    if (clock.empty()) {
        clock = unique(base + "_time");
        Json node = Json::object();
        node["id"] = clock;
        node["op"] = "value.input";
        node["parameters"] = Json{{"name", "scene_time"}};
        node["outputs"] = Json::array({"value"});
        add(std::move(node));
    }
    const auto playback = unique(base + "_playback");
    const auto curve = unique(base + "_curve");
    {
        Json node = Json::object();
        node["id"] = playback;
        node["op"] = "value.playback";
        node["inputs"] = Json{{"elapsed", Json{{"node", clock}, {"port", "value"}}}};
        node["outputs"] = Json::array({"value"});
        add(std::move(node));
    }
    // Two keys holding the value the port has right now: turning the mode into
    // a curve must not move the number under the user.
    double start = 0;
    for (const auto &row : instanceInputs(instance))
        if (row.port == port && row.has_stored) start = row.stored;
    {
        Json node = Json::object();
        node["id"] = curve;
        node["op"] = "value.curve";
        node["inputs"] = Json{{"time", Json{{"node", playback}, {"port", "value"}}}};
        node["parameters"] = Json{{"keys", Json::array({Json{{"time", 0}, {"value", start}},
                                                        Json{{"time", 1}, {"value", start}}})}};
        node["outputs"] = Json::array({"value"});
        add(std::move(node));
    }
    Json connect = Json::object();
    connect["op"] = "set_input";
    connect["node"] = instance;
    connect["port"] = port;
    connect["source"] = Json{{"node", curve}, {"port", "value"}};
    ops.push_back(std::move(connect));
    return ops;
}

bool GraphAttachmentModel::submitOps(Json ops) {
    if (!canEdit()) return false;
    edit_selection_revision_ = selection_revision_;
    notice_ = {InspectorNoticeKind::Information, "Saving graph changes..."};
    // One Inspector action is one op list, one ticket and one CAS: two edits in
    // the same frame cannot both land.
    request(RequestKind::Submit, "edit_graph_document",
            {{"base_source_digest", digest_}, {"ops", std::move(ops)}});
    return true;
}

bool GraphAttachmentModel::setInstanceParameter(const std::string &instance, const std::string &port, double value) {
    if (!canEdit()) return false;
    try {
        const auto *node = findInstance(instance);
        if (!node) throw std::invalid_argument("Select an attached graph first.");
        if (!std::isfinite(value)) throw std::invalid_argument("Enter a finite scalar value.");
        auto parameters = node->value("parameters", Json::object());
        parameters[port] = value;
        Json op = Json::object();
        op["op"] = "set_node_parameters";
        op["node"] = instance;
        op["parameters"] = std::move(parameters);
        return submitOps(Json::array({std::move(op)}));
    } catch (const std::exception &error) {
        notice_ = {InspectorNoticeKind::Error, error.what()};
        return false;
    }
}

bool GraphAttachmentModel::controlWithCurve(const std::string &instance, const std::string &port) {
    if (!canEdit() || !findInstance(instance)) return false;
    return submitOps(curveControlOps(instance, port));
}

bool GraphAttachmentModel::connectInstanceInput(const std::string &instance, const std::string &port,
                                                const GraphOutputCandidate &source) {
    if (!canEdit() || !findInstance(instance)) return false;
    if (source.node.empty() || source.port.empty()) return false;
    Json op = Json::object();
    op["op"] = "set_input";
    op["node"] = instance;
    op["port"] = port;
    op["source"] = Json{{"node", source.node}, {"port", source.port}};
    return submitOps(Json::array({std::move(op)}));
}

bool GraphAttachmentModel::disconnectInstanceInput(const std::string &instance, const std::string &port) {
    if (!canEdit() || !findInstance(instance)) return false;
    // The stored parameter was never removed, so this is the way back.
    Json op = Json::object();
    op["op"] = "set_input";
    op["node"] = instance;
    op["port"] = port;
    op["source"] = nullptr;
    return submitOps(Json::array({std::move(op)}));
}

bool GraphAttachmentModel::detach(std::string id) {
    if (!canEdit()) return false;
    if (std::none_of(instances_.begin(), instances_.end(),
                     [&](const Json &node) { return node.at("id") == id; })) return false;
    return submit(Json::array(), Json::array({std::move(id)}));
}
} // namespace PelicanStudio
