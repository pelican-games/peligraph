#include "graphfold.hpp"

#include "graphcomposition.hpp"
#include "renderpipeline.hpp"

#include <picosha2.h>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace Pelican {
namespace {

using Json = nlohmann::json;

[[noreturn]] void malformed(const std::string &message) {
    throw std::invalid_argument("fold_groups " + message);
}

void requireFoldKeys(const Json &object, std::initializer_list<std::string_view> keys, const std::string &where) {
    for (const auto &[key, value] : object.items()) {
        (void)value;
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) malformed(where + " has an unknown key: " + key);
    }
}

bool isFoldDigest(const std::string &text) {
    return text.size() == 64 &&
           std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

GraphFoldSubject parseSubject(const Json &value, const std::string &group, const std::vector<std::string> &nodes,
                              const std::string &field) {
    const auto where = group + ".folded.decisions." + field;
    if (!value.is_object()) malformed(where + " entries must be objects {node, element?, port?}");
    requireFoldKeys(value, {"node", "element", "port"}, where + " entry");
    const auto node = value.find("node");
    if (node == value.end() || !node->is_string() || node->get_ref<const std::string &>().empty())
        malformed(where + " entry needs a node id");
    GraphFoldSubject subject{.node = node->get<std::string>(), .element = std::nullopt, .port = std::nullopt};
    if (const auto element = value.find("element"); element != value.end()) {
        if (!element->is_number_integer()) malformed(where + " entry element must be an integer");
        subject.element = element->get<std::int64_t>();
    }
    if (const auto port = value.find("port"); port != value.end()) {
        if (field != "no_alias") malformed(where + " entry may not name a port (only no_alias may)");
        if (!port->is_string() || port->get_ref<const std::string &>().empty())
            malformed(where + " entry port must be a non-empty string");
        subject.port = port->get<std::string>();
    }
    if (std::find(nodes.begin(), nodes.end(), subject.node) == nodes.end())
        throw GraphRegistryError(std::string{graphFoldSubjectOutsideCode}, {{"group", group}, {"subject", value}});
    return subject;
}

// The first segment of an expanded name: the root node a name under an
// instance (`<instance>/<node>`) or a loop (`<loop>/<element>/<node>`) came
// from.
std::string rootSegment(const std::string &name) {
    return name.substr(0, name.find('/'));
}

const Json *rootNode(const Json &document, const std::string &id) {
    const auto nodes = document.find("nodes");
    if (nodes == document.end() || !nodes->is_array()) return nullptr;
    for (const auto &node : *nodes)
        if (node.is_object() && node.value("id", std::string{}) == id) return &node;
    return nullptr;
}

std::string opOf(const Json *node) {
    return node != nullptr ? node->value("op", std::string{}) : std::string{};
}

// The definitions a node supplies its body from: an instance's, or each of a
// loop's branch bodies.
void appendSuppliedDefinitions(const Json &node, std::vector<std::string> &names) {
    if (!node.is_object()) return;
    const auto op = node.value("op", std::string{});
    if (op == "graph.instance") {
        if (const auto definition = node.find("definition"); definition != node.end() && definition->is_string())
            names.push_back(definition->get<std::string>());
    } else if (op == "graph.loop") {
        const auto parameters = node.find("parameters");
        if (parameters == node.end() || !parameters->is_object()) return;
        const auto branches = parameters->find("branches");
        if (branches == parameters->end() || !branches->is_array()) return;
        for (const auto &branch : *branches)
            if (branch.is_object())
                if (const auto body = branch.find("body"); body != branch.end() && body->is_string())
                    names.push_back(body->get<std::string>());
    }
}

// An input's value with only the endpoints that come from `inside`.
std::optional<Json> insideEndpoints(const Json &value, const std::set<std::string> &inside) {
    const auto fromInside = [&](const Json &endpoint) {
        const auto node = endpoint.find("node");
        return node != endpoint.end() && node->is_string() && inside.contains(node->get<std::string>());
    };
    if (value.is_object() && value.contains("node")) {
        if (!fromInside(value)) return std::nullopt;
        return value;
    }
    if (value.is_array()) {
        auto kept = Json::array();
        bool endpoints = false;
        for (const auto &entry : value) {
            if (entry.is_object() && entry.contains("node")) {
                endpoints = true;
                if (fromInside(entry)) kept.push_back(entry);
            } else {
                kept.push_back(entry);
            }
        }
        if (endpoints && kept.empty()) return std::nullopt;
        return kept;
    }
    return value;
}

} // namespace

