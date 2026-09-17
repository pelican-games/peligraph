#include "graphcanvasmodel.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace PelicanStudio {
namespace {
using Json = nlohmann::json;

std::string diagnostic(const Json &response) {
    const auto &error = response.at("error");
    if (error.is_string()) return error.get<std::string>();
    auto message = error.value("message", std::string{"The graph read failed."});
    if (error.contains("code")) {
        const auto &code = error.at("code");
        message += " (" + (code.is_string() ? code.get<std::string>() : code.dump()) + ")";
    }
    return message;
}

std::string text(const Json &value, const char *key) {
    const auto found = value.find(key);
    if (found == value.end() || !found->is_string()) return {};
    return found->get<std::string>();
}

// The node ids of a definition's graph.input nodes, in declaration order. An
// instance's input ports are exactly those: the catalog cannot declare them,
// because they follow the definition rather than the operation.
std::vector<std::string> definitionInputs(const Json &definition) {
    std::vector<std::string> ports;
    const auto nodes = definition.find("nodes");
    if (nodes == definition.end() || !nodes->is_array()) return ports;
    for (const auto &node : *nodes)
        if (node.is_object() && text(node, "op") == "graph.input")
            ports.push_back(text(node, "id"));
    return ports;
}

// A logical type and a logical type pattern both name the semantic type under
// the same key. That name is the whole of what the socket needs.
std::string semanticTypeName(const Json &owner, const char *key) {
    const auto found = owner.find(key);
    if (found == owner.end() || !found->is_object()) return {};
    return text(*found, "semantic");
}

// graph.input is structural: no module registers it, so the catalog cannot
// declare its type. Composition expands it into value.parameter, whose output
// is the scalar -- so that, and not "unknown", is what leaves its port.
constexpr auto kGraphInputType = "pelican.value.scalar@1";
} // namespace

void GraphCanvasModel::request(RequestKind kind, std::string method, Json params) {
    const auto id = next_request_++;
    pending_ = PendingRequest{id, kind};
    requests_.push_back({id, std::move(method), std::move(params)});
}

void GraphCanvasModel::read(RequestKind kind, std::string method, Json params) {
    const auto id = next_request_++;
    // One slot per kind. Asking again replaces the slot, so the older answer
    // is dropped when it arrives rather than overwriting the newer one.
    std::erase_if(reads_, [kind](const PendingRequest &pending) {
        return pending.kind == kind;
    });
    reads_.push_back(PendingRequest{id, kind});
    requests_.push_back({id, std::move(method), std::move(params)});
}

std::optional<GraphCanvasModel::PendingRequest>
GraphCanvasModel::takeRead(std::uint64_t id) {
    const auto found = std::ranges::find(reads_, id, &PendingRequest::id);
    if (found == reads_.end()) return std::nullopt;
    const auto pending = *found;
    reads_.erase(found);
    return pending;
}

void GraphCanvasModel::clearContext() {
    digest_.clear();
    nodes_ = Json::array();
    definitions_ = Json::object();
    layout_ = Json::object();
    definitions_reported_ = false;
    context_received_ = false;
    breadcrumb_ = {std::string{}};
    values_received_ = false;
    values_published_ = false;
    values_ = Json::array();
    clearObservation();
    ++report_revision_;
    ++content_revision_;
}

void GraphCanvasModel::clearObservation() {
    node_values_instance_.clear();
    node_values_received_ = false;
    node_values_published_ = false;
    node_values_ = Json::array();
    // The question in the air was about what is no longer on screen. Letting
    // its slot stand would make the next ask look like a duplicate of it.
    std::erase_if(reads_, [](const PendingRequest &pending) {
        return pending.kind == RequestKind::NodeValues ||
               pending.kind == RequestKind::Status;
    });
}

void GraphCanvasModel::setWatchedInstance(std::string instance) {
    values_instance_ = std::move(instance);
    values_ = Json::array();
    values_received_ = false;
    values_published_ = false;
    clearObservation();
    clearTimeline();
    ++report_revision_;
}

void GraphCanvasModel::setConnected(bool connected) {
    if (connected_ == connected) return;
    connected_ = connected;
    pending_.reset();
    reads_.clear();
    scrubbing_ = false;
    status_received_ = false;
    status_time_ = 0;
    animation_available_ = false;
    animation_sinks_ = Json::array();
    clearTimeline();
    requests_.clear();
    catalog_requested_ = false;
    catalog_received_ = false;
    operations_ = Json::object();
    types_ = Json::object();
    failure_ = {};
    edit_notice_ = {};
    ticket_.clear();
    clearContext();
    refresh();
    requestCatalog();
}

void GraphCanvasModel::refresh() {
    if (canRefresh()) request(RequestKind::Context, "get_graph_instances", Json::object());
}

void GraphCanvasModel::requestCatalog() {
    if (!connected_ || catalog_requested_) return;
    catalog_requested_ = true;
    read(RequestKind::Catalog, "get_graph_catalog", Json::object());
}

void GraphCanvasModel::requestValues() {
    if (connected_ && !values_instance_.empty())
        read(RequestKind::Values, "get_graph_values", {{"instance", values_instance_}});
}

std::string GraphCanvasModel::nodeValuesInstance() const {
    // The root document is the empty question; a definition body is read
    // through the instance the "Values from" control names, because a
    // definition on its own has no generation that fed it.
    return currentScope().empty() ? std::string{} : values_instance_;
}

