#include "graphcanvasmodel.hpp"

#include "projectformat.hpp"
#include "projectpathresolver.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <utility>

namespace PelicanStudio {
namespace {
using Json = nlohmann::json;

std::string diagnostic(const Json &response) {
    const auto &error = response.at("error");
    if (error.is_string()) return error.get<std::string>();
    // LP-0d: a graph variant (preview, XR) that refused the edit is named
    // first, whatever the code -- its graph refusal keeps the graph's code.
    // A draw-loop refusal's own `variant` is not one (LP-2).
    const auto variant = graphRefusalVariant(error);
    const auto prefix =
        variant.empty() ? std::string{} : "The " + variant + " graph refused it: ";
    // LP-4c, decision 9: a loop's or a definition's refusal is a sentence
    // made from the fields the engine lays beside its code; a view_select
    // one likewise (LP-4a).
    if (auto loop = graphLoopRefusalDescription(error); !loop.empty()) return prefix + loop;
    if (auto view = viewSelectRefusalDescription(error); !view.empty()) return prefix + view;
    auto message = error.value("message", std::string{"The graph read failed."});
    // Q-10: a code Studio has a sentence for reads as that sentence, also when
    // it arrives inside the message of a broader refusal (a sound refused
    // while the render pipeline prepared the edit).
    const auto code_text = error.contains("code") && error.at("code").is_string()
                               ? error.at("code").get<std::string>()
                               : std::string{};
    if (auto known = knownRefusalDescription(code_text, message); !known.empty())
        return prefix + known;
    if (error.contains("code")) {
        const auto &code = error.at("code");
        message += " (" + (code.is_string() ? code.get<std::string>() : code.dump()) + ")";
    }
    return prefix + message;
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

// The output names of a definition, in the order its outputs object keeps.
std::vector<std::string> definitionOutputs(const Json &definition) {
    std::vector<std::string> ports;
    const auto outputs = definition.find("outputs");
    if (outputs == definition.end() || !outputs->is_object()) return ports;
    for (const auto &[name, endpoint] : outputs->items()) {
        (void)endpoint;
        ports.push_back(name);
    }
    return ports;
}

bool isLoop(const Json &node) {
    return node.is_object() && text(node, "op") == "graph.loop";
}

// LP-4c. The bodies of a loop's branches, in order; an empty string is a
// branch with `"body": null` (or none the table can read).
std::vector<std::string> loopBranchBodies(const Json &node) {
    std::vector<std::string> bodies;
    const auto parameters = node.find("parameters");
    if (parameters == node.end() || !parameters->is_object()) return bodies;
    const auto branches = parameters->find("branches");
    if (branches == parameters->end() || !branches->is_array()) return bodies;
    for (const auto &branch : *branches)
        bodies.push_back(branch.is_object() ? text(branch, "body") : std::string{});
    return bodies;
}

// The integers of a `when` / `where` list, or nothing when one of them is not
// a literal integer (an expression the table has no form for).
std::optional<std::vector<std::int64_t>> integerList(const Json &values) {
    std::vector<std::int64_t> result;
    if (!values.is_array()) return std::nullopt;
    for (const auto &value : values) {
        if (!value.is_number_integer()) return std::nullopt;
        result.push_back(value.get<std::int64_t>());
    }
    return result;
}

// LP-4e. The draw steps of a draw_loop body (design 4.1).
bool isDrawStep(std::string_view op) {
    return op == "draw.material" || op == "draw.variant" || op == "draw.override";
}

bool isDrawItemsLoop(const Json &node) {
    if (!isLoop(node)) return false;
    const auto parameters = node.find("parameters");
    if (parameters == node.end() || !parameters->is_object()) return false;
    const auto over = parameters->find("over");
    return over != parameters->end() && over->is_object() && over->contains("draw_items");
}

std::optional<std::vector<std::string>> stringList(const Json &values) {
    std::vector<std::string> result;
    if (!values.is_array()) return std::nullopt;
    for (const auto &value : values) {
        if (!value.is_string()) return std::nullopt;
        result.push_back(value.get<std::string>());
    }
    return result;
}

// {"include": [...], "exclude": [...], "require_all": [...]}, or nothing for
// a shape the fields cannot show.
std::optional<GraphCanvasTagFilter> readTagFilter(const Json &value) {
    if (!value.is_object()) return std::nullopt;
    GraphCanvasTagFilter filter;
    for (const auto &[key, list] : value.items()) {
        auto *target = key == "include"       ? &filter.include
                       : key == "exclude"     ? &filter.exclude
                       : key == "require_all" ? &filter.require_all
                                              : nullptr;
        auto values = stringList(list);
        if (target == nullptr || !values) return std::nullopt;
        *target = std::move(*values);
    }
    return filter;
}

Json tagFilterJson(const GraphCanvasTagFilter &filter) {
    Json value = Json::object();
    if (!filter.include.empty()) value["include"] = filter.include;
    if (!filter.exclude.empty()) value["exclude"] = filter.exclude;
    if (!filter.require_all.empty()) value["require_all"] = filter.require_all;
    return value;
}

// A draw_loop `when`, or nothing when it holds a key (or an operand) the
// table has no form for -- has_variant naming a variant, say.
std::optional<GraphCanvasDrawWhen> readDrawWhen(const Json &when) {
    if (!when.is_object() || when.empty()) return std::nullopt;
    GraphCanvasDrawWhen read;
    for (const auto &[key, operand] : when.items()) {
        if ((key == "material" || key == "contract") && operand.is_string()) {
            (key == "material" ? read.material : read.contract) = operand.get<std::string>();
        } else if (key == "tags") {
            auto tags = readTagFilter(operand);
            if (!tags) return std::nullopt;
            read.tags = std::move(*tags);
        } else if ((key == "skinned" || key == "has_variant") && operand.is_boolean()) {
            (key == "skinned" ? read.skinned : read.has_variant) = operand.get<bool>();
        } else {
            return std::nullopt;
        }
    }
    return read;
}

Json drawWhenJson(const GraphCanvasDrawWhen &when) {
    Json value = Json::object();
    if (when.material) value["material"] = *when.material;
    if (when.tags) value["tags"] = tagFilterJson(*when.tags);
    if (when.contract) value["contract"] = *when.contract;
    if (when.skinned) value["skinned"] = *when.skinned;
    if (when.has_variant) value["has_variant"] = *when.has_variant;
    return value;
}

// A branch's condition, read into the table's terms.
GraphCanvasLoopBranch readLoopBranch(const Json &branch) {
    GraphCanvasLoopBranch read;
    if (!branch.is_object()) {
        read.when = "other";
        return read;
    }
    const auto body = branch.find("body");
    if (body == branch.end() || body->is_null()) read.nothing = true;
    else if (body->is_string()) read.body = body->get<std::string>();
    const auto when = branch.find("when");
    if (when == branch.end()) return read;
    read.when = "other";
    // LP-4e: a draw_loop's condition names the material's attributes.
    if (auto draw = readDrawWhen(*when)) {
        read.when = "draw";
        read.draw_when = std::move(*draw);
        return read;
    }
    if (!when->is_object() || when->size() != 1) return read;
    const auto first = when->begin();
    const std::string key = first.key();
    const Json &value = first.value();
    if (key == "element" && value.is_number_integer()) {
        read.when = "element";
        read.values = {value.get<std::int64_t>()};
    } else if (key == "element_in" || key == "element_not_in") {
        if (auto values = integerList(value)) {
            read.when = key;
            read.values = std::move(*values);
        }
    }
    return read;
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

// LP-4c: the one parameter-dependent operation the palette offers, seeded
// whole by addNodeOps.
constexpr std::string_view kFullscreenTemplate = "render.fullscreen";

// LP-4e: what a draw step hands on and a material pass's `draws` takes.
constexpr std::string_view kDrawStreamType = "pelican.render.draw_stream@1";
constexpr std::string_view kMaterialPass = "render.material_pass";

void collectUiNodes(const Json &node, const std::string &parent,
                    std::vector<GraphCanvasUiNode> &out) {
    if (!node.is_object()) return;
    const auto id = text(node, "id");
    if (id.empty()) return;
    auto path = parent.empty() ? id : parent + "/" + id;
    out.push_back(GraphCanvasUiNode{path, text(node, "type")});
    const auto children = node.find("children");
    if (children == node.end() || !children->is_array()) return;
    for (const auto &child : *children) collectUiNodes(child, path, out);
}
} // namespace

std::vector<GraphCanvasUiNode> graphCanvasUiDocumentNodes(const Json &document) {
    std::vector<GraphCanvasUiNode> nodes;
    if (!document.is_object()) return nodes;
    const auto root = document.find("root");
    if (root != document.end()) collectUiNodes(*root, {}, nodes);
    return nodes;
}

GraphCanvasUiNodesRead readProjectUiNodes(const std::filesystem::path &project_root) {
    GraphCanvasUiNodesRead read;
    try {
        std::ifstream input{project_root / "project.json", std::ios::binary};
        if (!input) {
            read.error = "project.json could not be opened, so the UI document is unknown";
            return read;
        }
        const std::string contents{std::istreambuf_iterator<char>{input},
                                   std::istreambuf_iterator<char>{}};
        const auto parsed = Pelican::parseProjectEnvelopeText(
            contents, Pelican::ProjectEnvelopeParseOptions{.ignore_engine_version = true});
        const auto &basic = parsed.envelope.basic_config;
        const auto reference = basic.find("ui_config_json");
        if (reference == basic.end() || !reference->is_string() ||
            reference->get_ref<const std::string &>().empty()) {
            read.error = "project.json names no UI document (basic_config.ui_config_json)";
            return read;
        }
        Pelican::ProjectPathResolver resolver;
        resolver.setup(project_root, false, parsed.envelope);
        read.nodes = graphCanvasUiDocumentNodes(
            Json::parse(resolver.loadText(reference->get<std::string>())));
        if (read.nodes.empty())
            read.error = "the UI document " + reference->get<std::string>() +
                         " has no node with an id";
    } catch (const std::exception &error) {
        read.nodes.clear();
        read.error = std::string{"the UI document could not be read: "} + error.what();
    }
    return read;
}

GraphCanvasAudioAssetsRead readProjectAudioAssets(const std::filesystem::path &project_root) {
    GraphCanvasAudioAssetsRead read;
    if (project_root.empty()) {
        read.error = "no project is open";
        return read;
    }
    std::error_code error;
    std::filesystem::recursive_directory_iterator walk{
        project_root, std::filesystem::directory_options::skip_permission_denied, error};
    if (error) {
        read.error = "the project folder could not be read: " + error.message();
        return read;
    }
    try {
        for (auto end = std::filesystem::recursive_directory_iterator{}; walk != end;
             walk.increment(error)) {
            if (error) break;
            const auto &entry = *walk;
            std::error_code kind;
            if (entry.is_directory(kind)) {
                const auto name = entry.path().filename().u8string();
                if (!name.empty() && name.front() == u8'.') walk.disable_recursion_pending();
                continue;
            }
            auto extension = entry.path().extension().u8string();
            std::ranges::transform(extension, extension.begin(), [](char8_t letter) {
                return letter < 0x80 ? static_cast<char8_t>(std::tolower(letter)) : letter;
            });
            if (extension != u8".wav" && extension != u8".ogg") continue;
            const auto relative = entry.path().lexically_relative(project_root).generic_u8string();
            if (relative.empty() || relative.starts_with(u8"..")) continue;
            read.assets.push_back("project://" +
                                  std::string{relative.begin(), relative.end()});
        }
    } catch (const std::exception &caught) {
        read.error = std::string{"the project folder could not be read: "} + caught.what();
    }
    std::ranges::sort(read.assets);
    if (read.assets.empty() && read.error.empty())
        read.error = "no .wav or .ogg file in the project";
    return read;
}

bool GraphCanvasModel::isAudioPlayback(std::string_view op) noexcept {
    return op == "audio.playback";
}

const std::vector<std::string> &GraphCanvasModel::audioBusChoices() {
    static const std::vector<std::string> buses{"se", "bgm"};
    return buses;
}

const std::vector<std::string> &GraphCanvasModel::audioWrapChoices() {
    static const std::vector<std::string> wraps{"once", "repeat", "ping_pong"};
    return wraps;
}

std::string_view GraphCanvasModel::uiNodeParameter(std::string_view op) noexcept {
    return op == "ui.value" || op == "ui.visible" ? std::string_view{"node"}
                                                  : std::string_view{};
}

std::vector<std::string> GraphCanvasModel::uiNodeChoices(std::string_view op) const {
    std::vector<std::string> choices;
    if (uiNodeParameter(op).empty()) return choices;
    // A value write to anything but a gauge is ui.property_mismatch@1; the
    // engine would refuse it by name, so the list does not offer it.
    const bool value = op == "ui.value";
    for (const auto &node : ui_nodes_)
        if (!value || node.type == "gauge") choices.push_back(node.path);
    return choices;
}

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
    clearHistory();
    digest_.clear();
    layout_retry_.reset();
    layout_retrying_ = false;
    nodes_ = Json::array();
    definitions_ = Json::object();
    layout_ = Json::object();
    definitions_reported_ = false;
    context_received_ = false;
    breadcrumb_ = {std::string{}};
    entered_loop_.clear();
    loop_reports_ = Json::object();
    loop_reports_received_ = false;
    view_families_.clear();
    plan_node_views_.clear();
    plan_targets_.clear();
    refusal_marks_.clear();
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

void GraphCanvasModel::requestLoopReports() {
    // Only a document with a loop has anything in loops[]: a frame plan read
    // for any other would be the whole plan for nothing. LP-4f1: or a root
    // render node, whose View field reads its axes off the plan.
    const auto wanted = [](const Json &node) {
        return isLoop(node) || (node.is_object() && takesViewSelect(text(node, "op")));
    };
    if (!connected_ || std::ranges::none_of(nodes_, wanted)) {
        loop_reports_ = Json::object();
        loop_reports_received_ = false;
        view_families_.clear();
        plan_node_views_.clear();
        plan_targets_.clear();
        return;
    }
    read(RequestKind::Loops, "get_frame_plan", Json::object());
}

void GraphCanvasModel::requestNextLoopPage() {
    if (!connected_ || !loop_page_node_.empty()) return;
    for (const auto &[node, report] : loop_reports_.items()) {
        if (loop_pages_done_.contains(node)) continue;
        if (text(report, "tier") != "draw_loop") continue;
        const auto held = report.value("elements", Json::array()).size();
        const auto total = report.contains("total") && report.at("total").is_number_unsigned()
                               ? report.at("total").get<std::size_t>()
                               : held;
        if (held >= total) {
            loop_pages_done_.insert(node);
            continue;
        }
        // The largest page the engine gives, from the first material not
        // held yet.
        loop_page_node_ = node;
        read(RequestKind::LoopPage, "get_frame_plan",
             Json{{"loop_elements", Json{{"node", node}, {"offset", held}, {"limit", 4096}}}});
        return;
    }
}

void GraphCanvasModel::receiveLoopPage(const Json &plan) {
    const auto node = std::exchange(loop_page_node_, {});
    const auto held = loop_reports_.find(node);
    const auto loops = plan.find("loops");
    bool added = false;
    if (held != loop_reports_.end() && loops != plan.end() && loops->is_array())
        for (const auto &loop : *loops) {
            if (!loop.is_object() || text(loop, "node") != node) continue;
            auto &elements = (*held)["elements"];
            if (!elements.is_array()) elements = Json::array();
            // The page is the one asked for only if it starts where the held
            // list ends; a plan that moved in between is not spliced in.
            if (loop.value("offset", std::size_t{0}) != elements.size()) break;
            for (const auto &element : loop.value("elements", Json::array())) {
                elements.push_back(element);
                added = true;
            }
            if (loop.contains("total")) (*held)["total"] = loop.at("total");
            break;
        }
    // A page that brought nothing ends that loop's reading.
    if (!added) loop_pages_done_.insert(node);
    requestNextLoopPage();
}

const Json *GraphCanvasModel::loopReport(std::string_view id) const {
    const auto found = loop_reports_.find(std::string{id});
    return found == loop_reports_.end() ? nullptr : &*found;
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
    if (!pollWanted()) return;
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
        // LP-4c: the loop report only adds marks to what the document
        // already draws. Without it the canvas says "loop" and nothing else.
        if (kind == RequestKind::Loops) return;
        if (kind == RequestKind::LoopPage) {
            loop_pages_done_.insert(std::exchange(loop_page_node_, {}));
            requestNextLoopPage();
            return;
        }
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
        // A lost submission reply may have committed. Do not offer history
        // until a fresh document is read, or guess which edit it belongs to.
        clearHistory();
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
                if (pending.kind == RequestKind::Loops) return;
                if (pending.kind == RequestKind::LoopPage) {
                    // The loop keeps the materials it has; the next loop
                    // is asked for its own.
                    loop_pages_done_.insert(std::exchange(loop_page_node_, {}));
                    requestNextLoopPage();
                    return;
                }
                if (pending.kind == RequestKind::Catalog) catalog_requested_ = false;
                failure_ = {InspectorNoticeKind::Error, diagnostic(response)};
                ++report_revision_;
                return;
            }
            if (pending.kind == RequestKind::Loops) {
                // Only loops[] is read off the plan; by loop id.
                auto reports = Json::object();
                const auto loops = response.find("loops");
                if (loops != response.end() && loops->is_array())
                    for (const auto &loop : *loops)
                        if (loop.is_object() && !text(loop, "node").empty())
                            reports[text(loop, "node")] = loop;
                loop_reports_ = std::move(reports);
                loop_reports_received_ = true;
                readPlanViews(response);
                // LP-4e: a fresh report starts every loop's pages over.
                loop_pages_done_.clear();
                loop_page_node_.clear();
                requestNextLoopPage();
                ++report_revision_;
                return;
            }
            if (pending.kind == RequestKind::LoopPage) {
                receiveLoopPage(response);
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
            if (pending.kind == RequestKind::Loops) return;
            if (pending.kind == RequestKind::LoopPage) {
                loop_pages_done_.insert(std::exchange(loop_page_node_, {}));
                requestNextLoopPage();
                return;
            }
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
                // Q-13: a placement refused for a moved document goes again
                // once the document is read (see layout_retry_).
                const auto &error = response.at("error");
                const bool moved =
                    error.is_object() &&
                    error.value("code", std::string{}) == "external_modification";
                if (moved && !std::exchange(layout_retrying_, false) &&
                    pending_history_ &&
                    pending_history_->action == HistoryAction::Edit &&
                    std::ranges::all_of(pending_history_->entry.forward,
                                        [](const Json &op) {
                                            return op.value("op", std::string{}) ==
                                                   "set_layout";
                                        }))
                    layout_retry_ = pending_history_->entry.forward;
                pending_history_.reset();
                edit_notice_ = {InspectorNoticeKind::Error, diagnostic(response)};
                // LP-4c, decision 9: and where the refusal points.
                markRefusal(error, edit_notice_.message);
                ++content_revision_;
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
            commitHistory(response);
            layout_retrying_ = false;
            refusal_marks_.clear();
            edit_notice_ = {InspectorNoticeKind::Success, "Graph change saved."};
            refresh();
        } catch (const std::exception &error) {
            ticket_.clear();
            layout_retrying_ = false;
            pending_history_.reset();
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
        const bool reported = definitions != response.end() && definitions->is_object();
        auto incoming_definitions = reported ? *definitions : Json::object();
        // N-6: the root scope's placement. A reply from an engine that predates
        // it simply says nothing, which reads as "no placement yet".
        auto layout = response.value("layout", Json::object());
        if (!layout.is_object()) layout = Json::object();
        // N-8: an engine that predates this says nothing, which reads as "not a
        // graph root" -- the safe answer, because the placement is then not
        // written rather than written where it cannot be kept.
        auto root_schema = response.value("root_schema", std::string{});
        auto source_reference = response.value("source_reference", std::string{});
        // Q-5: the reply to a `set_layout` -- the canvas's own drag coming
        // back -- changes where things sit and nothing else. Every definition
        // carries its placement inside it, so the comparison takes that member
        // out of both sides before asking whether anything else moved.
        const auto withoutPlacement = [](Json scopes) {
            for (auto &[name, scope] : scopes.items()) {
                (void)name;
                if (scope.is_object()) scope.erase("layout");
            }
            return scopes;
        };
        const bool placement_only =
            context_received_ && failure_.kind == InspectorNoticeKind::None &&
            reported == definitions_reported_ && nodes == nodes_ &&
            root_schema == root_schema_ && source_reference == source_reference_ &&
            (layout != layout_ || incoming_definitions != definitions_) &&
            withoutPlacement(incoming_definitions) == withoutPlacement(definitions_);
        definitions_reported_ = reported;
        definitions_ = std::move(incoming_definitions);
        layout_ = std::move(layout);
        nodes_ = std::move(nodes);
        root_schema_ = std::move(root_schema);
        source_reference_ = std::move(source_reference);
        digest_ = response.value("source_digest", Json::object()).value("hex", std::string{});
        if (!history_digest_.empty() && digest_ != history_digest_)
            clearHistory();
        context_received_ = true;
        failure_ = {};
        // Q-13: the placement a moved document refused, against this one.
        if (auto retry = std::exchange(layout_retry_, std::nullopt))
            layout_retrying_ = submitOps(std::move(*retry));
        if (placement_only) {
            // The nodes, their ports and the scopes are the ones already
            // drawn, so the values reported against them still stand: asking
            // again would blank every value on the canvas for a round trip.
            ++placement_revision_;
            return;
        }
        // The scope trail can name a definition the new document dropped.
        std::vector<std::string> trail{std::string{}};
        for (std::size_t index = 1; index < breadcrumb_.size(); ++index)
            if (definitions_.contains(breadcrumb_[index])) trail.push_back(breadcrumb_[index]);
        breadcrumb_ = std::move(trail);
        // LP-4c: the loop a body was entered through has to still be there,
        // still running the body on screen.
        if (!entered_loop_.empty()) {
            const auto *loop = findNode({}, entered_loop_);
            const auto bodies = loop != nullptr ? loopBranchBodies(*loop)
                                                : std::vector<std::string>{};
            if (breadcrumb_.size() != 2 ||
                std::ranges::find(bodies, breadcrumb_.back()) == bodies.end())
                entered_loop_.clear();
        }
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
        requestLoopReports();
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
    entered_loop_.clear();
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

std::string GraphCanvasModel::loopEntryBody(const Json &node) const {
    const auto bodies = loopBranchBodies(node);
    const auto chosen = loop_branch_.find(text(node, "id"));
    if (chosen != loop_branch_.end() && chosen->second < bodies.size() &&
        !bodies[chosen->second].empty() && definitions_.contains(bodies[chosen->second]))
        return bodies[chosen->second];
    for (const auto &body : bodies)
        if (!body.empty() && definitions_.contains(body)) return body;
    return {};
}

bool GraphCanvasModel::canEnterInstance(std::string_view node_id) const {
    const auto *node = findNode(currentScope(), node_id);
    if (node == nullptr) return false;
    // LP-4c, decision 5: a loop opens the body its chosen branch runs, the
    // way an instance opens its definition.
    if (currentScope().empty() && isLoop(*node)) return !loopEntryBody(*node).empty();
    if (text(*node, "op") != "graph.instance") return false;
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
    if (currentScope().empty() && isLoop(*node)) {
        // A loop owns no scene object: there is no instance to watch, and
        // the body's values are not any one element's.
        const auto body = loopEntryBody(*node);
        const auto bodies = loopBranchBodies(*node);
        loop_branch_[std::string{node_id}] = static_cast<std::size_t>(
            std::distance(bodies.begin(), std::ranges::find(bodies, body)));
        entered_loop_ = std::string{node_id};
        breadcrumb_.push_back(body);
        enteredScope();
        return true;
    }
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
    entered_loop_.clear();
    enteredScope();
    return true;
}

std::size_t GraphCanvasModel::loopBranch(std::string_view id) const {
    const auto *node = findNode({}, id);
    if (node == nullptr || !isLoop(*node)) return 0;
    const auto bodies = loopBranchBodies(*node);
    const auto body = loopEntryBody(*node);
    const auto found = std::ranges::find(bodies, body);
    return found == bodies.end() ? 0 : static_cast<std::size_t>(found - bodies.begin());
}

bool GraphCanvasModel::selectLoopBranch(std::size_t branch) {
    if (entered_loop_.empty() || breadcrumb_.size() != 2) return false;
    const auto *node = findNode({}, entered_loop_);
    if (node == nullptr) return false;
    const auto bodies = loopBranchBodies(*node);
    if (branch >= bodies.size() || bodies[branch].empty() ||
        !definitions_.contains(bodies[branch]))
        return false;
    loop_branch_[entered_loop_] = branch;
    if (breadcrumb_.back() == bodies[branch]) return true;
    breadcrumb_.back() = bodies[branch];
    enteredScope();
    return true;
}

std::string GraphCanvasModel::loopZoneBody(std::string_view loop) const {
    const auto *node = findNode({}, loop);
    if (node == nullptr || !isLoop(*node)) return {};
    return loopEntryBody(*node);
}

bool GraphCanvasModel::showLoopBranch(std::string_view loop, std::size_t branch) {
    const auto *node = findNode({}, loop);
    if (node == nullptr || !isLoop(*node)) return false;
    const auto bodies = loopBranchBodies(*node);
    if (branch >= bodies.size() || bodies[branch].empty() ||
        !definitions_.contains(bodies[branch]))
        return false;
    loop_branch_[std::string{loop}] = branch;
    return true;
}

std::vector<GraphCanvasBodyOutput> GraphCanvasModel::bodyOutputs(std::string_view definition) const {
    std::vector<GraphCanvasBodyOutput> result;
    const auto found = definitions_.find(std::string{definition});
    if (found == definitions_.end() || !found->is_object()) return result;
    const auto outputs = found->find("outputs");
    if (outputs == found->end() || !outputs->is_object()) return result;
    for (const auto &[name, endpoint] : outputs->items())
        result.push_back(GraphCanvasBodyOutput{
            name, endpoint.is_object() ? text(endpoint, "node") : std::string{},
            endpoint.is_object() ? text(endpoint, "port") : std::string{}});
    return result;
}

std::size_t GraphCanvasModel::definitionUseCount(std::string_view definition) const {
    std::size_t uses = 0;
    for (const auto &node : nodes_) {
        if (!node.is_object()) continue;
        if (text(node, "op") == "graph.instance" && text(node, "definition") == definition)
            ++uses;
        if (isLoop(node))
            for (const auto &body : loopBranchBodies(node))
                if (body == definition) ++uses;
    }
    return uses;
}

bool GraphCanvasModel::enterLoopOrigin(std::string_view loop, std::int64_t element,
                                       std::string_view node) {
    const auto *found = findNode({}, loop);
    if (found == nullptr || !isLoop(*found)) return false;
    const auto bodies = loopBranchBodies(*found);
    const auto branch = branchForElement(loop, element);
    if (!branch || *branch >= bodies.size() || bodies[*branch].empty()) return false;
    if (findNode(bodies[*branch], node) == nullptr) return false;
    breadcrumb_ = {std::string{}};
    loop_branch_[std::string{loop}] = *branch;
    entered_loop_ = std::string{loop};
    breadcrumb_.push_back(bodies[*branch]);
    enteredScope();
    return true;
}

std::optional<std::size_t> GraphCanvasModel::branchForElement(std::string_view loop,
                                                              std::int64_t element) const {
    if (const auto *report = loopReport(loop)) {
        const auto elements = report->find("elements");
        if (elements != report->end() && elements->is_array())
            for (const auto &entry : *elements)
                // LP-4e: a draw_loop's element is a material's key, never
                // an integer; value() would throw on it.
                if (entry.is_object() && entry.contains("element") &&
                    entry.at("element").is_number_integer() &&
                    entry.at("element").get<std::int64_t>() == element &&
                    entry.contains("branch") && entry.at("branch").is_number_integer() &&
                    entry.at("branch").get<std::int64_t>() >= 0)
                    return static_cast<std::size_t>(entry.at("branch").get<std::int64_t>());
    }
    // No report: the conditions the table can read are integers, so they
    // can be asked here. An expression stops the walk -- guessing past it
    // would be naming a branch the engine may not run.
    const auto *node = findNode({}, loop);
    if (node == nullptr) return std::nullopt;
    const auto parameters = node->value("parameters", Json::object());
    const auto branches = parameters.value("branches", Json::array());
    for (std::size_t index = 0; index < branches.size(); ++index) {
        const auto read = readLoopBranch(branches[index]);
        if (read.when == "else") return index;
        if (read.when == "other" || read.when == "draw") return std::nullopt;
        const bool listed = std::ranges::find(read.values, element) != read.values.end();
        if ((read.when == "element" || read.when == "element_in") == listed) return index;
    }
    return std::nullopt;
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
    // LP-4c: a loop's body is no instance's; the watched instance's values
    // are about another body, whatever node ids the two share.
    if (!scope.empty() && !entered_loop_.empty()) return nullptr;
    // LP-4d1: nor is a body drawn in a zone of the root: the reply in hand is
    // about the scope on screen, whatever node ids the two share.
    if (scope != currentScope()) return nullptr;
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
    // LP-4e: parameter_dependent, but always the same one output.
    if (isDrawStep(op)) return std::string{kDrawStreamType};
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
        // LP-4c: a loop stands only in the root document, where its ports
        // are its body's boundary.
        drawn.loop = root && drawn.op == "graph.loop";
        // The body the ports are read from: the first branch that has one,
        // which is the boundary every branch has to share.
        std::string port_body;
        if (drawn.loop) {
            for (const auto &body : loopBranchBodies(node))
                if (!body.empty() && definitions_.contains(body)) {
                    port_body = body;
                    break;
                }
            drawn.definition = loopEntryBody(node);
            // No catalog entry names it (design §8): it is structure, as
            // the palette files it, not an operation the catalog missed.
            drawn.contract_known = true;
            drawn.domain = "structure";
            drawn.evaluation = "direct";
            drawn.badge = "↻ loop";
            if (const auto *report = loopReport(drawn.id)) {
                const auto tier = text(*report, "tier");
                const auto count = report->value("count", std::int64_t{0});
                if (!tier.empty())
                    drawn.badge = "↻ " + tier + " ×" + std::to_string(count);
            }
        }
        for (const auto &mark : refusal_marks_)
            if (mark.scope == scope && mark.node == drawn.id) {
                drawn.refused = true;
                drawn.refusal = mark.reason;
            }
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
        } else if (drawn.loop) {
            // LP-4c: the body's graph.input nodes, like an instance's. What
            // flows in is whatever the body reads it as -- a render resource
            // as often as a scalar -- so no type is claimed for it. The
            // parameters (over, branches, ...) are the Loop fields, not ports.
            if (!port_body.empty())
                for (auto &port : definitionInputs(definitions_.at(port_body)))
                    add_port(std::move(port), false);
        } else if (drawn.op == kMaterialPass || isDrawStep(drawn.op)) {
            // LP-4e: parameter_dependent, but the engine's contract is short
            // enough to restate: a material pass's sockets follow its
            // contract (its attachments, then depth, then the draws a
            // draw_loop hands it); a draw step follows the one on `after`.
            // The parameters are fields, never ports.
            const auto socket = [&](std::string name, bool required, std::string type) {
                GraphCanvasPort port;
                port.port = std::move(name);
                port.declared = true;
                port.required = required;
                port.type = std::move(type);
                ports.push_back(std::move(port));
            };
            if (drawn.op == kMaterialPass) {
                for (auto &name : materialPassColorSockets(drawn.parameters))
                    socket(std::move(name), true, {});
                socket("depth", true, {});
                socket("draws", false, std::string{kDrawStreamType});
            } else {
                socket("after", false, std::string{kDrawStreamType});
            }
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
                    if (const auto element = edge->find("element"); element != edge->end())
                        port.source_element = element->dump();
                }
            }
            const auto stored = drawn.loop ? drawn.parameters.end()
                                           : drawn.parameters.find(port.port);
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
        } else if (drawn.loop) {
            // LP-4c: what the loop declares, `p` (the last element's) and
            // `p[]` (one per element), each typed by the body's output `p`.
            const auto authored = node.find("outputs");
            if (authored != node.end() && authored->is_array())
                for (const auto &output : *authored) {
                    if (!output.is_string()) continue;
                    GraphCanvasOutput drawn_output;
                    drawn_output.port = output.get<std::string>();
                    drawn_output.bundle = drawn_output.port.ends_with("[]");
                    const auto base = drawn_output.bundle
                                          ? drawn_output.port.substr(0, drawn_output.port.size() - 2)
                                          : drawn_output.port;
                    if (!port_body.empty())
                        drawn_output.type = definitionOutputType(port_body, base, 8);
                    drawn.outputs.push_back(std::move(drawn_output));
                }
        } else if (isDrawStep(drawn.op)) {
            // LP-4e: a draw step hands the list on.
            drawn.outputs.push_back(GraphCanvasOutput{"draws", std::string{kDrawStreamType}});
        } else if (drawn.op == kMaterialPass) {
            // No output port: what it wrote is read through a target_ref.
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
            GraphCanvasEdge drawn{producer, text(edge, "port"), text(node, "id"), port};
            // LP-4d2, decision 4: which element of a `p[]` the edge reads, as
            // its label shows it -- an expression by its text, a literal as
            // written.
            if (const auto element = edge.find("element"); element != edge.end()) {
                if (element->is_object() && element->contains("$expr") &&
                    element->at("$expr").is_string())
                    drawn.element = element->at("$expr").get<std::string>();
                else if (element->is_string())
                    drawn.element = element->get<std::string>();
                else
                    drawn.element = element->dump();
            }
            result.push_back(std::move(drawn));
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

std::string GraphCanvasModel::sceneTimeNode(std::string_view scope) const {
    const auto *nodes = scopeNodes(scope);
    if (nodes == nullptr) return {};
    for (const auto &node : *nodes) {
        if (!node.is_object() || text(node, "op") != "value.input") continue;
        const auto parameters = node.find("parameters");
        if (parameters == node.end() || !parameters->is_object()) continue;
        if (text(*parameters, "name") == "scene_time") return text(node, "id");
    }
    return {};
}

std::string GraphCanvasModel::scopeToken() const {
    return scopeToken(currentScope());
}

std::string GraphCanvasModel::scopeToken(std::string_view scope) {
    // The engine spells the root document "root"; the canvas spells it with the
    // empty id so a definition named "root" is still a scope of its own.
    return scope.empty() ? std::string{"root"} : std::string{scope};
}

bool GraphCanvasModel::submitOps(Json ops) {
    if (!canEdit()) return false;
    if (!ops.is_array() || ops.empty()) return false;
    return submitHistory(HistoryAction::Edit, {ops, inverseOps(ops)});
}

Json GraphCanvasModel::inverseOps(const Json &ops) const {
    // LP-4c, decision 7. One list can add a node and point a later op at it,
    // or add a definition and then a node in it. Each op is therefore
    // inverted against the document as the ops before it left it: a copy of
    // the document is walked through the list, and every inverse is read off
    // the copy at that point. The inverses run in reverse order; whole nodes
    // and whole definition bodies are restored, so unknown fields and absent
    // values survive Undo.
    Json nodes = nodes_;
    Json definitions = definitions_.is_object() ? definitions_ : Json::object();
    const auto scopeNodesOf = [&](const std::string &scope) -> Json * {
        if (scope.empty()) return &nodes;
        const auto found = definitions.find(scope);
        if (found == definitions.end() || !found->is_object()) return nullptr;
        auto &body = *found;
        if (!body.contains("nodes") || !body.at("nodes").is_array())
            body["nodes"] = Json::array();
        return &body.at("nodes");
    };
    const auto locate = [](Json &scope_nodes, const std::string &id) {
        return std::ranges::find_if(scope_nodes, [&](const Json &value) {
            return value.is_object() && text(value, "id") == id;
        });
    };
    // A definition renamed in the list is still restored under the name the
    // document had before it, which is the name the placement is filed under.
    std::map<std::string, std::string> original_name;
    const auto originalScope = [&](const std::string &scope) {
        const auto found = original_name.find(scope);
        return found == original_name.end() ? scope : found->second;
    };
    std::vector<Json> inverses;
    std::set<std::string> layouts;
    for (const auto &op : ops) {
        const auto kind = op.at("op").get<std::string>();
        const auto scope = op.contains("scope") && op.at("scope").is_string()
                               ? op.at("scope").get<std::string>()
                               : std::string{"root"};
        const auto model_scope = scope == "root" ? std::string{} : scope;
        if (kind == "add_definition") {
            const auto name = op.at("name").get<std::string>();
            inverses.push_back(Json{{"op", "remove_definition"}, {"name", name}});
            auto body = op.value("definition", Json{});
            if (body.is_null()) body = Json{{"nodes", Json::array()}, {"outputs", Json::object()}};
            definitions[name] = std::move(body);
            original_name.erase(name);
            continue;
        }
        if (kind == "remove_definition") {
            const auto name = op.at("name").get<std::string>();
            const auto found = definitions.find(name);
            if (found == definitions.end())
                throw std::logic_error("Graph history definition is missing: " + name);
            // The body goes back whole, placement included: it travels inside.
            inverses.push_back(
                Json{{"op", "add_definition"}, {"name", name}, {"definition", *found}});
            definitions.erase(found);
            continue;
        }
        if (kind == "rename_definition") {
            const auto from = op.at("from").get<std::string>();
            const auto to = op.at("to").get<std::string>();
            const auto found = definitions.find(from);
            if (found == definitions.end())
                throw std::logic_error("Graph history definition is missing: " + from);
            inverses.push_back(Json{{"op", "rename_definition"}, {"from", to}, {"to", from}});
            auto body = std::move(*found);
            definitions.erase(from);
            definitions[to] = std::move(body);
            // The references follow the body in the same op, as the engine
            // moves them: an instance's definition and a loop's branches.
            for (auto &node : nodes) {
                if (!node.is_object()) continue;
                if (text(node, "op") == "graph.instance" && text(node, "definition") == from)
                    node["definition"] = to;
                if (isLoop(node) && node.contains("parameters") &&
                    node.at("parameters").is_object() &&
                    node.at("parameters").contains("branches") &&
                    node.at("parameters").at("branches").is_array())
                    for (auto &branch : node["parameters"]["branches"])
                        if (branch.is_object() && text(branch, "body") == from)
                            branch["body"] = to;
            }
            original_name[to] = originalScope(from);
            original_name.erase(from);
            continue;
        }
        if (kind == "set_layout") {
            layouts.insert(originalScope(model_scope));
            continue;
        }
        if (kind == "set_outputs") {
            // LP-4d2 (DO-1): a scope's outputs, replaced whole; its inverse is
            // the same op carrying what the scope held before. Only a
            // definition's are kept here -- the canvas never sends the
            // root's.
            if (model_scope.empty())
                throw std::logic_error("Graph history cannot invert the root's outputs");
            const auto found = definitions.find(model_scope);
            if (found == definitions.end() || !found->is_object())
                throw std::logic_error("Graph history scope is missing: " + scope);
            const auto previous = found->find("outputs");
            inverses.push_back(Json{{"op", "set_outputs"}, {"scope", scope},
                                    {"outputs", previous != found->end() && previous->is_object()
                                                    ? *previous
                                                    : Json::object()}});
            (*found)["outputs"] = op.at("outputs");
            continue;
        }
        auto *scope_nodes = scopeNodesOf(model_scope);
        if (scope_nodes == nullptr)
            throw std::logic_error("Graph history scope is missing: " + scope);
        if (kind == "add_node") {
            const auto &node = op.at("node");
            inverses.push_back(Json{{"scope", scope}, {"op", "remove_node"},
                                    {"node", node.at("id")}});
            const auto index = op.find("index");
            if (index != op.end() && index->is_number_integer() &&
                index->get<std::int64_t>() >= 0 &&
                index->get<std::size_t>() <= scope_nodes->size())
                scope_nodes->insert(scope_nodes->begin() + index->get<std::ptrdiff_t>(), node);
            else
                scope_nodes->push_back(node);
            continue;
        }
        // replace_node names its node by the node it carries.
        const auto id = kind == "replace_node" ? op.at("node").at("id").get<std::string>()
                                               : op.at("node").get<std::string>();
        const auto found = locate(*scope_nodes, id);
        if (found == scope_nodes->end())
            throw std::logic_error("Graph history node is missing: " + id);
        Json back{{"scope", scope}};
        back["op"] = kind == "remove_node" ? "add_node" : "replace_node";
        back["node"] = *found;
        if (kind == "remove_node")
            back["index"] = std::distance(scope_nodes->begin(), found);
        inverses.push_back(std::move(back));
        // Topology edits discard placements for removed nodes/edges in the
        // same transaction. Restore those only after their endpoints.
        if (kind == "remove_node" || kind == "set_input" || kind == "replace_node")
            layouts.insert(originalScope(model_scope));
        // And the copy moves on, so the next op reads what this one left.
        auto &node = *found;
        if (kind == "remove_node") {
            scope_nodes->erase(found);
        } else if (kind == "replace_node") {
            node = op.at("node");
        } else if (kind == "set_input") {
            const auto port = op.at("port").get<std::string>();
            const auto &source = op.at("source");
            if (source.is_null()) {
                if (node.contains("inputs") && node.at("inputs").is_object()) {
                    node["inputs"].erase(port);
                    if (node["inputs"].empty()) node.erase("inputs");
                }
            } else {
                node["inputs"][port] = source;
            }
        } else if (kind == "set_node_parameters") {
            const auto &parameters = op.at("parameters");
            if (parameters.empty()) node.erase("parameters");
            else node["parameters"] = parameters;
        } else if (kind == "set_node_label") {
            const auto &label = op.at("label");
            if (label.is_string() && label.get_ref<const std::string &>().empty())
                node.erase("label");
            else
                node["label"] = label;
        }
    }
    Json inverse = Json::array();
    for (auto it = inverses.rbegin(); it != inverses.rend(); ++it)
        inverse.push_back(std::move(*it));
    for (const auto &scope : layouts) {
        // A scope the list created has no placement to go back to: the
        // inverse removes it.
        if (!scope.empty() && !definitions_.contains(scope)) continue;
        // A scope with no placement goes back to having none: an empty one
        // would be a member the document did not have.
        const auto layout = scopeLayout(scope);
        inverse.push_back(Json{{"op", "set_layout"},
            {"scope", scope.empty() ? std::string{"root"} : scope},
            {"layout", layout.empty() ? Json{} : layout}});
    }
    return inverse;
}

bool GraphCanvasModel::submitHistory(HistoryAction action, HistoryEntry entry) {
    if (!canEdit()) return false;
    Json ops = action == HistoryAction::Undo ? entry.inverse : entry.forward;
    pending_history_ = PendingHistory{action, std::move(entry)};
    edit_notice_ = {InspectorNoticeKind::Information, "Saving the graph change..."};
    request(RequestKind::Submit, "edit_graph_document",
            {{"base_source_digest", digest_}, {"ops", std::move(ops)}});
    return true;
}

void GraphCanvasModel::clearHistory() {
    undo_.clear();
    redo_.clear();
    pending_history_.reset();
    history_digest_.clear();
}

void GraphCanvasModel::commitHistory(const Json &response) {
    const auto digest = response.value("source_digest", Json::object())
                            .value("hex", std::string{});
    if (!pending_history_ || digest.empty()) {
        clearHistory();
        return;
    }
    auto pending = std::move(*pending_history_);
    pending_history_.reset();
    if (pending.action == HistoryAction::Undo) {
        undo_.pop_back();
        redo_.push_back(std::move(pending.entry));
    } else {
        if (pending.action == HistoryAction::Redo) redo_.pop_back();
        else redo_.clear();
        undo_.push_back(std::move(pending.entry));
        if (undo_.size() > historyCapacity) undo_.erase(undo_.begin());
    }
    history_digest_ = digest;
}

bool GraphCanvasModel::undo() {
    return canUndo() && submitHistory(HistoryAction::Undo, undo_.back());
}

bool GraphCanvasModel::redo() {
    return canRedo() && submitHistory(HistoryAction::Redo, redo_.back());
}

Json GraphCanvasModel::nodeParameters(std::string_view id) const {
    return nodeParameters(currentScope(), id);
}

Json GraphCanvasModel::nodeParameters(std::string_view scope, std::string_view id) const {
    const auto *node = findNode(scope, id);
    if (node == nullptr) return Json::object();
    const auto found = node->find("parameters");
    if (found == node->end() || !found->is_object()) return Json::object();
    return *found;
}

bool GraphCanvasModel::canConnect(std::string_view from_node, std::string_view from_port,
                                  std::string_view to_node, std::string_view to_port) const {
    return canConnect(currentScope(), from_node, from_port, to_node, to_port);
}

bool GraphCanvasModel::canConnect(std::string_view scope, std::string_view from_node,
                                  std::string_view from_port, std::string_view to_node,
                                  std::string_view to_port) const {
    if (from_node.empty() || to_node.empty() || from_node == to_node) return false;
    const auto drawn = nodes(scope);
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
    return connectInput(currentScope(), node, port, source_node, source_port);
}

bool GraphCanvasModel::connectInput(std::string_view scope, const std::string &node,
                                    const std::string &port, const std::string &source_node,
                                    const std::string &source_port) {
    if (!canEdit()) return false;
    if (!canConnect(scope, source_node, source_port, node, port)) return false;
    return submitOps(Json::array({Json{
        {"op", "set_input"}, {"scope", scopeToken(scope)}, {"node", node}, {"port", port},
        {"source", Json{{"node", source_node}, {"port", source_port}}}}}));
}

bool GraphCanvasModel::disconnectInput(const std::string &node, const std::string &port) {
    return disconnectInput(currentScope(), node, port);
}

bool GraphCanvasModel::disconnectInput(std::string_view scope, const std::string &node,
                                       const std::string &port) {
    if (!canEdit() || findNode(scope, node) == nullptr) return false;
    return submitOps(Json::array({Json{
        {"op", "set_input"}, {"scope", scopeToken(scope)}, {"node", node}, {"port", port},
        {"source", nullptr}}}));
}

bool GraphCanvasModel::setNodeParameter(const std::string &node, const std::string &key,
                                        const Json &value) {
    return setNodeParameter(currentScope(), node, key, value);
}

bool GraphCanvasModel::setNodeParameter(std::string_view scope, const std::string &node,
                                        const std::string &key, const Json &value) {
    if (!canEdit() || findNode(scope, node) == nullptr) return false;
    if (key.empty()) return false;
    // set_node_parameters replaces the object, so the whole of it is read back
    // and sent again with this one key changed.
    auto parameters = nodeParameters(scope, node);
    parameters[key] = value;
    return submitOps(Json::array({Json{
        {"op", "set_node_parameters"}, {"scope", scopeToken(scope)}, {"node", node},
        {"parameters", std::move(parameters)}}}));
}

bool GraphCanvasModel::removeNode(const std::string &node) {
    return removeNode(currentScope(), node);
}

bool GraphCanvasModel::removeNode(std::string_view scope, const std::string &node) {
    if (!canEdit() || findNode(scope, node) == nullptr) return false;
    return submitOps(Json::array({Json{
        {"op", "remove_node"}, {"scope", scopeToken(scope)}, {"node", node}}}));
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
    return setNodeLabel(currentScope(), node, label);
}

bool GraphCanvasModel::setNodeLabel(std::string_view scope, const std::string &node,
                                    const std::string &label) {
    if (!canEdit() || findNode(scope, node) == nullptr) return false;
    return submitOps(Json::array({Json{
        {"op", "set_node_label"}, {"scope", scopeToken(scope)}, {"node", node},
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
    return suggestNodeId(currentScope(), op);
}

std::string GraphCanvasModel::suggestNodeId(std::string_view scope, std::string_view op) const {
    std::string base{op};
    // The op name is the honest starting point, but a node id is an identifier
    // the document points at: the dots go.
    std::ranges::replace(base, '.', '_');
    if (base.empty()) base = "node";
    const auto taken = [&](const std::string &candidate) {
        return findNode(scope, candidate) != nullptr;
    };
    if (!taken(base)) return base;
    for (int suffix = 2; suffix < 10000; ++suffix) {
        auto candidate = base + "_" + std::to_string(suffix);
        if (!taken(candidate)) return candidate;
    }
    return base;
}

Json GraphCanvasModel::addNodeOps(const std::string &op, const std::string &definition) const {
    return addNodeOpsIn(currentScope(), op, definition);
}

Json GraphCanvasModel::addNodeOpsIn(std::string_view in_scope, const std::string &op,
                                    const std::string &definition) const {
    Json ops = Json::array();
    if (op.empty()) return ops;
    const auto scope = scopeToken(in_scope);
    const bool root = in_scope.empty();
    if (!root && !definitions_.contains(std::string{in_scope})) return ops;
    const auto id = suggestNodeId(in_scope, op);
    Json node{{"id", id}, {"op", op}};
    if (op == "graph.input") {
        if (root) return Json::array();
        // A boundary port every instance can already stand on: without a value
        // to fall back to, the instances that exist stop expanding.
        node["default"] = 0.0;
        ops.push_back(Json{{"op", "add_node"}, {"scope", scope}, {"node", std::move(node)}});
        return ops;
    }
    // LP-4c: a loop over an existing body. A new body is addLoopOps's second
    // argument, which the palette passes itself.
    if (op == "graph.loop") return root ? addLoopOps(definition) : Json::array();
    if (op == "graph.instance") {
        if (!root || definition.empty() || !definitions_.contains(definition))
            return Json::array();
        node["definition"] = definition;
        node["parameters"] = definitionDefaults(definition);
        ops.push_back(Json{{"op", "add_node"}, {"scope", scope}, {"node", std::move(node)}});
        return ops;
    }
    if (op == kFullscreenTemplate) {
        // LP-4c. A fullscreen pass is not a name alone: its ports, its shader
        // and the family it runs in are its parameters. It arrives drawing
        // with the engine's passthrough into an image of its own -- the
        // smallest pass that is a whole document -- and the image is added
        // in the same list, as a required input's constant is.
        if (operation(op) == nullptr) return Json::array();
        const Json type{{"semantic", "pelican.render.color_signal@1"},
                        {"arguments", Json{{"reference", "scene"}, {"transfer", "linear"},
                                           {"range", "extended"}}}};
        const auto image = id + "__color";
        ops.push_back(Json{{"op", "add_node"}, {"scope", scope},
            {"node", Json{{"id", image}, {"op", "render.image"},
                          {"parameters", Json{{"type", type}, {"format", "R8G8B8A8_UNORM"},
                                              {"extent_scale", 1.0},
                                              {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED",
                                                                     "TRANSFER_SRC"})}}},
                          {"outputs", Json::array({"resource"})}}}});
        // RU-1 (LP-1e): no pass_set. A graph pass that names none joins the
        // first family of the composed frame, which is where the scene is
        // drawn; naming main_render put it nowhere in a pipeline without one.
        node["parameters"] = Json{
            {"shader", Json{{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}},
            {"ports", Json{{"color", Json{{"kind", "image"}, {"type", type}, {"access", "write"},
                                          {"attachment", "color"}, {"location", 0}}}}}};
        node["inputs"] = Json{{"color", Json{{"node", image}, {"port", "resource"}}}};
        node["outputs"] = Json::array({"color"});
        ops.push_back(Json{{"op", "add_node"}, {"scope", scope}, {"node", std::move(node)}});
        // RU-1 (LP-1e): the loop running this body is not given a place. A
        // loop with none expands after the scene passes, before the first
        // anchor, which is where post_main used to put it; the Loop fields'
        // Place still moves it.
        return ops;
    }
    if (op == kMaterialPass) {
        // LP-4e. A material pass is a contract and its attachments: it
        // arrives with the contract the palette's combo chose (the
        // `definition` argument), writing images of its own -- one per
        // socket, and a depth -- added in the same list, as a fullscreen
        // pass's is. Its draws are left unconnected: today's drawing.
        if (!root || operation(op) == nullptr) return Json::array();
        const auto contract = definition.empty() ? materialPassContracts().front() : definition;
        if (std::ranges::find(materialPassContracts(), contract) == materialPassContracts().end())
            return Json::array();
        Json parameters{{"material_contract", contract}, {"pass_set", "main_render"}};
        const Json color_type{{"semantic", "pelican.render.color_signal@1"},
                              {"arguments", Json{{"reference", "scene"}, {"transfer", "linear"},
                                                 {"range", "extended"}}}};
        const auto image = [&](const std::string &socket, const Json &type,
                               const char *format, const char *attachment_usage) {
            const auto image_id = id + "__" + socket;
            ops.push_back(Json{{"op", "add_node"}, {"scope", scope},
                {"node", Json{{"id", image_id}, {"op", "render.image"},
                              {"parameters", Json{{"type", type}, {"format", format},
                                                  {"extent_scale", 1.0},
                                                  {"usage", Json::array({attachment_usage,
                                                                         "SAMPLED"})}}},
                              {"outputs", Json::array({"resource"})}}}});
            return Json{{"node", image_id}, {"port", "resource"}};
        };
        Json inputs = Json::object();
        for (const auto &socket : materialPassColorSockets(parameters)) {
            // What the G-buffer's lighting reads back as data keeps its
            // precision; a colour is eight bits a channel.
            const bool wide = socket == "color" || socket == "normal" || socket == "worldpos";
            inputs[socket] = image(socket, color_type,
                                   wide ? "R16G16B16A16_SFLOAT" : "R8G8B8A8_UNORM",
                                   "COLOR_ATTACHMENT");
        }
        inputs["depth"] = image("depth",
                                Json{{"semantic", "pelican.render.depth@1"},
                                     {"arguments", Json{{"representation", "device"},
                                                        {"space", "projection"}}}},
                                "D32_SFLOAT", "DEPTH_STENCIL_ATTACHMENT");
        node["parameters"] = std::move(parameters);
        node["inputs"] = std::move(inputs);
        node["outputs"] = Json::array();
        ops.push_back(Json{{"op", "add_node"}, {"scope", scope}, {"node", std::move(node)}});
        return ops;
    }
    if (isDrawStep(op)) {
        // LP-4e, decision 4. A draw step stands in a draw_loop body only. It
        // arrives at the head of the body's list -- drawn first, the step
        // that makes `draws` still last -- so the body keeps one list.
        if (root || !drawBodyScope(in_scope) || operation(op) == nullptr)
            return Json::array();
        Json parameters = Json::object();
        // Something to draw differently, so the step is one the engine
        // takes: the proposed variant, or both faces.
        if (op == "draw.variant") parameters["variant"] = suggestDrawVariant(in_scope);
        if (op == "draw.override") parameters["render_state"] = Json{{"cull", "none"}};
        node["parameters"] = std::move(parameters);
        node["outputs"] = Json::array({"draws"});
        ops.push_back(Json{{"op", "add_node"}, {"scope", scope}, {"node", node}});
        // The head: from the step `draws` names, back along `after`.
        const auto &body = definitions_.at(std::string{in_scope});
        const auto outputs = body.value("outputs", Json::object());
        const auto draws = outputs.is_object() ? outputs.find("draws") : outputs.end();
        std::string head = draws != outputs.end() && draws->is_object() ? text(*draws, "node") : "";
        std::set<std::string> seen;
        while (!head.empty() && seen.insert(head).second) {
            const auto *step = findNode(in_scope, head);
            if (step == nullptr) break;
            const auto after = step->value("inputs", Json::object()).value("after", Json{});
            if (!after.is_object() || text(after, "node").empty()) break;
            head = text(after, "node");
        }
        const auto *head_node = head.empty() ? nullptr : findNode(in_scope, head);
        if (head_node != nullptr && isDrawStep(text(*head_node, "op")))
            ops.push_back(Json{{"op", "set_input"}, {"scope", scope}, {"node", head},
                               {"port", "after"},
                               {"source", Json{{"node", id}, {"port", "draws"}}}});
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
        // Q-11. A voice reads its sound along `time`; a constant there would
        // hold it on its first sample. It arrives on the scene clock instead
        // -- the scope's own scene_time when it has one, a new one otherwise
        // -- and so starts on its owner's first frame (AU-3). The clock and
        // the node go out as one edit, so one Undo takes both back.
        if (isAudioPlayback(op) && port == "time") {
            auto clock = sceneTimeNode(in_scope);
            if (clock.empty()) {
                clock = suggestNodeId(in_scope, "scene_time");
                ops.push_back(Json{{"op", "add_node"}, {"scope", scope},
                    {"node", Json{{"id", clock}, {"op", "value.input"},
                                  {"parameters", Json{{"name", "scene_time"}}},
                                  {"outputs", Json::array({"value"})}}}});
            }
            inputs[port] = Json{{"node", clock}, {"port", "value"}};
            continue;
        }
        const auto seed = id + "__" + port;
        ops.push_back(Json{{"op", "add_node"}, {"scope", scope},
            {"node", Json{{"id", seed}, {"op", "value.constant"},
                          {"parameters", Json{{"value", 0.0}}},
                          {"outputs", Json::array({"value"})}}}});
        inputs[port] = Json{{"node", seed}, {"port", "value"}};
    }
    if (!inputs.empty()) node["inputs"] = std::move(inputs);
    // Q-8. A ui.* sink without a node path is not a document the engine
    // loads either ("ui sink requires a node path"). It arrives writing the
    // first node the project's UI document offers for it; the details panel
    // lists the rest. With no UI document read, the key stays out and the
    // engine's refusal names what is missing.
    if (const auto key = uiNodeParameter(op); !key.empty()) {
        const auto choices = uiNodeChoices(op);
        if (!choices.empty())
            node["parameters"] = Json{{std::string{key}, choices.front()}};
    }
    // Q-10. audio.playback has no port for the sound it plays, and without
    // one the engine cannot lower it. It arrives on the project's first
    // sound; the `asset` field lists the rest. With none found, the key stays
    // out and the engine's refusal names what is missing. Its optional ports
    // (events, position, the time map) are left for the user to connect.
    if (isAudioPlayback(op) && !audio_assets_.empty())
        node["parameters"] = Json{{"asset", audio_assets_.front()}};
    ops.push_back(Json{{"op", "add_node"}, {"scope", scope}, {"node", std::move(node)}});
    return ops;
}

bool GraphCanvasModel::addNode(const std::string &op, const std::string &definition) {
    return addNodeIn(currentScope(), op, definition);
}

bool GraphCanvasModel::addNodeIn(std::string_view scope, const std::string &op,
                                 const std::string &definition) {
    if (!canEdit()) return false;
    auto ops = addNodeOpsIn(scope, op, definition);
    if (ops.empty()) return false;
    return submitOps(std::move(ops));
}

// --- LP-4c: graph.loop.

Json GraphCanvasModel::loopParameters(std::string_view id) const {
    const auto *node = findNode({}, id);
    if (node == nullptr || !isLoop(*node)) return Json::object();
    const auto found = node->find("parameters");
    if (found == node->end() || !found->is_object()) return Json::object();
    return *found;
}

bool GraphCanvasModel::setLoopParameters(const std::string &id, Json parameters) {
    if (!canEdit()) return false;
    const auto *node = findNode({}, id);
    if (node == nullptr || !isLoop(*node)) return false;
    if (node->value("parameters", Json::object()) == parameters) return false;
    // Loops stand in the root document only, so the op names the root
    // whatever scope the canvas is showing.
    return submitOps(Json::array({Json{
        {"op", "set_node_parameters"}, {"scope", "root"}, {"node", id},
        {"parameters", std::move(parameters)}}}));
}

std::optional<GraphCanvasLoop> GraphCanvasModel::loop(std::string_view id) const {
    const auto *node = findNode({}, id);
    if (node == nullptr || !isLoop(*node)) return std::nullopt;
    GraphCanvasLoop loop;
    loop.id = std::string{id};
    const auto parameters = loopParameters(id);
    const auto over = parameters.value("over", Json::object());
    // LP-4e: over the draws of one material pass.
    if (over.is_object())
        if (const auto draw_items = over.find("draw_items");
            draw_items != over.end() && draw_items->is_object()) {
            loop.draw_items = true;
            loop.contract = text(*draw_items, "contract");
        }
    const auto range = over.is_object() ? over.value("range", Json::object()) : Json::object();
    const auto count = range.is_object() ? range.find("count") : range.end();
    if (range.is_object() && count != range.end()) {
        if (count->is_number_integer()) {
            loop.count = count->get<std::int64_t>();
            loop.known_count = loop.count;
        } else if (count->is_object() && count->contains("$expr")) {
            loop.count_expression = true;
            loop.count_text = text(*count, "$expr");
        }
    }
    // LP-4f1: over one view axis; its length is the frame plan's.
    if (over.is_object())
        if (const auto view_axis = over.find("view_axis");
            view_axis != over.end() && view_axis->is_object()) {
            loop.view_family = text(*view_axis, "family");
            loop.view_axis = text(*view_axis, "axis");
            if (const auto *axes = familyAxes(loop.view_family))
                if (const auto axis = std::ranges::find(*axes, loop.view_axis, &FramePlanViewAxis::axis);
                    axis != axes->end()) {
                    loop.view_axis_info = *axis;
                    if (axis->length && !axis->runtime)
                        loop.known_count = static_cast<std::int64_t>(*axis->length);
                }
        }
    const auto order = parameters.find("order");
    loop.descending = order != parameters.end() && order->is_object() &&
                      text(*order, "direction") == "descending";
    const auto limit = parameters.find("limit");
    if (limit != parameters.end() && limit->is_number_integer())
        loop.limit = limit->get<std::int64_t>();
    loop.tier = text(parameters, "tier");
    const auto where = parameters.find("where");
    if (loop.draw_items && where != parameters.end() && where->is_object()) {
        if (const auto tags = where->find("tags"); tags != where->end())
            if (auto filter = readTagFilter(*tags)) loop.where_tags = std::move(*filter);
    } else if (where != parameters.end() && where->is_object() && where->size() == 1) {
        const auto first = where->begin();
        if (first.key() == "element_in" || first.key() == "element_not_in")
            if (auto values = integerList(first.value())) {
                loop.where = first.key();
                loop.where_values = std::move(*values);
            }
    }
    loop.anchor = text(parameters, "canonical_anchor");
    if (const auto insert = parameters.find("insert");
        insert != parameters.end() && insert->is_object() && insert->size() == 1)
        loop.insert = insert->begin().key() + " " +
                      (insert->begin()->is_string() ? insert->begin()->get<std::string>()
                                                    : insert->begin()->dump());
    for (const auto &branch : parameters.value("branches", Json::array()))
        loop.branches.push_back(readLoopBranch(branch));
    const auto inputs = node->value("inputs", Json::object());
    for (const auto &pair : parameters.value("carry", Json::array())) {
        if (!pair.is_object()) continue;
        GraphCanvasLoopCarry carry{text(pair, "from"), text(pair, "to"), false};
        carry.to_unbound = !inputs.is_object() || !inputs.contains(carry.to);
        loop.carry.push_back(std::move(carry));
    }
    for (const auto &output : node->value("outputs", Json::array()))
        if (output.is_string()) loop.outputs.push_back(output.get<std::string>());
    for (const auto &branch : loop.branches)
        if (!branch.body.empty() && definitions_.contains(branch.body)) {
            loop.body = branch.body;
            loop.body_inputs = definitionInputs(definitions_.at(branch.body));
            loop.body_outputs = definitionOutputs(definitions_.at(branch.body));
            break;
        }
    for (const auto &branch : loop.branches) {
        const auto *nodes = branch.body.empty() ? nullptr : scopeNodes(branch.body);
        if (nodes == nullptr) continue;
        for (const auto &body_node : *nodes) {
            const auto op = text(body_node, "op");
            loop.body_draws = loop.body_draws || op == "render.fullscreen" || op == "render.raster";
        }
    }
    // LP-4e: the material pass the loop's draws feed.
    for (const auto &candidate : nodes_) {
        if (!candidate.is_object() || text(candidate, "op") != kMaterialPass) continue;
        const auto draws = candidate.value("inputs", Json::object()).value("draws", Json{});
        if (draws.is_object() && text(draws, "node") == id) {
            loop.pass = text(candidate, "id");
            break;
        }
    }
    if (const auto *report = loopReport(id)) {
        loop.reported = true;
        loop.reported_tier = text(*report, "tier");
        loop.reported_count = report->value("count", std::int64_t{0});
        if (loop.pass.empty()) loop.pass = text(*report, "pass");
        const auto elements = report->value("elements", Json::array());
        for (const auto &element : elements) {
            if (!element.is_object()) continue;
            const auto found = element.find("element");
            if (found == element.end()) continue;
            // A draw_loop's element is a material's matching key.
            if (found->is_string()) loop.materials.push_back(found->get<std::string>());
            const auto branch = element.value("branch", std::int64_t{-1});
            if (branch < 0 || static_cast<std::size_t>(branch) >= loop.branches.size()) continue;
            auto &taken = loop.branches[static_cast<std::size_t>(branch)];
            if (found->is_string())
                taken.matched_materials.push_back(found->get<std::string>());
            else if (found->is_number_integer())
                taken.matched.push_back(found->get<std::int64_t>());
        }
        loop.reported_total = report->contains("total") && report->at("total").is_number_unsigned()
                                  ? report->at("total").get<std::size_t>()
                                  : elements.size();
        for (const auto &branch : report->value("unmatched_branches", Json::array()))
            if (branch.is_number_integer() && branch.get<std::int64_t>() >= 0 &&
                static_cast<std::size_t>(branch.get<std::int64_t>()) < loop.branches.size())
                loop.branches[static_cast<std::size_t>(branch.get<std::int64_t>())].unmatched = true;
    }
    return loop;
}

std::string GraphCanvasModel::suggestDefinitionName(std::string_view base) const {
    std::string name{base.empty() ? std::string_view{"loop_body"} : base};
    std::ranges::replace(name, '.', '_');
    std::ranges::replace(name, '/', '_');
    const auto taken = [&](const std::string &candidate) {
        return candidate == "root" || definitions_.contains(candidate);
    };
    if (!taken(name)) return name;
    for (int suffix = 2; suffix < 10000; ++suffix) {
        auto candidate = name + "_" + std::to_string(suffix);
        if (!taken(candidate)) return candidate;
    }
    return name;
}

Json GraphCanvasModel::addLoopOps(const std::string &body,
                                  const std::string &new_definition) const {
    Json ops = Json::array();
    // A loop stands in the root document only (graph.loop_nested@1).
    if (!currentScope().empty()) return ops;
    std::string runs = body;
    auto outputs = Json::array();
    if (!new_definition.empty()) {
        if (new_definition == "root" || new_definition.find('/') != std::string::npos ||
            definitions_.contains(new_definition))
            return Json::array();
        ops.push_back(Json{{"op", "add_definition"}, {"name", new_definition}});
        runs = new_definition;
    } else {
        if (body.empty() || !definitions_.contains(body)) return Json::array();
        for (const auto &output : definitionOutputs(definitions_.at(body))) outputs.push_back(output);
    }
    Json node{{"id", suggestNodeId("loop")},
              {"op", "graph.loop"},
              {"parameters",
               Json{{"over", Json{{"range", Json{{"count", 1}}}}},
                    {"branches", Json::array({Json{{"body", runs}}})}}},
              {"outputs", std::move(outputs)}};
    ops.push_back(Json{{"op", "add_node"}, {"scope", "root"}, {"node", std::move(node)}});
    return ops;
}

bool GraphCanvasModel::addLoop(const std::string &body, const std::string &new_definition) {
    if (!canEdit()) return false;
    auto ops = addLoopOps(body, new_definition);
    if (ops.empty()) return false;
    return submitOps(std::move(ops));
}

bool GraphCanvasModel::setLoopCount(const std::string &id, std::int64_t count) {
    auto parameters = loopParameters(id);
    if (count < 0) return false;
    parameters["over"] = Json{{"range", Json{{"count", count}}}};
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::setLoopCountExpression(const std::string &id,
                                              const std::string &expression) {
    if (expression.empty()) return false;
    auto parameters = loopParameters(id);
    parameters["over"] = Json{{"range", Json{{"count", Json{{"$expr", expression}}}}}};
    return setLoopParameters(id, std::move(parameters));
}

// --- LP-4f1: view axes.

namespace {

// `view_axes` as get_frame_plan writes it, read leniently: an entry the
// field cannot use is skipped rather than failing the whole plan (the
// Frame Plan tab is where a malformed plan is reported).
std::vector<FramePlanViewAxis> planViewAxes(const Json &node) {
    std::vector<FramePlanViewAxis> axes;
    const auto found = node.find("view_axes");
    if (found == node.end() || !found->is_array()) return axes;
    for (const auto &entry : *found) {
        if (!entry.is_object()) continue;
        FramePlanViewAxis axis;
        axis.axis = text(entry, "axis");
        if (axis.axis.empty()) continue;
        if (const auto length = entry.find("length");
            length != entry.end() && length->is_number_unsigned())
            axis.length = length->get<std::uint64_t>();
        axis.runtime = entry.value("runtime", false);
        if (const auto labels = entry.find("labels"); labels != entry.end() && labels->is_array())
            for (const auto &label : *labels)
                if (label.is_string()) axis.labels.push_back(label.get<std::string>());
        axes.push_back(std::move(axis));
    }
    return axes;
}

bool isElementExpression(const Json &value) {
    return value.is_object() && value.size() == 1 && text(value, "$expr") == "element";
}

} // namespace

void GraphCanvasModel::readPlanViews(const Json &plan) {
    view_families_.clear();
    plan_node_views_.clear();
    plan_targets_.clear();
    if (const auto nodes = plan.find("nodes"); nodes != plan.end() && nodes->is_array())
        for (const auto &node : *nodes) {
            if (!node.is_object()) continue;
            const auto family = text(node, "view_family");
            auto axes = planViewAxes(node);
            if (family.empty() || axes.empty()) continue;
            if (const auto name = text(node, "name"); !name.empty())
                plan_node_views_[name] = GraphCanvasViewFamily{family, axes};
            // The axes are the family's, the same on every node of it
            // (LP-Va2): the first node that lists them names them.
            if (std::ranges::none_of(view_families_, [&](const auto &known) {
                    return known.family == family;
                }))
                view_families_.push_back(GraphCanvasViewFamily{family, std::move(axes)});
        }
    if (const auto resources = plan.find("resources");
        resources != plan.end() && resources->is_array())
        for (const auto &resource : *resources)
            if (resource.is_object() && text(resource, "kind") == "render_target" &&
                !text(resource, "name").empty())
                plan_targets_.push_back(text(resource, "name"));
    std::ranges::sort(plan_targets_);
    plan_targets_.erase(std::unique(plan_targets_.begin(), plan_targets_.end()),
                        plan_targets_.end());
}

const std::vector<FramePlanViewAxis> *GraphCanvasModel::familyAxes(std::string_view family) const {
    const auto found = std::ranges::find(view_families_, family, &GraphCanvasViewFamily::family);
    return found == view_families_.end() ? nullptr : &found->axes;
}

bool GraphCanvasModel::takesViewSelect(std::string_view op) noexcept {
    return op == "render.fullscreen" || op == "render.raster" || op == "render.compute" ||
           op == "render.material_pass" || op == "render.shadow_depth" ||
           op == "render.snapshot_copy";
}

bool GraphCanvasModel::setLoopViewAxis(const std::string &id, const std::string &family,
                                       const std::string &axis) {
    if (family.empty() || axis.empty()) return false;
    auto parameters = loopParameters(id);
    parameters["over"] = Json{{"view_axis", Json{{"family", family}, {"axis", axis}}}};
    return setLoopParameters(id, std::move(parameters));
}

std::optional<GraphCanvasViewField> GraphCanvasModel::viewField(std::string_view scope,
                                                                std::string_view id) const {
    const auto *node = findNode(scope, id);
    if (node == nullptr || !takesViewSelect(text(*node, "op"))) return std::nullopt;
    const auto parameters = node->value("parameters", Json::object());
    const auto own = parameters.is_object() ? text(parameters, "view_family") : std::string{};
    GraphCanvasViewField field;
    const auto select = parameters.is_object() ? parameters.value("view_select", Json::object())
                                               : Json::object();
    if (select.is_object())
        for (const auto &[axis, value] : select.items()) {
            if (value.is_number_unsigned())
                field.view_select.push_back(
                    FramePlanViewSelectAxis{axis, value.get<std::uint64_t>()});
            else if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
                field.view_select.push_back(FramePlanViewSelectAxis{
                    axis, static_cast<std::uint64_t>(value.get<std::int64_t>())});
        }
    // In a body a loop over a view axis runs: that loop's family, and
    // "Each element" on its axis -- unless the node names another family.
    if (!scope.empty())
        for (const auto &candidate : nodes_) {
            if (!isLoop(candidate)) continue;
            const auto bodies = loopBranchBodies(candidate);
            if (std::ranges::find(bodies, scope) == bodies.end()) continue;
            const auto loop_parameters = candidate.value("parameters", Json::object());
            const auto over = loop_parameters.is_object()
                                  ? loop_parameters.value("over", Json::object())
                                  : Json::object();
            const auto view_axis = over.is_object() ? over.value("view_axis", Json::object())
                                                    : Json::object();
            const auto family = view_axis.is_object() ? text(view_axis, "family") : std::string{};
            if (family.empty() || (!own.empty() && own != family)) continue;
            field.family = family;
            field.loop = text(candidate, "id");
            field.element_axis = text(view_axis, "axis");
            break;
        }
    if (field.family.empty() && scope.empty())
        if (const auto planned = plan_node_views_.find(id); planned != plan_node_views_.end()) {
            field.family = planned->second.family;
            field.axes = planned->second.axes;
        }
    if (field.family.empty()) field.family = own;
    if (field.family.empty()) return std::nullopt;
    if (field.axes.empty())
        if (const auto *axes = familyAxes(field.family)) field.axes = *axes;
    if (field.axes.empty()) return std::nullopt;
    field.writes_family = own.empty();
    if (!field.element_axis.empty() && select.is_object())
        if (const auto chosen = select.find(field.element_axis); chosen != select.end())
            field.element_chosen = isElementExpression(*chosen);
    return field;
}

bool GraphCanvasModel::setViewSelect(std::string_view scope, const std::string &node,
                                     const Json &view_select, const std::string &family) {
    if (!canEdit() || findNode(scope, node) == nullptr) return false;
    if (!view_select.is_object()) return false;
    auto parameters = nodeParameters(scope, node);
    const auto before = parameters;
    if (view_select.empty()) {
        parameters.erase("view_select");
    } else {
        parameters["view_select"] = view_select;
        if (!family.empty() && text(parameters, "view_family").empty())
            parameters["view_family"] = family;
    }
    if (parameters == before) return false;
    return submitOps(Json::array({Json{
        {"op", "set_node_parameters"}, {"scope", scopeToken(scope)}, {"node", node},
        {"parameters", std::move(parameters)}}}));
}

bool GraphCanvasModel::setLoopDescending(const std::string &id, bool descending) {
    auto parameters = loopParameters(id);
    // Ascending is what an absent order means, so it is written by leaving
    // the member out rather than by spelling the default.
    if (descending)
        parameters["order"] = Json{{"by", "element"}, {"direction", "descending"}};
    else
        parameters.erase("order");
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::setLoopLimit(const std::string &id, std::optional<std::int64_t> limit) {
    auto parameters = loopParameters(id);
    if (limit) {
        if (*limit < 0) return false;
        parameters["limit"] = *limit;
    } else {
        parameters.erase("limit");
    }
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::setLoopTier(const std::string &id, const std::string &tier) {
    auto parameters = loopParameters(id);
    if (tier.empty()) parameters.erase("tier");
    else if (tier == "expand" || tier == "draw_loop") parameters["tier"] = tier;
    else return false;
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::setLoopWhere(const std::string &id, const std::string &kind,
                                    const std::vector<std::int64_t> &values) {
    auto parameters = loopParameters(id);
    if (kind.empty()) parameters.erase("where");
    else if (kind == "element_in" || kind == "element_not_in")
        parameters["where"] = Json{{kind, values}};
    else return false;
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::setLoopCarry(
    const std::string &id, const std::vector<std::pair<std::string, std::string>> &carry) {
    auto parameters = loopParameters(id);
    if (carry.empty()) {
        parameters.erase("carry");
    } else {
        auto pairs = Json::array();
        for (const auto &[from, to] : carry) {
            if (from.empty() || to.empty()) return false;
            pairs.push_back(Json{{"from", from}, {"to", to}});
        }
        parameters["carry"] = std::move(pairs);
    }
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::setLoopOutputs(const std::string &id,
                                      const std::vector<std::string> &outputs) {
    if (!canEdit()) return false;
    const auto *node = findNode({}, id);
    if (node == nullptr || !isLoop(*node)) return false;
    auto replaced = *node;
    replaced["outputs"] = outputs;
    if (replaced == *node) return false;
    // No op sets a node's outputs on its own: the node goes back whole, which
    // is also exactly what its Undo sends.
    return submitOps(Json::array({Json{
        {"op", "replace_node"}, {"scope", "root"}, {"node", std::move(replaced)}}}));
}

const std::vector<std::string> &GraphCanvasModel::loopAnchors() {
    // The engine's canonical anchors (featurecompose.cpp), in frame order.
    static const std::vector<std::string> anchors{
        "sprite", "post_main", "tonemap", "post_ldr",
        "pelican_ui", "debug_draw", "debug_text", "imgui"};
    return anchors;
}

bool GraphCanvasModel::setLoopAnchor(const std::string &id, const std::string &anchor) {
    auto parameters = loopParameters(id);
    if (!anchor.empty() && std::ranges::find(loopAnchors(), anchor) == loopAnchors().end())
        return false;
    // One place at a time: an anchor and an insert exclude each other.
    parameters.erase("insert");
    if (anchor.empty()) parameters.erase("canonical_anchor");
    else parameters["canonical_anchor"] = anchor;
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::addLoopBranch(const std::string &id) {
    auto parameters = loopParameters(id);
    auto branches = parameters.value("branches", Json::array());
    if (!branches.is_array() || branches.empty()) return false;
    // A new branch goes in front of the else and runs the else's body, so
    // the list it arrives in is one the engine takes: the same boundary, and
    // a condition (element 0) that is an integer the table can edit.
    const auto &last = branches.back();
    Json added{{"when", Json{{"element", 0}}}};
    // LP-4e: a draw_loop's elements are materials. The new branch catches
    // the first material the report names; before a report, the materials
    // that have a variant -- a condition the engine takes as it is.
    if (const auto *node = findNode({}, id); node != nullptr && isDrawItemsLoop(*node)) {
        const auto materials = loopMaterials(id);
        added["when"] = materials.empty() ? Json{{"has_variant", true}}
                                          : Json{{"material", materials.front()}};
    }
    added["body"] = last.is_object() && last.contains("body") ? last.at("body") : Json{};
    branches.insert(branches.end() - 1, std::move(added));
    parameters["branches"] = std::move(branches);
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::removeLoopBranch(const std::string &id, std::size_t branch) {
    auto parameters = loopParameters(id);
    auto branches = parameters.value("branches", Json::array());
    // The else is the last row, and it is never removed.
    if (!branches.is_array() || branch + 1 >= branches.size()) return false;
    branches.erase(branches.begin() + static_cast<std::ptrdiff_t>(branch));
    parameters["branches"] = std::move(branches);
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::moveLoopBranch(const std::string &id, std::size_t branch, int delta) {
    auto parameters = loopParameters(id);
    auto branches = parameters.value("branches", Json::array());
    if (!branches.is_array() || branch + 1 >= branches.size() || delta == 0) return false;
    const auto target = static_cast<std::int64_t>(branch) + delta;
    // Only among the conditional rows: the else stays last.
    if (target < 0 || static_cast<std::size_t>(target) + 1 >= branches.size()) return false;
    std::swap(branches[branch], branches[static_cast<std::size_t>(target)]);
    parameters["branches"] = std::move(branches);
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::setLoopBranchWhen(const std::string &id, std::size_t branch,
                                         const std::string &when,
                                         const std::vector<std::int64_t> &values) {
    auto parameters = loopParameters(id);
    auto branches = parameters.value("branches", Json::array());
    if (!branches.is_array() || branch + 1 >= branches.size()) return false;
    auto &row = branches[branch];
    if (!row.is_object()) return false;
    if (when == "element") {
        if (values.size() != 1) return false;
        row["when"] = Json{{"element", values.front()}};
    } else if (when == "element_in" || when == "element_not_in") {
        row["when"] = Json{{when, values}};
    } else {
        return false;
    }
    parameters["branches"] = std::move(branches);
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::setLoopBranchBody(const std::string &id, std::size_t branch,
                                         const std::optional<std::string> &body) {
    auto parameters = loopParameters(id);
    auto branches = parameters.value("branches", Json::array());
    if (!branches.is_array() || branch >= branches.size() || !branches[branch].is_object())
        return false;
    if (body && !definitions_.contains(*body)) return false;
    branches[branch]["body"] = body ? Json(*body) : Json{};
    parameters["branches"] = std::move(branches);
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::newLoopBranchDefinition(const std::string &id, std::size_t branch,
                                               const std::string &name) {
    if (!canEdit()) return false;
    const auto shape = loop(id);
    if (!shape || branch >= shape->branches.size()) return false;
    if (name.empty() || name == "root" || name.find('/') != std::string::npos ||
        definitions_.contains(name))
        return false;
    // The loop's boundary, empty inside: the first body's graph.input nodes
    // as they are declared, and its outputs. An output a boundary port feeds
    // is kept feeding it; an output some other node makes cannot be written
    // without that node, so the body is copied whole instead -- a branch
    // whose boundary differs is refused (graph.loop_branch_interface@1).
    Json definition{{"nodes", Json::array()}, {"outputs", Json::object()}};
    if (!shape->body.empty()) {
        const auto &source = definitions_.at(shape->body);
        bool whole = false;
        for (const auto &node : source.value("nodes", Json::array()))
            if (node.is_object() && text(node, "op") == "graph.input")
                definition["nodes"].push_back(node);
        // items() borrows its container, so the object outlives the loop.
        const auto source_outputs = source.value("outputs", Json::object());
        for (const auto &[port, endpoint] : source_outputs.items()) {
            const auto *producer =
                endpoint.is_object() ? findNode(shape->body, text(endpoint, "node")) : nullptr;
            if (producer == nullptr || text(*producer, "op") != "graph.input") {
                whole = true;
                break;
            }
            definition["outputs"][port] = endpoint;
        }
        if (whole) {
            definition = source;
            definition.erase("layout");
        }
    }
    auto parameters = loopParameters(id);
    auto branches = parameters.value("branches", Json::array());
    if (!branches.is_array() || !branches[branch].is_object()) return false;
    branches[branch]["body"] = name;
    parameters["branches"] = std::move(branches);
    return submitOps(Json::array({
        Json{{"op", "add_definition"}, {"name", name}, {"definition", std::move(definition)}},
        Json{{"op", "set_node_parameters"}, {"scope", "root"}, {"node", id},
             {"parameters", std::move(parameters)}}}));
}

std::vector<std::string> GraphCanvasModel::loopBodyChoices(std::string_view id) const {
    std::vector<std::string> choices;
    const auto shape = loop(id);
    if (!shape) return choices;
    const auto sorted = [](std::vector<std::string> values) {
        std::ranges::sort(values);
        return values;
    };
    const auto inputs = sorted(shape->body_inputs);
    const auto outputs = sorted(shape->body_outputs);
    for (const auto &[name, definition] : definitions_.items()) {
        // With no body yet, any definition sets the boundary.
        if (!shape->body.empty() && (sorted(definitionInputs(definition)) != inputs ||
                                     sorted(definitionOutputs(definition)) != outputs))
            continue;
        choices.push_back(name);
    }
    return choices;
}

bool GraphCanvasModel::submitEdit(Json ops) { return submitOps(std::move(ops)); }

bool GraphCanvasModel::renameDefinition(const std::string &from, const std::string &to) {
    if (!canEdit() || !definitions_.contains(from) || to.empty() || from == to) return false;
    return submitOps(Json::array({Json{
        {"op", "rename_definition"}, {"from", from}, {"to", to}}}));
}

bool GraphCanvasModel::removeDefinition(const std::string &name) {
    if (!canEdit() || !definitions_.contains(name)) return false;
    return submitOps(Json::array({Json{{"op", "remove_definition"}, {"name", name}}}));
}

std::vector<std::pair<std::string, std::string>>
GraphCanvasModel::perIterationSources(std::string_view receiver) const {
    std::vector<std::pair<std::string, std::string>> sources;
    for (const auto &node : nodes_) {
        if (!isLoop(node)) continue;
        const auto id = text(node, "id");
        // A loop reading its own `p[]` would be a cycle.
        if (id == receiver) continue;
        for (const auto &output : node.value("outputs", Json::array()))
            if (output.is_string() && output.get<std::string>().ends_with("[]"))
                sources.emplace_back(id, output.get<std::string>());
    }
    return sources;
}

bool GraphCanvasModel::connectPerIteration(const std::string &node, const std::string &port,
                                           const std::string &source_node,
                                           const std::string &source_port,
                                           const std::string &expression) {
    if (!canEdit() || expression.empty()) return false;
    const auto *receiver = findNode({}, node);
    if (receiver == nullptr || !isLoop(*receiver)) return false;
    const auto sources = perIterationSources(node);
    if (std::ranges::find(sources, std::pair{source_node, source_port}) == sources.end())
        return false;
    // `element` in the expression is the receiving loop's element: each of
    // its elements reads that element of the other loop's `p[]`.
    return submitOps(Json::array({Json{
        {"op", "set_input"}, {"scope", "root"}, {"node", node}, {"port", port},
        {"source", Json{{"node", source_node}, {"port", source_port},
                        {"element", Json{{"$expr", expression}}}}}}}));
}

namespace {

// LP-4d2: the definitions a loop's branches run, each once, in branch order.
// A branch that runs nothing, or names a definition the document does not
// hold, adds none.
std::vector<std::string> distinctLoopBodies(const Json &loop, const Json &definitions) {
    std::vector<std::string> bodies;
    for (const auto &body : loopBranchBodies(loop))
        if (!body.empty() && definitions.contains(body) &&
            std::ranges::find(bodies, body) == bodies.end())
            bodies.push_back(body);
    return bodies;
}

// A name proposed from a port's: what the node id rule takes (no slash), and
// no `[]` -- the loop spells an output per element with it.
std::string loopPortBase(std::string_view base, std::string_view fallback) {
    std::string name{base};
    if (name.ends_with("[]")) name.resize(name.size() - 2);
    std::ranges::replace(name, '/', '_');
    std::ranges::replace(name, '.', '_');
    return name.empty() ? std::string{fallback} : name;
}

// Whether a set_outputs endpoint may name `port` of this node: a graph.input's
// `value`, an instance's or a loop's outputs (the expansion answers for
// those), or a port the node declares -- what the engine's check reads.
bool givesOutput(const Json &node, std::string_view port) {
    const auto op = text(node, "op");
    if (op == "graph.input") return port == "value";
    if (op == "graph.instance" || op == "graph.loop") return true;
    const auto outputs = node.find("outputs");
    return outputs != node.end() && outputs->is_array() &&
           std::ranges::any_of(*outputs, [&](const Json &output) {
               return output.is_string() && output.get<std::string>() == port;
           });
}

} // namespace

std::string GraphCanvasModel::suggestLoopInputName(std::string_view loop,
                                                   std::string_view base) const {
    const auto name = loopPortBase(base, "input");
    const auto *node = findNode({}, loop);
    if (node == nullptr || !isLoop(*node)) return name;
    const auto bodies = distinctLoopBodies(*node, definitions_);
    const auto inputs = node->value("inputs", Json::object());
    const auto taken = [&](const std::string &candidate) {
        if (inputs.is_object() && inputs.contains(candidate)) return true;
        return std::ranges::any_of(bodies, [&](const std::string &body) {
            return findNode(body, candidate) != nullptr;
        });
    };
    if (!taken(name)) return name;
    for (int suffix = 2; suffix < 10000; ++suffix) {
        auto candidate = name + "_" + std::to_string(suffix);
        if (!taken(candidate)) return candidate;
    }
    return name;
}

std::string GraphCanvasModel::suggestLoopOutputName(std::string_view loop,
                                                    std::string_view base) const {
    const auto name = loopPortBase(base, "output");
    const auto *node = findNode({}, loop);
    if (node == nullptr || !isLoop(*node)) return name;
    const auto bodies = distinctLoopBodies(*node, definitions_);
    const auto declared = node->value("outputs", Json::array());
    const auto taken = [&](const std::string &candidate) {
        if (declared.is_array() &&
            std::ranges::any_of(declared, [&](const Json &output) {
                return output == candidate || output == candidate + "[]";
            }))
            return true;
        return std::ranges::any_of(bodies, [&](const std::string &body) {
            const auto outputs = definitions_.at(body).value("outputs", Json::object());
            return outputs.is_object() && outputs.contains(candidate);
        });
    };
    if (!taken(name)) return name;
    for (int suffix = 2; suffix < 10000; ++suffix) {
        auto candidate = name + "_" + std::to_string(suffix);
        if (!taken(candidate)) return candidate;
    }
    return name;
}

Json GraphCanvasModel::zoneInputOps(std::string_view loop, const std::string &source_node,
                                    const std::string &source_port, const std::string &body,
                                    const std::string &target_node,
                                    const std::string &target_port) const {
    Json ops = Json::array();
    const auto *node = findNode({}, loop);
    if (node == nullptr || !isLoop(*node)) return ops;
    // A loop reading its own output would be a cycle; the source is a root
    // node, the zone's outside.
    if (source_node.empty() || source_port.empty() || source_node == loop ||
        findNode({}, source_node) == nullptr)
        return ops;
    const auto bodies = distinctLoopBodies(*node, definitions_);
    if (bodies.empty()) return ops;
    const bool targeted = !target_node.empty();
    if (targeted && (target_port.empty() || std::ranges::find(bodies, body) == bodies.end() ||
                     findNode(body, target_node) == nullptr))
        return ops;
    const auto name = suggestLoopInputName(loop, source_port);
    // Every branch's body gets the port: the engine compares their
    // boundaries (graph.loop_branch_interface@1) at the end of the list. No
    // default -- the loop's edge is what it reads, a resource as often as a
    // number.
    for (const auto &each : bodies)
        ops.push_back(Json{{"op", "add_node"}, {"scope", each},
                           {"node", Json{{"id", name}, {"op", "graph.input"}}}});
    ops.push_back(Json{{"op", "set_input"}, {"scope", "root"}, {"node", std::string{loop}},
                       {"port", name},
                       {"source", Json{{"node", source_node}, {"port", source_port}}}});
    if (targeted)
        ops.push_back(Json{{"op", "set_input"}, {"scope", body}, {"node", target_node},
                           {"port", target_port},
                           {"source", Json{{"node", name}, {"port", "value"}}}});
    return ops;
}

bool GraphCanvasModel::addZoneInput(const std::string &loop, const std::string &source_node,
                                    const std::string &source_port, const std::string &body,
                                    const std::string &target_node,
                                    const std::string &target_port) {
    if (!canEdit()) return false;
    auto ops = zoneInputOps(loop, source_node, source_port, body, target_node, target_port);
    if (ops.empty()) return false;
    return submitOps(std::move(ops));
}

Json GraphCanvasModel::zoneOutputOps(std::string_view loop, const std::string &body,
                                     const std::string &node, const std::string &port,
                                     std::string *refusal) const {
    Json ops = Json::array();
    const auto *loop_node = findNode({}, loop);
    if (loop_node == nullptr || !isLoop(*loop_node)) return ops;
    const auto bodies = distinctLoopBodies(*loop_node, definitions_);
    if (std::ranges::find(bodies, body) == bodies.end()) return ops;
    const auto *maker = findNode(body, node);
    if (maker == nullptr || port.empty() || !givesOutput(*maker, port)) return ops;
    const auto name = suggestLoopOutputName(loop, port);
    const auto branches = loopBranchBodies(*loop_node);
    for (const auto &each : bodies) {
        auto outputs = definitions_.at(each).value("outputs", Json::object());
        if (!outputs.is_object()) outputs = Json::object();
        if (each != body) {
            // Every body gives the loop the same outputs. Another body gives
            // this one from its node of the same id and port -- the same
            // wiring, which is what a copied body has -- or not at all.
            const auto *same = findNode(each, node);
            if (same == nullptr || !givesOutput(*same, port)) {
                if (refusal != nullptr) {
                    const auto branch = std::distance(
                        branches.begin(), std::ranges::find(branches, each));
                    *refusal = "Branch " + std::to_string(branch) + " of loop " +
                               std::string{loop} + " runs " + each + ", which has no " + node +
                               "." + port + " to give " + name +
                               " from: add the output there first.";
                }
                return Json::array();
            }
        }
        outputs[name] = Json{{"node", node}, {"port", port}};
        ops.push_back(Json{{"op", "set_outputs"}, {"scope", each}, {"outputs", std::move(outputs)}});
    }
    // The loop hands on the last element's value as `p`; `p[]` is the Loop
    // fields' to add.
    auto replaced = *loop_node;
    if (!replaced.contains("outputs") || !replaced.at("outputs").is_array())
        replaced["outputs"] = Json::array();
    replaced["outputs"].push_back(name);
    ops.push_back(Json{{"op", "replace_node"}, {"scope", "root"}, {"node", std::move(replaced)}});
    return ops;
}

bool GraphCanvasModel::addZoneOutput(const std::string &loop, const std::string &body,
                                     const std::string &node, const std::string &port) {
    if (!canEdit()) return false;
    std::string refusal;
    auto ops = zoneOutputOps(loop, body, node, port, &refusal);
    if (ops.empty()) {
        if (!refusal.empty()) edit_notice_ = {InspectorNoticeKind::Error, refusal};
        return false;
    }
    return submitOps(std::move(ops));
}

bool GraphCanvasModel::addLoopCarry(const std::string &loop, const std::string &from,
                                    const std::string &to) {
    const auto read = this->loop(loop);
    if (!read || from.empty() || to.empty()) return false;
    std::vector<std::pair<std::string, std::string>> carry;
    for (const auto &pair : read->carry) {
        if (pair.from == from && pair.to == to) return false;
        carry.emplace_back(pair.from, pair.to);
    }
    carry.emplace_back(from, to);
    return setLoopCarry(loop, carry);
}

bool GraphCanvasModel::removeLoopCarry(const std::string &loop, const std::string &from,
                                       const std::string &to) {
    const auto read = this->loop(loop);
    if (!read) return false;
    std::vector<std::pair<std::string, std::string>> carry;
    bool found = false;
    for (const auto &pair : read->carry) {
        if (pair.from == from && pair.to == to) {
            found = true;
            continue;
        }
        carry.emplace_back(pair.from, pair.to);
    }
    return found && setLoopCarry(loop, carry);
}

std::string GraphCanvasModel::showLoopOrigin(std::string_view loop, std::int64_t element,
                                             std::string_view node) {
    const auto *found = findNode({}, loop);
    if (found == nullptr || !isLoop(*found)) return {};
    const auto bodies = loopBranchBodies(*found);
    const auto branch = branchForElement(loop, element);
    if (!branch || *branch >= bodies.size() || bodies[*branch].empty()) return {};
    const auto body = bodies[*branch];
    if (findNode(body, node) == nullptr) return {};
    loop_branch_[std::string{loop}] = *branch;
    if (breadcrumb_.size() != 1 || !entered_loop_.empty()) {
        breadcrumb_ = {std::string{}};
        entered_loop_.clear();
        enteredScope();
    }
    return body;
}

void GraphCanvasModel::markRefusal(const Json &error, const std::string &reason) {
    refusal_marks_.clear();
    if (!error.is_object()) return;
    // The engine puts a named refusal's fields beside its code; a relayed
    // one may carry them as error_fields.
    const auto fields_member = error.find("error_fields");
    const Json &fields = fields_member != error.end() && fields_member->is_object()
                             ? *fields_member
                             : error;
    const auto code = text(error, "code");
    const auto mark = [&](const std::string &scope, const std::string &node) {
        if (node.empty() || findNode(scope, node) == nullptr) return;
        GraphCanvasRefusalMark entry{scope, node, reason};
        if (std::ranges::find(refusal_marks_, entry) == refusal_marks_.end())
            refusal_marks_.push_back(std::move(entry));
    };
    // A node of one element's copy: the loop, and the body node it copies,
    // in the scope of the branch that element ran.
    if (const auto origin = fields.find("loop_origin");
        origin != fields.end() && origin->is_object()) {
        const auto loop_id = text(*origin, "loop");
        mark({}, loop_id);
        const auto *loop_node = findNode({}, loop_id);
        const auto element = origin->value("element", std::int64_t{0});
        if (loop_node != nullptr && isLoop(*loop_node)) {
            const auto bodies = loopBranchBodies(*loop_node);
            const auto branch = branchForElement(loop_id, element);
            if (branch && *branch < bodies.size() && !bodies[*branch].empty())
                mark(bodies[*branch], text(*origin, "node"));
        }
        return;
    }
    // A loop refused by name.
    if (code.starts_with("graph.loop_")) {
        const auto node = fields.find("node");
        if (node != fields.end() && node->is_string()) mark({}, node->get<std::string>());
    }
}

std::vector<GraphCanvasCatalogEntry> GraphCanvasModel::paletteOperations(
    std::string_view text_filter, std::string_view domain) const {
    return paletteOperationsIn(currentScope(), text_filter, domain);
}

std::vector<GraphCanvasCatalogEntry> GraphCanvasModel::paletteOperationsIn(
    std::string_view scope, std::string_view text_filter, std::string_view domain) const {
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
        if (op == "graph.input" || op == "graph.instance" || op == "graph.loop") continue;
        // An operation whose ports follow its parameters cannot be authored
        // from a name alone: the palette does not pretend otherwise. LP-4c
        // makes one exception, render.fullscreen, which arrives whole: the
        // engine's passthrough drawing into an image of its own (addNodeOps).
        // LP-4e: and two more families, each on its own side: a material
        // pass in the root, which arrives with its attachments, and the draw
        // steps in a body a draw_loop runs.
        const bool seeded = op == kFullscreenTemplate ||
                            (op == kMaterialPass && scope.empty()) ||
                            (isDrawStep(op) && drawBodyScope(scope));
        if (text(declaration, "ports") != "declared" && !seeded) continue;
        GraphCanvasCatalogEntry entry{op, text(declaration, "domain"),
                                      text(declaration, "evaluation"),
                                      declaration.value("sink", false), false};
        if (matches(entry)) result.push_back(std::move(entry));
    }
    std::ranges::sort(result, {}, &GraphCanvasCatalogEntry::op);
    // A boundary port belongs to a definition body; an instance stands in the
    // root document. Offering either on the wrong side would only earn a named
    // refusal from the engine.
    if (scope.empty()) {
        if (!definitions_.empty()) {
            GraphCanvasCatalogEntry entry{"graph.instance", "structure", "direct", false, true};
            if (matches(entry)) result.push_back(std::move(entry));
        }
        // LP-4c: a loop stands in the root too, and can bring its body with
        // it (a new definition), so it is offered with no definition yet.
        GraphCanvasCatalogEntry loop_entry{"graph.loop", "structure", "direct", false, true};
        if (matches(loop_entry)) result.push_back(std::move(loop_entry));
    } else {
        GraphCanvasCatalogEntry entry{"graph.input", "structure", "direct", false, true};
        if (matches(entry)) result.push_back(std::move(entry));
    }
    return result;
}

std::vector<std::string> GraphCanvasModel::paletteDomains() const {
    return paletteDomainsIn(currentScope());
}

std::vector<std::string> GraphCanvasModel::paletteDomainsIn(std::string_view scope) const {
    std::set<std::string> domains;
    for (const auto &entry : paletteOperationsIn(scope)) domains.insert(entry.domain);
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

// --- LP-4e: the draw_loop stage, render.material_pass and draw.*.

const std::vector<std::string> &GraphCanvasModel::materialPassContracts() {
    // renderpipeline.cpp's materialPassContractFromName, less the legacy one:
    // a draw_loop does not run over it (graph.loop_contract_unsupported@1).
    static const std::vector<std::string> contracts{
        "forward_opaque_v1", "deferred_geometry_v1", "forward_transparent_v1"};
    return contracts;
}

const std::vector<std::string> &GraphCanvasModel::materialShaderContracts() {
    // What a draw_loop's `when.contract` compares (graphcomposition.cpp).
    static const std::vector<std::string> contracts{
        "gbuffer_v1", "forward_scene_color_v1", "legacy_gbuffer_v1"};
    return contracts;
}

std::vector<std::string> GraphCanvasModel::materialPassColorSockets(const Json &parameters) {
    std::vector<std::string> sockets;
    if (!parameters.is_object()) return sockets;
    if (const auto outputs = parameters.find("material_outputs"); outputs != parameters.end()) {
        // {schema: pelican.material_outputs, outputs: [{name, ...}]}
        if (outputs->is_object())
            for (const auto &output : outputs->value("outputs", Json::array()))
                if (output.is_object() && !text(output, "name").empty())
                    sockets.push_back(text(output, "name"));
        return sockets;
    }
    const auto contract = text(parameters, "material_contract");
    if (contract == "forward_opaque_v1" || contract == "forward_transparent_v1") return {"color"};
    if (contract == "deferred_geometry_v1" || contract == "legacy_gbuffer_v1")
        return {"albedo", "normal", "material", "worldpos", "emissive"};
    return sockets;
}

bool GraphCanvasModel::drawBodyScope(std::string_view scope) const {
    if (scope.empty()) return false;
    const auto found = definitions_.find(std::string{scope});
    if (found == definitions_.end() || !found->is_object()) return false;
    for (const auto &loop : nodes_) {
        if (!isDrawItemsLoop(loop)) continue;
        const auto bodies = loopBranchBodies(loop);
        if (std::ranges::find(bodies, scope) != bodies.end()) return true;
    }
    for (const auto &node : found->value("nodes", Json::array()))
        if (node.is_object() && isDrawStep(text(node, "op"))) return true;
    return false;
}

bool GraphCanvasModel::setLoopWhereTags(const std::string &id, const GraphCanvasTagFilter &tags) {
    auto parameters = loopParameters(id);
    if (tags.empty()) parameters.erase("where");
    else parameters["where"] = Json{{"tags", tagFilterJson(tags)}};
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::setLoopBranchDrawWhen(const std::string &id, std::size_t branch,
                                             const GraphCanvasDrawWhen &when) {
    // An empty condition is the else, which only the last row is; a tag
    // test with no tag in it tests nothing.
    if (when.empty() || (when.tags && when.tags->empty())) return false;
    if (when.material && when.material->empty()) return false;
    if (when.contract && std::ranges::find(materialShaderContracts(), *when.contract) ==
                             materialShaderContracts().end())
        return false;
    auto parameters = loopParameters(id);
    auto branches = parameters.value("branches", Json::array());
    if (!branches.is_array() || branch + 1 >= branches.size()) return false;
    auto &row = branches[branch];
    if (!row.is_object()) return false;
    row["when"] = drawWhenJson(when);
    parameters["branches"] = std::move(branches);
    return setLoopParameters(id, std::move(parameters));
}

std::vector<std::string> GraphCanvasModel::loopMaterials(std::string_view id) const {
    std::vector<std::string> materials;
    const auto *report = loopReport(id);
    if (report == nullptr) return materials;
    for (const auto &element : report->value("elements", Json::array()))
        if (element.is_object() && element.contains("element") && element.at("element").is_string()) {
            auto key = element.at("element").get<std::string>();
            if (std::ranges::find(materials, key) == materials.end())
                materials.push_back(std::move(key));
        }
    return materials;
}

bool GraphCanvasModel::setDrawVariant(const std::string &node, const std::string &variant) {
    return setDrawVariant(currentScope(), node, variant);
}

bool GraphCanvasModel::setDrawVariant(std::string_view scope, const std::string &node,
                                      const std::string &variant) {
    const auto *found = findNode(scope, node);
    if (found == nullptr || text(*found, "op") != "draw.variant" || variant.empty()) return false;
    if (text(nodeParameters(scope, node), "variant") == variant) return false;
    return setNodeParameter(scope, node, "variant", variant);
}

bool GraphCanvasModel::setDrawOverride(const std::string &node, const Json &render_state,
                                       const std::vector<std::string> &defines) {
    return setDrawOverride(currentScope(), node, render_state, defines);
}

bool GraphCanvasModel::setDrawOverride(std::string_view scope, const std::string &node,
                                       const Json &render_state,
                                       const std::vector<std::string> &defines) {
    if (!canEdit()) return false;
    const auto *found = findNode(scope, node);
    if (found == nullptr || text(*found, "op") != "draw.override") return false;
    if (!render_state.is_object()) return false;
    // An override that replaces nothing is refused by the engine: the step
    // would draw exactly what draw.material draws.
    if (render_state.empty() && defines.empty()) return false;
    if (std::ranges::any_of(defines, [](const std::string &define) { return define.empty(); }))
        return false;
    auto parameters = nodeParameters(scope, node);
    if (render_state.empty()) parameters.erase("render_state");
    else parameters["render_state"] = render_state;
    if (defines.empty()) parameters.erase("defines");
    else parameters["defines"] = defines;
    if (parameters == nodeParameters(scope, node)) return false;
    return submitOps(Json::array({Json{
        {"op", "set_node_parameters"}, {"scope", scopeToken(scope)}, {"node", node},
        {"parameters", std::move(parameters)}}}));
}

bool GraphCanvasModel::setMaterialPassContract(const std::string &node, const std::string &contract) {
    const auto *found = findNode(currentScope(), node);
    if (found == nullptr || text(*found, "op") != kMaterialPass) return false;
    if (std::ranges::find(materialPassContracts(), contract) == materialPassContracts().end())
        return false;
    if (text(nodeParameters(node), "material_contract") == contract) return false;
    return setNodeParameter(node, "material_contract", contract);
}

std::string GraphCanvasModel::suggestDrawVariant() const {
    return suggestDrawVariant(currentScope());
}

std::string GraphCanvasModel::suggestDrawVariant(std::string_view scope) const {
    for (const auto &loop : nodes_) {
        if (!isDrawItemsLoop(loop)) continue;
        const auto bodies = loopBranchBodies(loop);
        if (std::ranges::find(bodies, scope) == bodies.end()) continue;
        const auto id = text(loop, "id");
        for (const auto &pass : nodes_) {
            if (!pass.is_object() || text(pass, "op") != kMaterialPass) continue;
            const auto draws = pass.value("inputs", Json::object()).value("draws", Json{});
            if (!draws.is_object() || text(draws, "node") != id) continue;
            const auto variant = text(pass.value("parameters", Json::object()), "material_variant");
            if (!variant.empty()) return variant;
        }
    }
    return "variant";
}

} // namespace PelicanStudio