std::vector<GraphFoldGroup> parseGraphFoldGroups(const Json &document) {
    std::vector<GraphFoldGroup> result;
    if (!document.is_object()) return result;
    const auto declared = document.find("fold_groups");
    if (declared == document.end()) return result;
    if (!declared->is_object()) malformed("must be an object of group name: {nodes, folded}");
    for (const auto &[name, entry] : declared->items()) {
        if (name.empty()) malformed("group names must not be empty");
        if (!entry.is_object()) malformed(name + " must be an object {nodes, folded}");
        requireFoldKeys(entry, {"nodes", "folded"}, name);
        GraphFoldGroup group{.group = name};
        const auto nodes = entry.find("nodes");
        if (nodes == entry.end() || !nodes->is_array() || nodes->empty())
            malformed(name + ".nodes must be a non-empty array of root node ids");
        for (const auto &node : *nodes) {
            if (!node.is_string() || node.get_ref<const std::string &>().empty())
                malformed(name + ".nodes must name root nodes by id");
            const auto id = node.get<std::string>();
            if (std::find(group.nodes.begin(), group.nodes.end(), id) != group.nodes.end())
                malformed(name + ".nodes names " + id + " twice");
            group.nodes.push_back(id);
        }
        const auto folded = entry.find("folded");
        if (folded == entry.end() || !folded->is_object()) malformed(name + ".folded must be an object {kind, ...}");
        const auto kind = folded->find("kind");
        if (kind == folded->end() || !kind->is_string()) malformed(name + ".folded.kind must be a string");
        group.kind = kind->get<std::string>();
        if (group.kind != "decisions")
            throw GraphRegistryError(std::string{graphFoldKindUnsupportedCode}, {{"group", name}, {"kind", group.kind}});
        requireFoldKeys(*folded, {"kind", "decisions", "verified"}, name + ".folded");
        if (const auto decisions = folded->find("decisions"); decisions != folded->end()) {
            if (!decisions->is_object()) malformed(name + ".folded.decisions must be an object");
            requireFoldKeys(*decisions, {"serial", "isolate", "no_alias"}, name + ".folded.decisions");
            for (auto [field, target] : {std::pair{"serial", &group.serial}, std::pair{"isolate", &group.isolate},
                                         std::pair{"no_alias", &group.no_alias}}) {
                const auto subjects = decisions->find(field);
                if (subjects == decisions->end()) continue;
                if (!subjects->is_array())
                    malformed(name + ".folded.decisions." + field + " must be an array of subjects");
                for (const auto &subject : *subjects) target->push_back(parseSubject(subject, name, group.nodes, field));
            }
        }
        if (const auto verified = folded->find("verified"); verified != folded->end()) {
            if (!verified->is_object()) malformed(name + ".folded.verified must be an object {digest, at, frames?}");
            requireFoldKeys(*verified, {"digest", "at", "frames"}, name + ".folded.verified");
            const auto digest = verified->find("digest");
            if (digest == verified->end() || !digest->is_string() || !isFoldDigest(digest->get<std::string>()))
                malformed(name + ".folded.verified.digest must be 64 lowercase hex characters");
            const auto at = verified->find("at");
            if (at == verified->end() || !at->is_string()) malformed(name + ".folded.verified.at must be a string");
            group.verified = *verified;
        }
        result.push_back(std::move(group));
    }
    return result;
}