void GraphCanvasModel::requestObservation() {
    // The overlay is what the user asked for by naming an instance. Without
    // one, the canvas is the document's and nothing is read.
    if (!connected_ || values_instance_.empty()) return;
    // A pair already in the air is the answer to the same question. Asking
    // again would only drop it: a Live tick on an engine slower than the tick
    // must not build a queue of reads nobody will ever look at.
    if (std::ranges::any_of(reads_, [](const PendingRequest &pending) {
            return pending.kind == RequestKind::NodeValues;
        }))
        return;
    const auto instance = nodeValuesInstance();
    // The status goes first, and the order is load-bearing: one client, one
    // pipe, replies in the order they were asked for. A Timeline sample is
    // taken when the node values land, and it is stamped with the time the
    // status just reported -- so the status has to be the earlier question or
    // every sample would carry the previous tick's clock.
    read(RequestKind::Status, "get_status", Json::object());
    read(RequestKind::NodeValues, "get_graph_node_values",
         instance.empty() ? Json::object() : Json{{"instance", instance}});
}

void GraphCanvasModel::watchInstance(std::string instance) {
    if (values_instance_ == instance) return;
    setWatchedInstance(std::move(instance));
    requestValues();
    requestObservation();
}

void GraphCanvasModel::setLive(bool live) {
    if (live_ == live) return;
    live_ = live;
    ++report_revision_;
    if (live_) requestObservation();
}

void GraphCanvasModel::setTimelineNode(std::string id) {
    if (timeline_node_ == id) return;
    timeline_node_ = std::move(id);
    clearTimeline();
    // No revision bump: which node the strip follows is the strip's business,
    // and nothing on the canvas is drawn differently for it. Bumping here
    // would redraw the canvas on every selection -- including the selection a
    // click on a pill makes, which would delete the editor that same click
    // just opened.
}

void GraphCanvasModel::clearTimeline() {
    timeline_.clear();
    timeline_ports_.clear();
}

bool GraphCanvasModel::scrub(double time) {
    if (!connected_ || scrubbing_) return false;
    if (!std::isfinite(time)) return false;
    scrubbing_ = true;
    // O-1: a scrub is the clock plus one drawn frame. The order is the whole
    // of the rule -- rendering before the time is set draws the old moment --
    // so the two go out as one pair, in it. Neither takes the edit slot: a
    // scrub is not a change to the document, and must not make the canvas
    // look busy to an edit that is trying to land.
    requests_.push_back({next_request_++, "set_time", Json{{"t", time}}});
    scrub_render_ = next_request_++;
    requests_.push_back({scrub_render_, "render_frame", Json::object()});
    return true;
}

void GraphCanvasModel::poll() {
    if (!connected_) return;
    requestCatalog();
    // Live is the only thing that reads on a clock. Off, the overlay follows
    // the document, which is what it did before the switch existed.
    if (live_) requestObservation();
    // An accepted edit owns the request slot until the engine says what became
    // of it. Nothing else may take that slot in the meantime.
    if (!pending_ && !ticket_.empty())
        request(RequestKind::Result, "get_edit_result", {{"ticket", ticket_}});
}

std::vector<InspectorRpcRequest> GraphCanvasModel::takeRpcRequests() {
    return std::exchange(requests_, {});
}

void GraphCanvasModel::receiveRpcFailure(std::uint64_t id, std::string message) {
    if (scrubbing_ && scrub_render_ == id) {
        scrubbing_ = false;
        scrub_render_ = 0;
        failure_ = {InspectorNoticeKind::Error, std::move(message)};
        ++report_revision_;
        return;
    }
    if (const auto pending = takeRead(id)) {
        const auto kind = pending->kind;
        if (kind == RequestKind::Catalog) catalog_requested_ = false;
        // The canvas keeps standing: a report that did not arrive removes an
        // overlay, never the graph the document already described.
        failure_ = {InspectorNoticeKind::Error, std::move(message)};
        ++report_revision_;
        return;
    }
    if (!pending_ || pending_->id != id) return;
    const auto kind = pending_->kind;
    pending_.reset();
    if (kind == RequestKind::Result) {
        // A poll that did not arrive cancels neither the engine's ticket nor
        // the wait for it: the next poll asks again.
        edit_notice_ = {InspectorNoticeKind::Error, std::move(message)};
        ++report_revision_;
        return;
    }
    if (kind == RequestKind::Submit) {
        edit_notice_ = {InspectorNoticeKind::Error, std::move(message)};
        ticket_.clear();
        // The gesture is gone; the document the canvas shows may not be.
        refresh();
        return;
    }
    failure_ = {InspectorNoticeKind::Error, std::move(message)};
    ++content_revision_;
}

