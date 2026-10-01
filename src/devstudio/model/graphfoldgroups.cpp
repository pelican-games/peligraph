#include "graphfoldgroups.hpp"

#include "inspectormodel.hpp"

#include <algorithm>
#include <set>

namespace PelicanStudio {

using Json = nlohmann::json;

namespace {

std::string textAt(const Json &object, const char *key) {
    if (!object.is_object()) return {};
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

const GraphCanvasNode *findNode(const std::vector<GraphCanvasNode> &root, std::string_view id) {
    const auto found = std::ranges::find(root, id, &GraphCanvasNode::id);
    return found != root.end() ? &*found : nullptr;
}

bool resourceOp(std::string_view op) {
    return op == "render.image" || op == "render.buffer" || op == "render.target_ref";
}

} // namespace

std::string_view graphFoldFieldKey(GraphFoldField field) {
    switch (field) {
    case GraphFoldField::serial: return "serial";
    case GraphFoldField::isolate: return "isolate";
    case GraphFoldField::no_alias: return "no_alias";
    }
    return {};
}

std::string_view graphFoldFieldTitle(GraphFoldField field) {
    switch (field) {
    case GraphFoldField::serial: return "Serial";
    case GraphFoldField::isolate: return "Isolate";
    case GraphFoldField::no_alias: return "No alias";
    }
    return {};
}

Json GraphFoldSubject::toJson() const {
    Json subject{{"node", node}};
    if (element) subject["element"] = *element;
    if (!port.empty()) subject["port"] = port;
    return subject;
}

std::string GraphFoldSubject::text() const {
    auto line = node;
    if (element) line += " [" + std::to_string(*element) + "]";
    if (!port.empty()) line += "." + port;
    return line;
}

std::optional<GraphFoldSubject> GraphFoldSubject::fromJson(const Json &value) {
    if (!value.is_object()) return std::nullopt;
    GraphFoldSubject subject;
    subject.node = textAt(value, "node");
    if (subject.node.empty()) return std::nullopt;
    if (const auto element = value.find("element");
        element != value.end() && element->is_number_integer())
        subject.element = element->get<std::int64_t>();
    subject.port = textAt(value, "port");
    return subject;
}

std::vector<GraphFoldSubject> &GraphFoldGroup::subjects(GraphFoldField field) {
    return field == GraphFoldField::serial    ? serial
           : field == GraphFoldField::isolate ? isolate
                                              : no_alias;
}

const std::vector<GraphFoldSubject> &GraphFoldGroup::subjects(GraphFoldField field) const {
    return field == GraphFoldField::serial    ? serial
           : field == GraphFoldField::isolate ? isolate
                                              : no_alias;
}

bool GraphFoldGroup::contains(std::string_view node) const {
    return std::ranges::find(nodes, node) != nodes.end();
}

Json GraphFoldGroup::folded() const {
    Json decisions = Json::object();
    for (const auto field : graphFoldFields) {
        const auto &list = subjects(field);
        if (list.empty()) continue;
        auto entries = Json::array();
        for (const auto &subject : list) entries.push_back(subject.toJson());
        decisions[std::string{graphFoldFieldKey(field)}] = std::move(entries);
    }
    return Json{{"kind", kind.empty() ? std::string{"decisions"} : kind},
                {"decisions", std::move(decisions)}};
}

std::string GraphFoldGroup::stateLine() const {
    if (state == "verified") return {};
    if (state == "stale") return std::string{graphFoldStaleLine};
    return std::string{graphFoldUnverifiedLine};
}

std::string GraphFoldGroup::stateTooltip() const {
    if (state == "stale")
        return "The group's nodes changed since it was verified, so the engine runs them as they "
               "are and leaves its decisions out.\nverified digest: " +
               verified_digest + "\ncurrent digest: " + digest;
    if (state == "unverified")
        return "Nothing has checked yet that the decisions draw the same picture, so the engine "
               "runs the nodes as they are and leaves the decisions out.\ncurrent digest: " +
               digest;
    return "Verified: the engine applies the decisions.\ndigest: " + digest;
}

std::vector<GraphFoldGroup> parseGraphFoldGroupsReport(const Json &report) {
    std::vector<GraphFoldGroup> groups;
    const auto list = report.is_object() ? report.find("fold_groups") : report.end();
    if (!report.is_object() || list == report.end() || !list->is_array()) return groups;
    for (const auto &entry : *list) {
        GraphFoldGroup group;
        group.group = textAt(entry, "group");
        if (group.group.empty()) continue;
        if (const auto nodes = entry.find("nodes"); nodes != entry.end() && nodes->is_array())
            for (const auto &node : *nodes)
                if (node.is_string()) group.nodes.push_back(node.get<std::string>());
        if (const auto kind = textAt(entry, "kind"); !kind.empty()) group.kind = kind;
        group.digest = textAt(entry, "digest");
        if (const auto verified = entry.find("verified");
            verified != entry.end() && verified->is_object())
            group.verified_digest = textAt(*verified, "digest");
        if (const auto state = textAt(entry, "state"); !state.empty()) group.state = state;
        groups.push_back(std::move(group));
    }
    return groups;
}

void readGraphFoldDecisions(std::vector<GraphFoldGroup> &groups, const Json &root) {
    const auto declared = root.is_object() ? root.find("fold_groups") : root.end();
    for (auto &group : groups) {
        for (const auto field : graphFoldFields) group.subjects(field).clear();
        if (!root.is_object() || declared == root.end() || !declared->is_object()) continue;
        const auto entry = declared->find(group.group);
        if (entry == declared->end() || !entry->is_object()) continue;
        const auto folded = entry->find("folded");
        if (folded == entry->end() || !folded->is_object()) continue;
        const auto decisions = folded->find("decisions");
        if (decisions == folded->end() || !decisions->is_object()) continue;
        for (const auto field : graphFoldFields) {
            const auto list = decisions->find(std::string{graphFoldFieldKey(field)});
            if (list == decisions->end() || !list->is_array()) continue;
            for (const auto &value : *list)
                if (auto subject = GraphFoldSubject::fromJson(value))
                    group.subjects(field).push_back(std::move(*subject));
        }
    }
}

std::string suggestGraphFoldGroupName(const std::vector<GraphFoldGroup> &groups,
                                      const std::vector<std::string> &nodes,
                                      const std::vector<GraphCanvasNode> &root) {
    const auto taken = [&](const std::string &name) {
        return std::ranges::any_of(groups, [&](const GraphFoldGroup &group) {
            return group.group == name;
        });
    };
    // `<loop>_fold`, then `<loop>_fold_2`; `fold_1`, then `fold_2`.
    const auto free = [&](const std::string &base, bool bare) {
        if (bare && !taken(base)) return base;
        for (int index = bare ? 2 : 1;; ++index) {
            const auto name = base + "_" + std::to_string(index);
            if (!taken(name)) return name;
        }
    };
    for (const auto &id : nodes)
        if (const auto *node = findNode(root, id);
            node != nullptr && (node->loop || node->op == "graph.loop"))
            return free(id + "_fold", true);
    return free("fold", false);
}

Json graphFoldNodesParams(const std::string &base_source_digest, const std::string &group,
                          const std::vector<std::string> &nodes, const Json &folded) {
    Json params{{"base_source_digest", base_source_digest}, {"group", group}, {"folded", folded}};
    if (!folded.is_null()) params["nodes"] = nodes;
    return params;
}

Json graphFoldNewFolded() {
    return Json{{"kind", "decisions"}, {"decisions", Json::object()}};
}

std::vector<GraphFoldNodeChoice> graphFoldNodeChoices(
    const GraphFoldGroup &group, GraphFoldField field, const std::vector<GraphCanvasNode> &root,
    const std::map<std::string, std::int64_t, std::less<>> &loop_counts) {
    std::vector<GraphFoldNodeChoice> choices;
    for (const auto &id : group.nodes) {
        const auto *node = findNode(root, id);
        GraphFoldNodeChoice choice;
        choice.node = id;
        choice.op = node != nullptr ? node->op : std::string{};
        choice.loop = node != nullptr && (node->loop || node->op == "graph.loop");
        if (choice.loop)
            if (const auto count = loop_counts.find(id); count != loop_counts.end())
                choice.count = count->second;
        if (field == GraphFoldField::no_alias) {
            const bool subject = choice.loop || choice.op == "render.image" ||
                                 choice.op == "render.buffer" || choice.op == "graph.instance";
            if (!subject && node != nullptr && node->op.starts_with("render.")) {
                // A writer: the inputs it reads from an image, a buffer or a
                // target_ref -- the resource the port is connected to.
                std::set<std::string> seen;
                for (const auto &port : node->inputs) {
                    if (!port.connected || seen.contains(port.port)) continue;
                    const auto *source = findNode(root, port.source_node);
                    if (source == nullptr || !resourceOp(source->op)) continue;
                    seen.insert(port.port);
                    choice.ports.push_back(port.port);
                }
                for (const auto &socket : node->input_sockets) {
                    if (!socket.connected() || seen.contains(socket.port)) continue;
                    const auto *source = findNode(root, socket.source_node);
                    if (source == nullptr || !resourceOp(source->op)) continue;
                    seen.insert(socket.port);
                    choice.ports.push_back(socket.port);
                }
            }
            if (!subject && choice.ports.empty()) continue;
            choice.needs_port = !subject;
        }
        choices.push_back(std::move(choice));
    }
    return choices;
}

std::string graphFoldPickRefusal(const std::vector<GraphFoldPick> &picks) {
    if (picks.empty()) return "Pick the root nodes to fold first.";
    for (const auto &pick : picks) {
        if (!pick.zone_scope.empty())
            return pick.node + " is inside a loop's zone (" + pick.zone_scope +
                   "): fold the loop itself.";
        if (pick.derived)
            return pick.node + " is a folded feature or a pass of the rendering section, not a "
                               "node of the graph.";
    }
    return {};
}

Json graphFoldVerifyParams(const std::string &base_source_digest, const std::string &group,
                           bool quick) {
    Json params{{"base_source_digest", base_source_digest}, {"group", group}};
    if (quick) {
        params["frames"] = 2;
        params["warmup_seconds"] = 1;
    }
    return params;
}

namespace {

std::string numberAt(const Json &object, const char *key) {
    if (!object.is_object()) return "?";
    const auto found = object.find(key);
    if (found == object.end()) return "?";
    if (found->is_string()) return found->get<std::string>();
    if (found->is_number()) return found->dump();
    return "?";
}

} // namespace

GraphFoldVerifyOutcome readGraphFoldVerifyOutcome(const Json &response, const std::string &group) {
    GraphFoldVerifyOutcome outcome;
    outcome.group = group;
    if (!response.is_object()) {
        outcome.notice = group + ": the engine's answer to verify_fold could not be read.";
        return outcome;
    }
    if (response.value("status", std::string{}) == "committed" &&
        response.value("committed", false)) {
        outcome.kind = GraphFoldVerifyOutcome::Kind::passed;
        outcome.notice = group + " verified \xE2\x80\x94 its decisions now apply";
        return outcome;
    }
    const auto error = response.find("error");
    const auto &reason = error != response.end() && error->is_object() ? *error : response;
    const auto code = reason.value("code", std::string{});
    const auto detail = reason.value("detail", Json::object());
    if (code == "graph.fold_mismatch@1") {
        outcome.kind = GraphFoldVerifyOutcome::Kind::mismatch;
        const auto what = reason.value("what", std::string{});
        if (what == "pixels")
            outcome.heading = "frame " + numberAt(detail, "frame") + " differs in " +
                              numberAt(detail, "differing_pixels") + " pixels";
        else if (what == "validation")
            outcome.heading = numberAt(detail, "validation_errors") +
                              " validation errors (open form: " +
                              numberAt(detail, "control_validation_errors") + ")";
        else if (what == "compile")
            outcome.heading = "the decided form exited with " + numberAt(detail, "exit_code");
        else
            outcome.heading = "the decided form differs (" + what + ")";
        outcome.notice = group + " is not verified: " + outcome.heading +
                         " \xE2\x80\x94 its decisions still do not apply.";
        return outcome;
    }
    if (code == "graph.fold_verify_unstable@1") {
        outcome.kind = GraphFoldVerifyOutcome::Kind::unstable;
        outcome.heading = "the open form itself is not stable (frame " + numberAt(detail, "frame") +
                          ", " + numberAt(detail, "differing_pixels") +
                          " pixels) \xE2\x80\x94 not the decisions' fault";
        outcome.notice = group + ": " + outcome.heading +
                         ". Clear Quick check (a longer warm-up) and verify again.";
        return outcome;
    }
    if (code == "external_modification") {
        outcome.notice = group + ": the graph changed while verifying; verify again.";
        return outcome;
    }
    // The refusals Studio has a sentence for (inspectormodel's table).
    auto line = graphLoopRefusalDescription(reason);
    if (line.empty()) {
        const auto message = reason.value("message", std::string{});
        line = code.empty() ? message : message.empty() ? code : code + ": " + message;
    }
    outcome.notice = line.empty() ? group + ": verify_fold was refused." : line;
    return outcome;
}

std::string graphFoldVerifyingLine(std::int64_t seconds) {
    return "verifying\xE2\x80\xA6 (" + std::to_string(seconds) + " s)";
}

namespace {

// The root node (and loop element) a pass came from, by its origin, else by
// the first segments of its expanded name.
std::pair<std::string, std::optional<std::int64_t>> rootOf(const std::string &name,
                                                           const GraphCanvasPlanPass *pass) {
    if (pass != nullptr && pass->origin) {
        const auto &origin = *pass->origin;
        if (!origin.loop.empty()) return {origin.loop, origin.element};
        if (!origin.instance_path.empty())
            return {origin.instance_path.substr(0, origin.instance_path.find('/')), std::nullopt};
        return {origin.node, std::nullopt};
    }
    const auto slash = name.find('/');
    if (slash == std::string::npos) return {name, std::nullopt};
    const auto first = name.substr(0, slash);
    const auto rest = name.substr(slash + 1);
    const auto next = rest.find('/');
    const auto segment = rest.substr(0, next);
    if (next != std::string::npos && !segment.empty() &&
        std::ranges::all_of(segment, [](char c) { return c >= '0' && c <= '9'; }))
        return {first, std::stoll(segment)};
    return {first, std::nullopt};
}

} // namespace

std::vector<std::string> graphFoldAppliedNames(
    const std::string &group, GraphFoldField field, const GraphFoldSubject &subject,
    const std::vector<GraphCanvasFoldDecision> &decisions,
    const std::vector<GraphCanvasPlanPass> &passes,
    const std::function<std::string(const std::string &, const std::string &)> &port_resource) {
    std::vector<std::string> names;
    const std::string key{graphFoldFieldKey(field)};
    const auto port_target =
        !subject.port.empty() && port_resource ? port_resource(subject.node, subject.port)
                                               : std::string{};
    for (const auto &decision : decisions) {
        if (decision.group != group || decision.field != key) continue;
        bool here = false;
        if (!subject.port.empty()) {
            here = !port_target.empty() && decision.name == port_target;
        } else {
            const GraphCanvasPlanPass *pass = nullptr;
            if (decision.kind == "node")
                if (const auto found = std::ranges::find(passes, decision.name, &GraphCanvasPlanPass::name);
                    found != passes.end())
                    pass = &*found;
            const auto [node, element] = rootOf(decision.name, pass);
            here = node == subject.node && (!subject.element || element == subject.element);
        }
        if (here && std::ranges::find(names, decision.name) == names.end())
            names.push_back(decision.name);
    }
    return names;
}

GraphTargetPlanning GraphTargetPlanning::fromRoot(const Json &root, const std::string &graph) {
    GraphTargetPlanning planning;
    planning.read = true;
    if (!root.is_object()) return planning;
    const auto rendering = root.find("rendering");
    if (rendering == root.end() || !rendering->is_object()) return planning;
    const auto target = rendering->find("target_planning");
    if (target == rendering->end() || !target->is_object()) return planning;
    const auto graphs = target->find("graphs");
    if (graphs == target->end() || !graphs->is_object()) return planning;
    const auto declared = graphs->find(graph);
    if (declared == graphs->end() || !declared->is_object()) return planning;
    if (const auto nodes = declared->find("nodes"); nodes != declared->end() && nodes->is_object())
        planning.nodes = *nodes;
    if (const auto resources = declared->find("resources");
        resources != declared->end() && resources->is_object())
        planning.resources = *resources;
    return planning;
}

bool GraphTargetPlanning::flag(std::string_view kind, std::string_view name,
                               std::string_view field) const {
    const auto &entries = kind == "resource" ? resources : nodes;
    const auto entry = entries.find(std::string{name});
    if (entry == entries.end() || !entry->is_object()) return false;
    const auto value = entry->find(std::string{field});
    return value != entry->end() && value->is_boolean() && value->get<bool>();
}

Json graphTargetPlanningParams(const std::string &base_source_digest, const std::string &graph,
                               std::string_view kind, const std::string &name,
                               const std::string &field, bool on) {
    const std::string member = kind == "resource" ? "resources" : "nodes";
    return Json{{"base_source_digest", base_source_digest},
                {"graph", graph},
                {member, Json{{name, Json{{field, on ? Json(true) : Json(nullptr)}}}}}};
}

} // namespace PelicanStudio