std::string graphFoldGroupDigest(const Json &document, std::span<const std::string> nodes) {
    const std::set<std::string> inside(nodes.begin(), nodes.end());
    auto canonical_nodes = Json::object();
    std::vector<std::string> pending;
    for (const auto &id : inside) {
        const auto *node = rootNode(document, id);
        if (node == nullptr) {
            canonical_nodes[id] = nullptr;
            continue;
        }
        auto copy = *node;
        if (const auto inputs = copy.find("inputs"); inputs != copy.end() && inputs->is_object()) {
            auto kept = Json::object();
            for (const auto &[port, value] : inputs->items())
                if (auto endpoint = insideEndpoints(value, inside)) kept[port] = std::move(*endpoint);
            *inputs = std::move(kept);
        }
        appendSuppliedDefinitions(*node, pending);
        canonical_nodes[id] = std::move(copy);
    }
    auto canonical_definitions = Json::object();
    const auto definitions = document.find("definitions");
    while (!pending.empty()) {
        const auto name = std::move(pending.back());
        pending.pop_back();
        if (canonical_definitions.contains(name)) continue;
        const Json *body = nullptr;
        if (definitions != document.end() && definitions->is_object())
            if (const auto found = definitions->find(name); found != definitions->end()) body = &*found;
        if (body == nullptr) {
            canonical_definitions[name] = nullptr;
            continue;
        }
        auto copy = *body;
        if (copy.is_object()) {
            copy.erase("layout");
            if (const auto body_nodes = copy.find("nodes"); body_nodes != copy.end() && body_nodes->is_array())
                for (const auto &node : *body_nodes) appendSuppliedDefinitions(node, pending);
        }
        canonical_definitions[name] = std::move(copy);
    }
    const Json canonical{{"nodes", std::move(canonical_nodes)}, {"definitions", std::move(canonical_definitions)}};
    const auto bytes = canonical.dump();
    return picosha2::hash256_hex_string(bytes.begin(), bytes.end());
}

std::string_view graphFoldStateName(GraphFoldState state) {
    switch (state) {
    case GraphFoldState::verified: return "verified";
    case GraphFoldState::stale: return "stale";
    case GraphFoldState::unverified: return "unverified";
    }
    return "unverified";
}

std::vector<GraphFoldGroupReport> reportGraphFoldGroups(const Json &document, std::span<const GraphFoldGroup> groups) {
    std::vector<GraphFoldGroupReport> reports;
    for (const auto &group : groups) {
        GraphFoldGroupReport report{
            .group = group.group,
            .nodes = group.nodes,
            .kind = group.kind,
            .digest = graphFoldGroupDigest(document, group.nodes),
            .verified = group.verified,
        };
        if (!group.verified) report.state = GraphFoldState::unverified;
        else if (group.verified->at("digest").get<std::string>() == report.digest)
            report.state = GraphFoldState::verified;
        else report.state = GraphFoldState::stale;
        reports.push_back(std::move(report));
    }
    return reports;
}

nlohmann::ordered_json graphFoldGroupReportToJson(const GraphFoldGroupReport &report) {
    nlohmann::ordered_json result{{"group", report.group},
                                  {"nodes", report.nodes},
                                  {"kind", report.kind},
                                  {"digest", report.digest}};
    if (report.verified) result["verified"] = nlohmann::ordered_json::parse(report.verified->dump());
    result["state"] = std::string{graphFoldStateName(report.state)};
    return result;
}