void GraphCanvasModel::receiveRpcResult(std::uint64_t id, std::string_view json) {
    if (scrubbing_ && scrub_render_ == id) {
        // The frame the scrub asked for has been drawn; what it produced is
        // read back the ordinary way, on the read slots.
        scrubbing_ = false;
        scrub_render_ = 0;
        requestObservation();
        ++report_revision_;
        return;
    }
    if (const auto slot = takeRead(id)) {
        const auto pending = *slot;
        try {
            const auto response = Json::parse(json);
            if (response.contains("error")) {
                if (pending.kind == RequestKind::Catalog) catalog_requested_ = false;
                failure_ = {InspectorNoticeKind::Error, diagnostic(response)};
                ++report_revision_;
                return;
            }
            if (pending.kind == RequestKind::Catalog) {
                auto operations = Json::object();
                auto types = Json::object();
                for (const auto &module : response.value("modules", Json::array())) {
                    for (const auto &operation : module.value("operations", Json::array()))
                        if (operation.is_object() && operation.contains("op"))
                            operations[text(operation, "op")] = operation;
                    for (const auto &schema : module.value("types", Json::array()))
                        if (schema.is_object() && schema.contains("id"))
                            types[text(schema, "id")] = schema;
                }
                operations_ = std::move(operations);
                types_ = std::move(types);
                catalog_received_ = true;
            } else if (pending.kind == RequestKind::Status) {
                status_time_ = response.value("time", 0.0);
                status_received_ = true;
                const auto animation = response.find("animation");
                animation_available_ =
                    animation != response.end() && animation->is_object() &&
                    animation->value("available", false);
                animation_sinks_ = animation_available_
                                       ? animation->value("sinks", Json::array())
                                       : Json::array();
            } else if (pending.kind == RequestKind::NodeValues) {
                // The reply names what it answered for. A scope change in
                // flight means the answer in hand is about the other scope,
                // and a value drawn against the wrong node list is worse than
                // no value at all.
                node_values_instance_ = text(response, "instance");
                node_values_ = response.value("nodes", Json::array());
                node_values_published_ = response.value("published", false);
                node_values_received_ = true;
                appendTimelineSample(response.value("frame", std::uint64_t{0}));
            } else if (text(response, "instance") == values_instance_) {
                values_ = response.value("inputs", Json::array());
                values_published_ = response.value("published", false);
                values_received_ = true;
            }
            failure_ = {};
            ++report_revision_;
        } catch (const std::exception &error) {
            if (pending.kind == RequestKind::Catalog) catalog_requested_ = false;
            failure_ = {InspectorNoticeKind::Error, error.what()};
            ++report_revision_;
        }
        return;
    }
    if (!pending_ || pending_->id != id) return;
    const auto kind = pending_->kind;
    pending_.reset();
    if (kind == RequestKind::Submit || kind == RequestKind::Result) {
        try {
            const auto response = Json::parse(json);
            if (response.contains("error")) {
                // A CAS mismatch and a named refusal arrive the same way: the
                // gesture is discarded, the reason is shown, and the canvas
                // reads the document again rather than keeping a stale one.
                ticket_.clear();
                edit_notice_ = {InspectorNoticeKind::Error, diagnostic(response)};
                refresh();
                return;
            }
            if (kind == RequestKind::Submit) {
                if (response.at("status") != "accepted")
                    throw std::invalid_argument("The engine did not accept the graph edit.");
                ticket_ = response.at("ticket").get<std::string>();
                if (ticket_.empty())
                    throw std::invalid_argument("The engine returned no graph edit ticket.");
                poll();
                return;
            }
            const auto status = response.at("status").get<std::string>();
            // Still queued. Nothing on the canvas has changed, so nothing is
            // redrawn: a poll every quarter second must not throw away the
            // drag or the editor the user is in the middle of.
            if (status == "pending") return;
            ticket_.clear();
            if (status != "committed")
                throw std::invalid_argument("The graph edit did not commit: " + status);
            edit_notice_ = {InspectorNoticeKind::Success, "Graph change saved."};
            refresh();
        } catch (const std::exception &error) {
            ticket_.clear();
            edit_notice_ = {InspectorNoticeKind::Error, error.what()};
            refresh();
        }
        return;
    }
    try {
        const auto response = Json::parse(json);
        if (response.contains("error")) {
            failure_ = {InspectorNoticeKind::Error, diagnostic(response)};
            ++content_revision_;
            return;
        }
        auto nodes = response.value("nodes", Json::array());
        if (!nodes.is_array()) throw std::invalid_argument("The graph document has no node array.");
        const auto definitions = response.find("definitions");
        // A reply without definitions is not a broken canvas: the root scope
        // is still the whole document the engine published. Say so and draw it.
        definitions_reported_ = definitions != response.end() && definitions->is_object();
        definitions_ = definitions_reported_ ? *definitions : Json::object();
        // N-6: the root scope's placement. A reply from an engine that predates
        // it simply says nothing, which reads as "no placement yet".
        auto layout = response.value("layout", Json::object());
        layout_ = layout.is_object() ? std::move(layout) : Json::object();
        nodes_ = std::move(nodes);
        digest_ = response.value("source_digest", Json::object()).value("hex", std::string{});
        context_received_ = true;
        failure_ = {};
        // The scope trail can name a definition the new document dropped.
        std::vector<std::string> trail{std::string{}};
        for (std::size_t index = 1; index < breadcrumb_.size(); ++index)
            if (definitions_.contains(breadcrumb_[index])) trail.push_back(breadcrumb_[index]);
        breadcrumb_ = std::move(trail);
        ++content_revision_;
        // The document moved under the reported values: ask again rather than
        // showing the previous generation's numbers against new ports.
        values_ = Json::array();
        values_received_ = false;
        values_published_ = false;
        clearObservation();
        ++report_revision_;
        requestValues();
        requestObservation();
    } catch (const std::exception &error) {
        failure_ = {InspectorNoticeKind::Error, error.what()};
        ++content_revision_;
    }
}

std::vector<GraphCanvasScope> GraphCanvasModel::scopes() const {
    std::vector<GraphCanvasScope> result;
    result.push_back(GraphCanvasScope{std::string{}, "root", true});
    for (const auto &[name, definition] : definitions_.items()) {
        (void)definition;
        result.push_back(GraphCanvasScope{name, name, false});
    }
    return result;
}

bool GraphCanvasModel::selectScope(std::string_view scope) {
    if (scope.empty()) {
        breadcrumb_ = {std::string{}};
        enteredScope();
        return true;
    }
    if (!definitions_.contains(std::string{scope})) return false;
    breadcrumb_ = {std::string{}, std::string{scope}};
    enteredScope();
    return true;
}

void GraphCanvasModel::enteredScope() {
    // The scope decides which instance the node values answer for, so the one
    // in hand is about the scope that was left.
    clearObservation();
    ++report_revision_;
    requestObservation();
}

bool GraphCanvasModel::canEnterInstance(std::string_view node_id) const {
    const auto *node = findNode(currentScope(), node_id);
    if (node == nullptr || text(*node, "op") != "graph.instance") return false;
    const auto definition = text(*node, "definition");
    return !definition.empty() && definitions_.contains(definition);
}

bool GraphCanvasModel::enterInstance(std::string_view node_id) {
    if (!canEnterInstance(node_id)) return false;
    // Descending into an instance is also a choice of whose values to watch:
    // the definition body on screen belongs to the instance that was opened.
    // The watch is set before the scope moves and the reads go out after, so
    // one descent is one question rather than the root's and then the body's.
    const auto *node = findNode(currentScope(), node_id);
    const auto definition = text(*node, "definition");
    const bool watch = currentScope().empty() &&
                       node->find("owner") != node->end() &&
                       values_instance_ != node_id;
    if (watch) setWatchedInstance(std::string{node_id});
    breadcrumb_.push_back(definition);
    enteredScope();
    if (watch) requestValues();
    return true;
}

bool GraphCanvasModel::leaveScope() {
    if (breadcrumb_.size() <= 1) return false;
    breadcrumb_.pop_back();
    enteredScope();
    return true;
}

const Json *GraphCanvasModel::scopeNodes(std::string_view scope) const {
    if (scope.empty()) return &nodes_;
    const auto found = definitions_.find(std::string{scope});
    if (found == definitions_.end()) return nullptr;
    const auto nodes = found->find("nodes");
    if (nodes == found->end() || !nodes->is_array()) return nullptr;
    return &*nodes;
}

const Json *GraphCanvasModel::findNode(std::string_view scope, std::string_view id) const {
    const auto *nodes = scopeNodes(scope);
    if (nodes == nullptr) return nullptr;
    for (const auto &node : *nodes)
        if (node.is_object() && std::string_view{text(node, "id")} == id) return &node;
    return nullptr;
}

const Json *GraphCanvasModel::nodeOutputs(std::string_view scope,
                                          std::string_view id) const {
    if (!node_values_received_ || !node_values_published_) return nullptr;
    // The reply answers one question. If the scope on screen would ask a
    // different one, the answer in hand is not about these nodes.
    const auto expected = scope.empty() ? std::string{} : values_instance_;
    if (node_values_instance_ != expected) return nullptr;
    for (const auto &reported : node_values_) {
        if (!reported.is_object()) continue;
        if (std::string_view{text(reported, "node")} != id) continue;
        const auto outputs = reported.find("outputs");
        if (outputs == reported.end() || !outputs->is_object()) return nullptr;
        return &*outputs;
    }
    return nullptr;
}

void GraphCanvasModel::appendTimelineSample(std::uint64_t frame) {
    if (!live_ || timeline_node_.empty()) return;
    const auto *outputs = nodeOutputs(currentScope(), timeline_node_);
    if (outputs == nullptr) return;
    // The port list is fixed by the first sample of a run: a line that changed
    // which port it meant halfway along would be two lines drawn as one.
    if (timeline_ports_.empty())
        for (const auto &[port, value] : outputs->items()) {
            if (!value.is_number()) continue;
            if (timeline_ports_.size() >= timelineTraceLimit) break;
            timeline_ports_.push_back(port);
        }
    if (timeline_ports_.empty()) return;

    GraphCanvasTimelineSample sample;
    sample.time = status_time_;
    sample.frame = frame;
    sample.values.reserve(timeline_ports_.size());
    for (const auto &port : timeline_ports_) {
        const auto found = outputs->find(port);
        sample.values.push_back(found != outputs->end() && found->is_number()
                                    ? found->get<double>()
                                    : 0.0);
    }
    // The animation band, when the watched instance owns a sink. The phase and
    // the transition are the two readings that mean anything on a time axis.
    for (const auto &sink : animationSinks()) {
        if (!sink.cursors.empty()) {
            sample.has_phase = true;
            sample.phase = sink.cursors.front().normalized_phase;
        }
        if (sink.transition_active) {
            sample.has_progress = true;
            sample.progress = sink.transition_progress;
        }
        break;
    }
    timeline_.push_back(std::move(sample));
    if (timeline_.size() > timelineCapacity)
        timeline_.erase(timeline_.begin(),
                        timeline_.begin() +
                            static_cast<std::ptrdiff_t>(timeline_.size() -
                                                        timelineCapacity));
}

std::vector<GraphCanvasAnimationSink> GraphCanvasModel::animationSinks() const {
    std::vector<GraphCanvasAnimationSink> found;
    if (!animation_available_ || values_instance_.empty()) return found;
    // The owner is the instance's, read off the document. An instance with no
    // owner owns no sink, so there is nothing to match against and nothing to
    // draw -- which is not the same as "the engine reported none".
    const auto *instance = findNode({}, values_instance_);
    if (instance == nullptr) return found;
    const auto owner = instance->find("owner");
    if (owner == instance->end() || !owner->is_object()) return found;
    const auto scene = text(*owner, "scene");
    const auto object = owner->value("object", std::uint64_t{0});
    for (const auto &reported : animation_sinks_) {
        if (!reported.is_object()) continue;
        const auto sink_owner = reported.find("owner");
        if (sink_owner == reported.end() || !sink_owner->is_object()) continue;
        if (text(*sink_owner, "scene") != scene) continue;
        if (sink_owner->value("object", std::uint64_t{0}) != object) continue;
        GraphCanvasAnimationSink sink;
        sink.node = text(reported, "node");
        sink.source = text(reported, "source");
        sink.state = text(reported, "state");
        const auto transition = reported.find("transition");
        if (transition != reported.end() && transition->is_object()) {
            sink.transition_active = transition->value("active", false);
            sink.transition_target = text(*transition, "target");
            sink.transition_progress = transition->value("progress", 0.0);
        }
        for (const auto &cursor : reported.value("cursors", Json::array())) {
            if (!cursor.is_object()) continue;
            sink.cursors.push_back(GraphCanvasAnimationSink::Cursor{
                text(cursor, "clip"), cursor.value("time_seconds", 0.0),
                cursor.value("normalized_phase", 0.0)});
        }
        found.push_back(std::move(sink));
    }
    return found;
}