GraphFoldApplication applyGraphFoldGroups(const Json &document, Json &composed,
                                          const std::vector<RenderPassProvenance> &passes,
                                          const std::vector<RenderResourceProvenance> &resources) {
    GraphFoldApplication application;
    const auto groups = parseGraphFoldGroups(document);
    if (groups.empty()) return application;
    application.groups = reportGraphFoldGroups(document, groups);

    // Where the composed config puts each name.
    std::vector<std::string> families;
    std::map<std::string, std::string> pass_family;
    if (const auto sets = composed.find("rendering_passes"); sets != composed.end() && sets->is_array())
        for (const auto &set : *sets) {
            if (!set.is_object() || !set.contains("name") || !set.at("name").is_string()) continue;
            const auto family = set.at("name").get<std::string>();
            families.push_back(family);
            if (const auto declared = set.find("passes"); declared != set.end() && declared->is_array())
                for (const auto &pass : *declared)
                    if (pass.is_object() && pass.contains("name") && pass.at("name").is_string())
                        pass_family.emplace(pass.at("name").get<std::string>(), family);
        }
    std::set<std::string> tasks;
    if (const auto declared = composed.find("compute_tasks"); declared != composed.end() && declared->is_array())
        for (const auto &task : *declared)
            if (task.is_object() && task.contains("name") && task.at("name").is_string())
                tasks.insert(task.at("name").get<std::string>());
    std::set<std::string> declared_resources;
    for (const auto *key : {"render_targets", "buffers"})
        if (const auto declared = composed.find(key); declared != composed.end() && declared->is_array())
            for (const auto &resource : *declared)
                if (resource.is_object() && resource.contains("name") && resource.at("name").is_string())
                    declared_resources.insert(resource.at("name").get<std::string>());

    // The root node each graph-made pass and task came from (GL-2a's origin;
    // a loop's copy by its loop), and the loop element when it is a copy.
    struct Made {
        std::string name;
        std::string root;
        const RenderLoopOrigin *loop = nullptr;
    };
    std::vector<Made> made_passes;
    for (const auto &provenance : passes) {
        if (provenance.source == RenderPipelineProvenanceSource::feature) continue;
        if (provenance.loop_origin) {
            made_passes.push_back({provenance.name, rootSegment(provenance.loop_origin->loop), &*provenance.loop_origin});
        } else if (provenance.graph_origin) {
            const auto &origin = *provenance.graph_origin;
            made_passes.push_back(
                {provenance.name, origin.instance_path.empty() ? origin.node : rootSegment(origin.instance_path)});
        }
    }
    std::vector<Made> made_resources;
    for (const auto &provenance : resources) {
        if (provenance.source == RenderPipelineProvenanceSource::feature) continue;
        if (!declared_resources.contains(provenance.name)) continue;
        if (provenance.loop_origin)
            made_resources.push_back(
                {provenance.name, rootSegment(provenance.loop_origin->loop), &*provenance.loop_origin});
        else if (provenance.name.find('/') != std::string::npos)
            made_resources.push_back({provenance.name, rootSegment(provenance.name)});
    }
    const auto matches = [](const Made &made, const GraphFoldSubject &subject) {
        if (made.root != subject.node) return false;
        if (!subject.element) return true;
        return made.loop != nullptr && made.loop->loop == subject.node && made.loop->element == *subject.element;
    };

    std::set<std::tuple<std::string, std::string, std::string, std::string, std::string>> seen;
    const auto add = [&](const std::string &group, const std::string &graph, const char *kind,
                         const std::string &name, const char *field) {
        if (!seen.emplace(group, graph, kind, name, field).second) return;
        application.planning.push_back({group, graph, kind, name, field});
    };
    for (std::size_t index = 0; index < groups.size(); ++index) {
        if (application.groups[index].state != GraphFoldState::verified) continue;
        const auto &group = groups[index];
        for (const auto &[field, subjects] :
             {std::pair{"serial", &group.serial}, std::pair{"isolate", &group.isolate}}) {
            for (const auto &subject : *subjects) {
                const auto op = opOf(rootNode(document, subject.node));
                // An image or buffer node makes no pass; a subject names
                // passes and tasks through the node that made them.
                if (op == "render.image" || op == "render.buffer") continue;
                for (const auto &made : made_passes) {
                    if (!matches(made, subject)) continue;
                    if (const auto family = pass_family.find(made.name); family != pass_family.end())
                        add(group.group, family->second, "node", made.name, field);
                    else if (tasks.contains(made.name))
                        for (const auto &family : families) add(group.group, family, "node", made.name, field);
                }
            }
        }
        for (const auto &subject : group.no_alias) {
            const auto *node = rootNode(document, subject.node);
            const auto op = opOf(node);
            std::vector<std::string> names;
            if (subject.port) {
                // The image or buffer the writer's input `port` is connected to.
                const auto *inputs = node != nullptr && node->contains("inputs") ? &node->at("inputs") : nullptr;
                if (inputs != nullptr && inputs->is_object()) {
                    const auto endpoint = inputs->find(*subject.port);
                    if (endpoint != inputs->end() && endpoint->is_object() && endpoint->contains("node") &&
                        endpoint->at("node").is_string()) {
                        const auto source = endpoint->at("node").get<std::string>();
                        const auto *producer = rootNode(document, source);
                        const auto producer_op = opOf(producer);
                        if (producer_op == "render.image" || producer_op == "render.buffer") {
                            names.push_back(source);
                        } else if (producer_op == "render.target_ref") {
                            const auto parameters = producer->value("parameters", Json::object());
                            if (parameters.contains("name") && parameters.at("name").is_string())
                                names.push_back(parameters.at("name").get<std::string>());
                        }
                    }
                }
            } else if (op == "render.image" || op == "render.buffer") {
                names.push_back(subject.node);
            } else if (op == "graph.loop" || op == "graph.instance") {
                for (const auto &made : made_resources)
                    if (matches(made, subject)) names.push_back(made.name);
            }
            for (const auto &name : names) {
                if (!declared_resources.contains(name)) continue;
                for (const auto &family : families) add(group.group, family, "resource", name, "no_alias");
            }
        }
    }
    if (application.planning.empty()) return application;

    auto &planning = composed["target_planning"];
    if (planning.is_null()) planning = Json::object();
    if (!planning.is_object()) throw std::runtime_error("rendering config target_planning must be an object");
    auto &graphs = planning["graphs"];
    if (graphs.is_null()) graphs = Json::object();
    if (!graphs.is_object()) throw std::runtime_error("rendering config target_planning.graphs must be an object");
    for (const auto &entry : application.planning) {
        auto &graph = graphs[entry.graph];
        if (graph.is_null()) graph = Json::object();
        auto &holder = graph[entry.kind == "node" ? "nodes" : "resources"];
        if (holder.is_null()) holder = Json::object();
        auto &declaration = holder[entry.name];
        if (declaration.is_null()) declaration = Json::object();
        if (!declaration.is_object())
            throw std::runtime_error("rendering config target_planning " + entry.graph + " " + entry.name +
                                     " must be an object");
        if (const auto found = declaration.find(entry.field);
            found != declaration.end() && found->is_boolean() && !found->get<bool>())
            throw GraphRegistryError(std::string{graphFoldDecisionConflictCode},
                                     graphFoldDecisionConflictDetail(entry));
        declaration[entry.field] = true;
    }
    return application;
}

std::optional<GraphFoldPlanningEntry> graphFoldDecisionConflict(std::span<const GraphFoldPlanningEntry> entries,
                                                                std::string_view graph, std::string_view kind,
                                                                std::string_view name, std::string_view field,
                                                                bool value) {
    if (value) return std::nullopt;
    for (const auto &entry : entries)
        if (entry.graph == graph && entry.kind == kind && entry.name == name && entry.field == field) return entry;
    return std::nullopt;
}

Json graphFoldDecisionConflictDetail(const GraphFoldPlanningEntry &entry) {
    return {{"group", entry.group}, {"name", entry.name}, {"field", entry.field}};
}

} // namespace Pelican