const Json *GraphCanvasModel::operation(std::string_view op) const {
    const auto found = operations_.find(std::string{op});
    return found == operations_.end() ? nullptr : &*found;
}

const Json *GraphCanvasModel::typeSchema(std::string_view id) const {
    const auto found = types_.find(std::string{id});
    return found == types_.end() ? nullptr : &*found;
}

std::string GraphCanvasModel::definitionOutputType(std::string_view definition,
                                                   std::string_view port,
                                                   int depth) const {
    if (depth <= 0) return {};
    const auto body = definitions_.find(std::string{definition});
    if (body == definitions_.end()) return {};
    const auto outputs = body->find("outputs");
    if (outputs == body->end() || !outputs->is_object()) return {};
    const auto endpoint = outputs->find(std::string{port});
    if (endpoint == outputs->end() || !endpoint->is_object()) return {};
    const auto producer = text(*endpoint, "node");
    const auto producer_port = text(*endpoint, "port");
    const auto *node = findNode(definition, producer);
    if (node == nullptr) return {};
    const auto op = text(*node, "op");
    if (op == "graph.input") return kGraphInputType;
    if (op == "graph.instance")
        return definitionOutputType(text(*node, "definition"), producer_port,
                                    depth - 1);
    const auto *contract = operation(op);
    if (contract == nullptr) return {};
    for (const auto &output : contract->value("outputs", Json::array()))
        if (text(output, "port") == producer_port)
            return semanticTypeName(output, "type");
    return {};
}

std::vector<GraphCanvasNode> GraphCanvasModel::nodes(std::string_view scope) const {
    std::vector<GraphCanvasNode> result;
    const auto *nodes = scopeNodes(scope);
    if (nodes == nullptr) return result;
    const bool root = scope.empty();
    for (const auto &node : *nodes) {
        if (!node.is_object()) continue;
        GraphCanvasNode drawn;
        drawn.id = text(node, "id");
        drawn.op = text(node, "op");
        drawn.label = text(node, "label");
        drawn.definition = text(node, "definition");
        drawn.instance = drawn.op == "graph.instance";
        const auto parameters = node.find("parameters");
        if (parameters != node.end() && parameters->is_object()) drawn.parameters = *parameters;
        const auto owner = node.find("owner");
        if (owner != node.end() && owner->is_object()) {
            drawn.has_owner = true;
            drawn.owner_scene = text(*owner, "scene");
            drawn.owner_object = owner->value("object", std::uint64_t{0});
        }

        const auto *contract = operation(drawn.op);
        const bool declared = contract != nullptr && text(*contract, "ports") == "declared";
        if (contract != nullptr) {
            drawn.contract_known = true;
            drawn.domain = text(*contract, "domain");
            drawn.evaluation = text(*contract, "evaluation");
            drawn.sink = contract->value("sink", false);
        }

        const auto authored_inputs = node.find("inputs");
        const bool has_authored_inputs =
            authored_inputs != node.end() && authored_inputs->is_object();

        std::vector<GraphCanvasPort> ports;
        const auto add_port = [&](std::string name, bool from_contract,
                                  std::string type = {}) {
            if (name.empty()) return;
            if (std::ranges::any_of(ports, [&](const auto &existing) {
                    return existing.port == name;
                }))
                return;
            GraphCanvasPort port;
            port.port = std::move(name);
            port.declared = from_contract;
            port.type = std::move(type);
            ports.push_back(std::move(port));
        };

        if (drawn.instance && definitions_.contains(drawn.definition)) {
            // graph.instance is parameter_dependent: the catalog declares no
            // ports for it, so the definition's own graph.input nodes are the
            // port list, in the order the definition declares them.
            for (auto &port : definitionInputs(definitions_.at(drawn.definition)))
                add_port(std::move(port), false, kGraphInputType);
        } else if (declared) {
            for (const auto &input : contract->value("inputs", Json::array())) {
                GraphCanvasPort port;
                port.port = text(input, "port");
                if (port.port.empty()) continue;
                port.declared = true;
                port.required = input.value("required", false);
                if (input.contains("accepted_type")) port.accepted_type = input.at("accepted_type").dump();
                port.type = semanticTypeName(input, "accepted_type");
                ports.push_back(std::move(port));
            }
        } else if (drawn.parameters.is_object()) {
            // Neither a contract nor a definition to read: the node's own keys
            // are all there is, and they are labelled as exactly that.
            for (const auto &[name, value] : drawn.parameters.items()) {
                (void)value;
                add_port(name, false);
            }
        }
        if (has_authored_inputs)
            for (const auto &[name, value] : authored_inputs->items()) {
                (void)value;
                add_port(name, false);
            }

        for (auto &port : ports) {
            if (has_authored_inputs) {
                const auto edge = authored_inputs->find(port.port);
                if (edge != authored_inputs->end() && edge->is_object()) {
                    port.connected = true;
                    port.source_node = text(*edge, "node");
                    port.source_port = text(*edge, "port");
                }
            }
            const auto stored = drawn.parameters.find(port.port);
            if (stored != drawn.parameters.end()) {
                port.has_parameter = true;
                port.parameter = *stored;
            }
            // Only a root instance can carry published values; a node inside a
            // definition body has no RPC that answers for it (AG-3).
            if (!root || !drawn.instance || drawn.id != values_instance_ || !values_received_)
                continue;
            for (const auto &reported : values_) {
                if (text(reported, "port") != port.port) continue;
                const auto value = reported.find("value");
                if (value != reported.end() && value->is_number()) {
                    port.has_value = true;
                    port.value = value->get<double>();
                }
                port.value_source = text(reported, "source");
                const auto connected = reported.find("connected");
                if (connected != reported.end() && connected->is_object()) {
                    port.value_connected_node = text(*connected, "node");
                    port.value_connected_port = text(*connected, "port");
                }
                break;
            }
        }
        drawn.inputs = std::move(ports);

        if (drawn.instance && definitions_.contains(drawn.definition)) {
            const auto &definition = definitions_.at(drawn.definition);
            const auto outputs = definition.find("outputs");
            if (outputs != definition.end() && outputs->is_object())
                for (const auto &[name, target] : outputs->items()) {
                    (void)target;
                    drawn.outputs.push_back(GraphCanvasOutput{
                        name, definitionOutputType(drawn.definition, name, 8)});
                }
        } else if (drawn.op == "graph.input") {
            // The structural boundary port. Its single output is the scalar
            // the expansion produces, which no catalog entry can say.
            drawn.outputs.push_back(GraphCanvasOutput{"value", kGraphInputType});
        } else if (declared) {
            for (const auto &output : contract->value("outputs", Json::array())) {
                const auto name = text(output, "port");
                if (!name.empty())
                    drawn.outputs.push_back(
                        GraphCanvasOutput{name, semanticTypeName(output, "type")});
            }
        } else {
            const auto authored = node.find("outputs");
            if (authored != node.end() && authored->is_array())
                for (const auto &output : *authored)
                    if (output.is_string())
                        drawn.outputs.push_back(
                            GraphCanvasOutput{output.get<std::string>(), {}});
        }

        // AG-3b: what the published generation left on the outputs. This is
        // the one report that reaches a definition body, so it is applied in
        // every scope rather than only to a root instance.
        if (const auto *outputs = nodeOutputs(scope, drawn.id))
            for (auto &output : drawn.outputs) {
                const auto found = outputs->find(output.port);
                if (found == outputs->end() || !found->is_number()) continue;
                output.has_value = true;
                output.value = found->get<double>();
            }
        result.push_back(std::move(drawn));
    }
    return result;
}

std::vector<GraphCanvasEdge> GraphCanvasModel::edges(std::string_view scope) const {
    std::vector<GraphCanvasEdge> result;
    const auto *nodes = scopeNodes(scope);
    if (nodes == nullptr) return result;
    std::set<std::string, std::less<>> present;
    for (const auto &node : *nodes)
        if (node.is_object()) present.insert(text(node, "id"));
    for (const auto &node : *nodes) {
        if (!node.is_object()) continue;
        const auto inputs = node.find("inputs");
        if (inputs == node.end() || !inputs->is_object()) continue;
        for (const auto &[port, edge] : inputs->items()) {
            if (!edge.is_object()) continue;
            const auto producer = text(edge, "node");
            // An edge to a node this scope does not hold would draw a line to
            // nowhere; the document says where it goes, not this canvas.
            if (!present.contains(producer)) continue;
            result.push_back(GraphCanvasEdge{producer, text(edge, "port"), text(node, "id"), port});
        }
    }
    return result;
}

std::vector<std::pair<std::string, std::string>> GraphCanvasModel::allNodeKeys() const {
    std::vector<std::pair<std::string, std::string>> keys;
    for (const auto &scope : scopes()) {
        const auto *nodes = scopeNodes(scope.id);
        if (nodes == nullptr) continue;
        for (const auto &node : *nodes)
            if (node.is_object()) keys.emplace_back(scope.id, text(node, "id"));
    }
    return keys;
}

std::vector<std::string> GraphCanvasModel::valueInstances() const {
    std::vector<std::string> instances;
    for (const auto &node : nodes_) {
        if (!node.is_object() || text(node, "op") != "graph.instance") continue;
        const auto owner = node.find("owner");
        if (owner == node.end() || !owner->is_object()) continue;
        instances.push_back(text(node, "id"));
    }
    return instances;
}

std::string GraphCanvasModel::scopeToken() const {
    const auto &scope = currentScope();
    // The engine spells the root document "root"; the canvas spells it with the
    // empty id so a definition named "root" is still a scope of its own.
    return scope.empty() ? std::string{"root"} : scope;
}

bool GraphCanvasModel::submitOps(Json ops) {
    if (!canEdit()) return false;
    if (!ops.is_array() || ops.empty()) return false;
    edit_notice_ = {InspectorNoticeKind::Information, "Saving the graph change..."};
    request(RequestKind::Submit, "edit_graph_document",
            {{"base_source_digest", digest_}, {"ops", std::move(ops)}});
    return true;
}

Json GraphCanvasModel::nodeParameters(std::string_view id) const {
    const auto *node = findNode(currentScope(), id);
    if (node == nullptr) return Json::object();
    const auto found = node->find("parameters");
    if (found == node->end() || !found->is_object()) return Json::object();
    return *found;
}

bool GraphCanvasModel::canConnect(std::string_view from_node, std::string_view from_port,
                                  std::string_view to_node, std::string_view to_port) const {
    if (from_node.empty() || to_node.empty() || from_node == to_node) return false;
    const auto drawn = nodes(currentScope());
    const auto producer = std::ranges::find(drawn, std::string{from_node}, &GraphCanvasNode::id);
    const auto consumer = std::ranges::find(drawn, std::string{to_node}, &GraphCanvasNode::id);
    if (producer == drawn.end() || consumer == drawn.end()) return false;
    const auto output = std::ranges::find(producer->outputs, std::string{from_port},
                                          &GraphCanvasOutput::port);
    const auto input = std::ranges::find(consumer->inputs, std::string{to_port},
                                         &GraphCanvasPort::port);
    if (output == producer->outputs.end() || input == consumer->inputs.end()) return false;
    // A port that declares no accepted type takes anything, and a producer the
    // catalog never typed cannot be refused on a type it does not have.
    if (output->type.empty() || input->type.empty()) return true;
    return output->type == input->type;
}

bool GraphCanvasModel::connectInput(const std::string &node, const std::string &port,
                                    const std::string &source_node,
                                    const std::string &source_port) {
    if (!canEdit()) return false;
    if (!canConnect(source_node, source_port, node, port)) return false;
    return submitOps(Json::array({Json{
        {"op", "set_input"}, {"scope", scopeToken()}, {"node", node}, {"port", port},
        {"source", Json{{"node", source_node}, {"port", source_port}}}}}));
}

bool GraphCanvasModel::disconnectInput(const std::string &node, const std::string &port) {
    if (!canEdit() || findNode(currentScope(), node) == nullptr) return false;
    return submitOps(Json::array({Json{
        {"op", "set_input"}, {"scope", scopeToken()}, {"node", node}, {"port", port},
        {"source", nullptr}}}));
}

bool GraphCanvasModel::setNodeParameter(const std::string &node, const std::string &key,
                                        const Json &value) {
    if (!canEdit() || findNode(currentScope(), node) == nullptr) return false;
    if (key.empty()) return false;
    // set_node_parameters replaces the object, so the whole of it is read back
    // and sent again with this one key changed.
    auto parameters = nodeParameters(node);
    parameters[key] = value;
    return submitOps(Json::array({Json{
        {"op", "set_node_parameters"}, {"scope", scopeToken()}, {"node", node},
        {"parameters", std::move(parameters)}}}));
}

bool GraphCanvasModel::removeNode(const std::string &node) {
    if (!canEdit() || findNode(currentScope(), node) == nullptr) return false;
    return submitOps(Json::array({Json{
        {"op", "remove_node"}, {"scope", scopeToken()}, {"node", node}}}));
}

nlohmann::json GraphCanvasModel::scopeLayout(std::string_view scope) const {
    if (scope.empty()) return layout_.is_object() ? layout_ : Json::object();
    const auto found = definitions_.find(std::string{scope});
    if (found == definitions_.end() || !found->is_object()) return Json::object();
    auto layout = found->value("layout", Json::object());
    return layout.is_object() ? layout : Json::object();
}

bool GraphCanvasModel::setLayout(const std::string &scope, const Json &layout) {
    if (!canEdit()) return false;
    return submitOps(Json::array({Json{
        {"op", "set_layout"},
        {"scope", scope.empty() ? std::string{"root"} : scope},
        {"layout", layout}}}));
}

bool GraphCanvasModel::setNodeLabel(const std::string &node, const std::string &label) {
    if (!canEdit() || findNode(currentScope(), node) == nullptr) return false;
    return submitOps(Json::array({Json{
        {"op", "set_node_label"}, {"scope", scopeToken()}, {"node", node},
        {"label", label}}}));
}

Json GraphCanvasModel::definitionDefaults(std::string_view definition) const {
    Json parameters = Json::object();
    const auto body = definitions_.find(std::string{definition});
    if (body == definitions_.end()) return parameters;
    for (const auto &node : body->value("nodes", Json::array())) {
        if (!node.is_object() || text(node, "op") != "graph.input") continue;
        const auto id = text(node, "id");
        if (id.empty()) continue;
        const auto declared = node.find("default");
        parameters[id] = declared != node.end() && declared->is_number() ? *declared : Json(0.0);
    }
    return parameters;
}

std::string GraphCanvasModel::suggestNodeId(std::string_view op) const {
    std::string base{op};
    // The op name is the honest starting point, but a node id is an identifier
    // the document points at: the dots go.
    std::ranges::replace(base, '.', '_');
    if (base.empty()) base = "node";
    const auto taken = [&](const std::string &candidate) {
        return findNode(currentScope(), candidate) != nullptr;
    };
    if (!taken(base)) return base;
    for (int suffix = 2; suffix < 10000; ++suffix) {
        auto candidate = base + "_" + std::to_string(suffix);
        if (!taken(candidate)) return candidate;
    }
    return base;
}

Json GraphCanvasModel::addNodeOps(const std::string &op, const std::string &definition) const {
    Json ops = Json::array();
    if (op.empty()) return ops;
    const auto scope = scopeToken();
    const bool root = currentScope().empty();
    const auto id = suggestNodeId(op);
    Json node{{"id", id}, {"op", op}};
    if (op == "graph.input") {
        if (root) return Json::array();
        // A boundary port every instance can already stand on: without a value
        // to fall back to, the instances that exist stop expanding.
        node["default"] = 0.0;
        ops.push_back(Json{{"op", "add_node"}, {"scope", scope}, {"node", std::move(node)}});
        return ops;
    }
    if (op == "graph.instance") {
        if (!root || definition.empty() || !definitions_.contains(definition))
            return Json::array();
        node["definition"] = definition;
        node["parameters"] = definitionDefaults(definition);
        ops.push_back(Json{{"op", "add_node"}, {"scope", scope}, {"node", std::move(node)}});
        return ops;
    }
    const auto *contract = operation(op);
    if (contract == nullptr || text(*contract, "ports") != "declared") return Json::array();
    auto outputs = Json::array();
    for (const auto &output : contract->value("outputs", Json::array())) {
        const auto name = text(output, "port");
        if (!name.empty()) outputs.push_back(name);
    }
    node["outputs"] = std::move(outputs);
    Json inputs = Json::object();
    for (const auto &input : contract->value("inputs", Json::array())) {
        const auto port = text(input, "port");
        if (port.empty() || !input.value("required", false)) continue;
        // A required input with nothing on it is not a document the engine
        // loads, so the node arrives standing on its own constant -- which is
        // also the number the row's pill then edits.
        if (semanticTypeName(input, "accepted_type") != kGraphInputType) continue;
        const auto seed = id + "__" + port;
        ops.push_back(Json{{"op", "add_node"}, {"scope", scope},
            {"node", Json{{"id", seed}, {"op", "value.constant"},
                          {"parameters", Json{{"value", 0.0}}},
                          {"outputs", Json::array({"value"})}}}});
        inputs[port] = Json{{"node", seed}, {"port", "value"}};
    }
    if (!inputs.empty()) node["inputs"] = std::move(inputs);
    ops.push_back(Json{{"op", "add_node"}, {"scope", scope}, {"node", std::move(node)}});
    return ops;
}

bool GraphCanvasModel::addNode(const std::string &op, const std::string &definition) {
    if (!canEdit()) return false;
    auto ops = addNodeOps(op, definition);
    if (ops.empty()) return false;
    return submitOps(std::move(ops));
}

std::vector<GraphCanvasCatalogEntry> GraphCanvasModel::paletteOperations(
    std::string_view text_filter, std::string_view domain) const {
    const auto lowered = [](std::string_view value) {
        std::string result{value};
        std::ranges::transform(result, result.begin(), [](unsigned char letter) {
            return static_cast<char>(std::tolower(letter));
        });
        return result;
    };
    const auto needle = lowered(text_filter);
    const auto matches = [&](const GraphCanvasCatalogEntry &entry) {
        if (!domain.empty() && entry.domain != domain) return false;
        return needle.empty() || lowered(entry.op).find(needle) != std::string::npos;
    };
    std::vector<GraphCanvasCatalogEntry> result;
    for (const auto &[op, declaration] : operations_.items()) {
        // The two structural operations are added below, on the side of the
        // boundary each belongs to, and never taken from the catalog.
        if (op == "graph.input" || op == "graph.instance") continue;
        // An operation whose ports follow its parameters cannot be authored
        // from a name alone: the palette does not pretend otherwise.
        if (text(declaration, "ports") != "declared") continue;
        GraphCanvasCatalogEntry entry{op, text(declaration, "domain"),
                                      text(declaration, "evaluation"),
                                      declaration.value("sink", false), false};
        if (matches(entry)) result.push_back(std::move(entry));
    }
    std::ranges::sort(result, {}, &GraphCanvasCatalogEntry::op);
    // A boundary port belongs to a definition body; an instance stands in the
    // root document. Offering either on the wrong side would only earn a named
    // refusal from the engine.
    if (currentScope().empty()) {
        if (!definitions_.empty()) {
            GraphCanvasCatalogEntry entry{"graph.instance", "structure", "direct", false, true};
            if (matches(entry)) result.push_back(std::move(entry));
        }
    } else {
        GraphCanvasCatalogEntry entry{"graph.input", "structure", "direct", false, true};
        if (matches(entry)) result.push_back(std::move(entry));
    }
    return result;
}

std::vector<std::string> GraphCanvasModel::paletteDomains() const {
    std::set<std::string> domains;
    for (const auto &entry : paletteOperations()) domains.insert(entry.domain);
    return {domains.begin(), domains.end()};
}

InspectorNotice GraphCanvasModel::notice() const {
    // An edit the user just attempted outranks everything: it is the one thing
    // they are waiting to hear about.
    if (edit_notice_.kind == InspectorNoticeKind::Error) return edit_notice_;
    if (failure_.kind != InspectorNoticeKind::None) return failure_;
    if (edit_notice_.kind != InspectorNoticeKind::None) return edit_notice_;
    if (context_received_ && !catalog_received_)
        return {InspectorNoticeKind::Information,
                "Operation catalog not fetched: ports and domains come from the "
                "authored document alone."};
    if (context_received_ && !definitions_reported_)
        return {InspectorNoticeKind::Information,
                "The engine reported no definitions: only the root scope can be shown."};
    if (!values_instance_.empty() && values_received_ && !values_published_)
        return {InspectorNoticeKind::Information,
                values_instance_ + " is not published: no drawn generation has fed its inputs."};
    // The node values are a separate report and can be unpublished on their
    // own -- a definition scope has no instance inputs to be unpublished.
    if (!values_instance_.empty() && node_values_received_ && !node_values_published_)
        return {InspectorNoticeKind::Information,
                values_instance_ +
                    " is not published: no drawn generation has left node values."};
    return {};
}

} // namespace PelicanStudio
