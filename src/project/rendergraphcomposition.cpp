#include "rendergraphcomposition.hpp"
#include "featurecompose.hpp"
#include "graphpartition.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>

#include <stdexcept>
#include <set>

namespace Pelican {
namespace {
using Json = nlohmann::json;
void forbid(const Json &parameters, std::initializer_list<const char *> fields) {
    if (!parameters.is_object()) throw std::invalid_argument("render node parameters must be an object");
    for (const auto field : fields)
        if (parameters.contains(field))
            throw std::invalid_argument(std::string("render graph derives field from connections: ") + field);
}
}

LoweredRenderGraphDocument lowerRenderGraphDocumentForComposition(const Json &document,
    const std::set<std::string> &inactive_scopes, const GraphModuleRegistry &modules) {
    const auto graph = partitionGraphDocument(document, modules).rendering;
    LoweredRenderGraphDocument lowered;
    auto config = document.value("rendering", Json::object());
    // Existing global settings, presets and image passes can coexist during
    // migration, but buffer/compute declarations have one authoring owner.
    forbid(config, {"buffers", "compute_tasks"});
    config["buffers"] = Json::array();
    config["compute_tasks"] = Json::array();
    std::map<std::string, const Json *> nodes;
    for (const auto &node : graph.nodes) nodes.emplace(node.at("id").get<std::string>(), &node);
    const auto inactive = [&](const std::string &id) {
        const auto found = graph.node_scopes.find(id);
        return found != graph.node_scopes.end() &&
            std::any_of(found->second.begin(), found->second.end(),
                [&](const auto &scope) { return inactive_scopes.contains(scope); });
    };
    // LP-0b: every graphics pass in lowering (id) order with its lowered edges,
    // and the passes that wait for an anchor.
    struct PendingPlacement {
        std::string node;
        std::string family;
        bool after;
        std::string anchor;
        Json pass;
    };
    std::vector<std::string> graph_pass_order;
    std::map<std::string, std::pair<Json, Json>> graph_pass_edges;
    std::vector<PendingPlacement> pending_placements;
    std::set<std::string> resolving;
    std::function<std::string(const Json &)> resourceName = [&](const Json &endpoint) {
        const auto id = endpoint.at("node").get<std::string>();
        const auto &node = *nodes.at(id);
        if (node.at("op") == "render.buffer" || node.at("op") == "render.attribute" ||
            node.at("op") == "render.image") return id;
        // LP-0a: a reference owns nothing; the port uses the target's own name.
        if (node.at("op") == "render.target_ref")
            return node.at("parameters").at("name").get<std::string>();
        if (!resolving.insert(id).second)
            throw std::invalid_argument("cyclic render resource connection: " + id);
        auto resource = resourceName(node.at("inputs").at(endpoint.at("port").get<std::string>()));
        resolving.erase(id);
        return resource;
    };
    for (const auto &node : graph.nodes) {
        const auto id = node.at("id").get<std::string>();
        if (inactive(id)) continue;
        // A contract can be registered without a backend that runs it. Say so
        // by name instead of lowering an unknown operation as if it were a pass.
        static const std::set<std::string> lowerable{"render.buffer", "render.attribute",
            "render.image", "render.target_ref", "render.compute", "render.fullscreen", "render.raster"};
        if (!lowerable.contains(node.at("op").get<std::string>()))
            throw GraphRegistryError("graph.operation_unexecutable@1",
                {{"node", id}, {"op", node.at("op").get<std::string>()},
                 {"backend", "pelican.render"}});
        auto parameters = node.at("parameters");
        // Ordering is not a data dependency: an active task need not wait for
        // a task that no longer executes. Unknown names still reach validation.
        for (const auto *key : {"after", "before"}) {
            if (!parameters.contains(key)) continue;
            auto &order = parameters.at(key);
            if (!order.is_array()) throw std::invalid_argument("render ordering must be an array");
            order.erase(std::remove_if(order.begin(), order.end(), [&](const Json &name) {
                return inactive(name.get<std::string>());
            }), order.end());
        }
        if (node.at("op") == "render.buffer" || node.at("op") == "render.attribute") {
            if (parameters.contains("source"))
                throw std::invalid_argument("attribute source must be resolved before render lowering: " + id);
            forbid(parameters, {"name"});
            if (node.at("op") == "render.attribute" && parameters.at("domain") == "index")
                parameters["index_buffer"] = true;
            parameters.erase("element");
            parameters.erase("domain");
            parameters.erase("count");
            parameters["name"] = id;
            config["buffers"].push_back(std::move(parameters));
            continue;
        }
        if (node.at("op") == "render.target_ref") {
            // No declaration: the target, its aliasing and its history stay
            // with whoever declared it. Whether it exists is known only after
            // composition (resolveGraphDocumentAfterComposition).
            lowered.target_refs.push_back({id, parameters.at("name").get<std::string>(), parameters.at("type")});
            continue;
        }
        if (node.at("op") == "render.image") {
            forbid(parameters, {"name", "kind"});
            parameters.erase("type");
            parameters["name"] = id;
            config["render_targets"].push_back(std::move(parameters));
            continue;
        }
        const bool graphics = node.at("op") != "render.compute";
        forbid(parameters, {"name", "reads", "writes", "resource_ports"});
        if (graphics) forbid(parameters, {"type", "input", "output"});
        auto ports = parameters.at("ports");
        parameters.erase("ports");
        if (node.at("op") == "render.raster" && parameters.contains("draw")) {
            auto &draw = parameters.at("draw");
            const auto bind_count = [&](const char *source_key, const char *count_key,
                                        const char *first_key, const char *domain) {
                if (!draw.contains(source_key)) return;
                if (draw.contains(count_key))
                    throw std::invalid_argument(std::string("draw count is derived from attribute: ") + count_key);
                const auto port = draw.at(source_key).get<std::string>();
                const auto &attribute = *nodes.at(resourceName(node.at("inputs").at(port)));
                if (attribute.at("op") != "render.attribute" ||
                    attribute.at("parameters").at("domain") != domain || ports.at(port).at("access") != "read")
                    throw std::invalid_argument(std::string("draw source requires a readable ") + domain + " attribute");
                const auto count = attribute.at("parameters").at("count").get<std::uint64_t>();
                const auto first = draw.value(first_key, std::uint64_t{0});
                if (first > count || count > (std::numeric_limits<std::uint32_t>::max)())
                    throw std::invalid_argument("attribute range exceeds direct draw limits");
                draw[count_key] = count - first;
                draw.erase(source_key);
            };
            if (draw.contains("index_source")) {
                forbid(draw, {"vertex_source", "vertex_count", "first_vertex", "index_buffer", "operation"});
                const auto port = draw.at("index_source").get<std::string>();
                const auto buffer = resourceName(node.at("inputs").at(port));
                bind_count("index_source", "index_count", "first_index", "index");
                draw["operation"] = "indexed";
                draw["index_buffer"] = buffer;
            }
            bind_count("vertex_source", "vertex_count", "first_vertex", "vertex");
            bind_count("instance_source", "instance_count", "first_instance", "instance");
        }
        parameters["name"] = id;
        parameters["reads"] = Json::array();
        parameters["writes"] = Json::array();
        parameters["resource_ports"] = Json::object();
        Json attachments = {{"color", Json::array()}, {"depth", nullptr}};
        std::map<unsigned, std::string> colors;
        std::set<std::string> reads, writes;
        auto after = parameters.value("after", Json::array());
        if (!after.is_array()) throw std::invalid_argument("render after must be an array");
        for (const auto &[port, declaration] : ports.items()) {
            for (const auto &[key, value] : declaration.items())
                if (key != "element" && key != "domain" && key != "access" && key != "kind" && key != "type" &&
                    key != "binding" && key != "view" && key != "sampling" && key != "subresource" &&
                    key != "history" &&
                    !(graphics && (key == "attachment" || key == "location")))
                    throw std::invalid_argument("unknown render port setting: " + key);
            const auto &endpoint = node.at("inputs").at(port);
            const auto producer = endpoint.at("node").get<std::string>();
            // The port contract admits `history` only on an image read
            // (graph.history_write@1), so this reads the previous frame and
            // waits for no pass of this one.
            const bool history = declaration.value("history", false);
            const auto target = resourceName(endpoint);
            const auto resource = history ? target + "@history" : target;
            if (history) lowered.history_reads.push_back({id, port, target});
            if (!history &&
                nodes.at(producer)->at("op") != "render.buffer" &&
                nodes.at(producer)->at("op") != "render.attribute" &&
                nodes.at(producer)->at("op") != "render.image" &&
                nodes.at(producer)->at("op") != "render.target_ref" &&
                std::find(after.begin(), after.end(), producer) == after.end())
                after.push_back(producer);
            const auto access = declaration.at("access").get<std::string>();
            if (access != "read" && access != "write" && access != "read_write")
                throw std::invalid_argument("unknown compute port access: " + access);
            if (graphics && declaration.contains("attachment")) {
                if (declaration.value("kind", std::string{"buffer"}) != "image" || access != "write")
                    throw std::invalid_argument("render attachment requires a write image port: " + port);
                for (const auto *key : {"binding", "view", "sampling", "subresource"})
                    if (declaration.contains(key))
                        throw std::invalid_argument("shader resource setting on render attachment: " + port);
                const auto attachment = declaration.at("attachment").get<std::string>();
                if (attachment == "color") {
                    const auto &location = declaration.at("location");
                    if (!location.is_number_integer() || location.get<std::int64_t>() < 0 ||
                        location.get<std::uint64_t>() > (std::numeric_limits<unsigned>::max)())
                        throw std::invalid_argument("invalid color attachment location: " + port);
                    if (!colors.emplace(location.get<unsigned>(), resource).second)
                        throw std::invalid_argument("duplicate color attachment location");
                } else if (attachment == "depth") {
                    if (declaration.contains("location") || !attachments["depth"].is_null())
                        throw std::invalid_argument("duplicate depth attachment or invalid depth location");
                    attachments["depth"] = resource;
                } else throw std::invalid_argument("unknown render attachment: " + attachment);
                continue;
            }
            if (graphics && (access != "read" || declaration.contains("location")))
                throw std::invalid_argument("graphics shader ports require read access; outputs require attachments");
            if (access != "write") reads.insert(resource);
            if (access != "read") writes.insert(resource);
            auto binding = declaration;
            binding.erase("type");
            binding.erase("history");
            binding["resource"] = resource;
            binding["kind"] = declaration.value("kind", std::string{"buffer"});
            binding["access"] = declaration.value("binding",
                graphics && binding["kind"] == "image" ? std::string{"sampled"} : std::string{"storage"});
            binding.erase("binding");
            parameters["resource_ports"][port] = std::move(binding);
        }
        parameters["reads"] = reads;
        parameters["writes"] = writes;
        parameters["after"] = std::move(after);
        if (graphics) {
            for (const auto &[location, resource] : colors) {
                if (location != attachments["color"].size())
                    throw std::invalid_argument("color attachment locations must be contiguous from zero");
                attachments["color"].push_back(resource);
            }
            parameters.erase("reads");
            parameters.erase("writes");
            parameters["input"] = reads;
            parameters["output"] = std::move(attachments);
            parameters["type"] = node.at("op") == "render.fullscreen" ? "fullscreen" : "raster";
            const auto family = parameters.value("pass_set", std::string{"main"});
            parameters.erase("pass_set");
            if (!graph.node_scopes.at(id).empty())
                config["target_planning"]["graphs"][family]["nodes"][id]["isolate"] = true;
            graph_pass_order.push_back(id);
            graph_pass_edges.emplace(id, std::pair{parameters.value("after", Json::array()),
                                                   parameters.value("before", Json::array())});
            // LP-0b: the anchor may be a preset's or a feature's pass, so the
            // pass waits for the resolution stage instead of joining its family
            // here. The contract has checked the shape.
            if (parameters.contains("insert") || parameters.contains("canonical_anchor")) {
                PendingPlacement pending{id, family, true, {}, {}};
                if (parameters.contains("insert")) {
                    const auto &insert = parameters.at("insert");
                    pending.after = insert.contains("after");
                    pending.anchor = insert.at(pending.after ? "after" : "before").get<std::string>();
                } else {
                    pending.anchor = parameters.at("canonical_anchor").get<std::string>();
                }
                parameters.erase("insert");
                parameters.erase("canonical_anchor");
                pending.pass = std::move(parameters);
                pending_placements.push_back(std::move(pending));
                continue;
            }
            auto &families = config["rendering_passes"];
            if (families.is_null()) families = Json::array();
            auto found = std::find_if(families.begin(), families.end(), [&](const Json &candidate) {
                return candidate.at("name") == family;
            });
            if (found == families.end()) {
                families.push_back({{"name", family}, {"passes", Json::array()}});
                found = std::prev(families.end());
            }
            (*found)["passes"].push_back(std::move(parameters));
        } else config["compute_tasks"].push_back(std::move(parameters));
    }
    if (!pending_placements.empty()) {
        // The graph's own order: the expansion lists nodes by id, not by
        // dependency, so rank the graphics passes by their edges (a stable
        // topological order; ties keep the id order). A dependency through a
        // pass that is not placed still counts.
        std::map<std::string, std::set<std::string>> predecessors;
        for (const auto &id : graph_pass_order) predecessors[id];
        for (const auto &[id, edges] : graph_pass_edges) {
            for (const auto &name : edges.first)
                if (name.is_string() && graph_pass_edges.contains(name.get<std::string>()))
                    predecessors[id].insert(name.get<std::string>());
            for (const auto &name : edges.second)
                if (name.is_string() && graph_pass_edges.contains(name.get<std::string>()))
                    predecessors[name.get<std::string>()].insert(id);
        }
        std::map<std::string, std::size_t> rank;
        while (rank.size() < graph_pass_order.size()) {
            const auto ready = std::find_if(graph_pass_order.begin(), graph_pass_order.end(),
                [&](const std::string &id) {
                    return !rank.contains(id) && std::all_of(predecessors[id].begin(), predecessors[id].end(),
                        [&](const std::string &before) { return rank.contains(before); });
                });
            // A cycle: keep the rest in id order; the frame planner refuses it.
            const auto next = ready != graph_pass_order.end() ? ready
                : std::find_if(graph_pass_order.begin(), graph_pass_order.end(),
                      [&](const std::string &id) { return !rank.contains(id); });
            rank.emplace(*next, rank.size());
        }
        std::stable_sort(pending_placements.begin(), pending_placements.end(),
            [&](const PendingPlacement &lhs, const PendingPlacement &rhs) {
                return rank.at(lhs.node) < rank.at(rhs.node);
            });
        for (auto &pending : pending_placements) {
            const auto run = std::find_if(lowered.placed_runs.begin(), lowered.placed_runs.end(),
                [&](const RenderGraphPassRun &candidate) {
                    return candidate.family == pending.family && candidate.after == pending.after &&
                        candidate.anchor == pending.anchor;
                });
            if (run == lowered.placed_runs.end())
                lowered.placed_runs.push_back({pending.node, pending.family, pending.after, pending.anchor,
                                               {std::move(pending.pass)}});
            else run->passes.push_back(std::move(pending.pass));
        }
    }
    lowered.config = std::move(config);
    return lowered;
}

void placeRenderGraphPassRun(Json &composed, const RenderGraphPassRun &run) {
    if (run.passes.empty()) return;
    // The same lookup a feature's insert uses, kept to the run's family: a
    // pass family is its own frame graph, and an edge across two means nothing.
    const auto matches = findRenderPassAnchorMatches(composed, run.anchor);
    std::vector<RenderPassAnchorMatch> own;
    for (const auto &match : matches)
        if (match.family == run.family) own.push_back(match);
    if (own.empty())
        throw GraphRegistryError(matches.empty() ? "graph.insert_anchor_unknown@1" : "graph.insert_family_mismatch@1",
                                 {{"node", run.node}, {"anchor", run.anchor}});
    if (own.size() > 1)
        throw std::runtime_error("render graph pass insert anchor is ambiguous: " + run.anchor);
    std::set<std::string> taken;
    for (const auto &family : composed.at("rendering_passes"))
        for (const auto &pass : family.at("passes")) taken.insert(pass.value("name", std::string{}));
    if (const auto tasks = composed.find("compute_tasks"); tasks != composed.end() && tasks->is_array())
        for (const auto &task : *tasks) taken.insert(task.value("name", std::string{}));
    for (const auto &pass : run.passes) {
        const auto name = pass.at("name").get<std::string>();
        if (name == "output_transform" || name.rfind("__anchor_", 0) == 0 || !taken.insert(name).second)
            throw std::runtime_error("Pass name collides with canonical frame node: " + name);
    }
    insertRenderPassRunAtAnchor(*own.front().passes, own.front().index, run.after, run.passes);
}

Json lowerRenderGraphDocument(const Json &document,
    const std::set<std::string> &inactive_scopes, const GraphModuleRegistry &modules) {
    return lowerRenderGraphDocumentForComposition(document, inactive_scopes, modules).config;
}

namespace {
// A composed target's image in the terms of a logical image type. An absent
// field is what the target parser defaults it to; an absent format (a target
// the color resolver fills in later) says nothing about the kind.
std::string composedTargetKind(const Json &target) {
    const auto format = target.value("format", std::string{});
    if (format.empty()) return {};
    return format.front() == 'D' ? "depth" : "color";
}

std::string composedTargetTopology(const Json &target) {
    if (target.value("dimension", std::string{"2d"}) == "cube") return "cube";
    const auto layers = target.find("layers");
    if (layers != target.end() && layers->is_number_unsigned() && layers->get<std::uint64_t>() > 1)
        return "two_d_array";
    return "two_d";
}
}

void resolveGraphDocumentAfterComposition(Json &composed, const LoweredRenderGraphDocument &lowered) {
    std::map<std::string, const Json *> targets;
    if (const auto found = composed.find("render_targets"); found != composed.end() && found->is_array())
        for (const auto &target : *found)
            if (target.is_object() && target.contains("name") && target.at("name").is_string())
                targets.emplace(target.at("name").get<std::string>(), &target);
    for (const auto &reference : lowered.target_refs) {
        const auto found = targets.find(reference.name);
        if (found == targets.end())
            throw GraphRegistryError("graph.target_ref_unknown@1",
                {{"node", reference.node}, {"name", reference.name}});
        // The declared type is only what connections are matched by; the
        // target's own declaration decides its image. The two may not say
        // different things. legacy_opaque_image states nothing to compare.
        const auto semantic = reference.type.at("semantic").get<std::string>();
        const auto declared_kind = semantic == "pelican.render.color_signal@1" ? std::string{"color"}
            : semantic == "pelican.render.depth@1" ? std::string{"depth"} : std::string{};
        if (declared_kind.empty()) continue;
        const auto mismatch = [&](const char *field, const std::string &declared, const std::string &actual) {
            throw GraphRegistryError("graph.target_ref_type_mismatch@1",
                {{"node", reference.node}, {"name", reference.name}, {"field", field},
                 {"declared", declared}, {"actual", actual}});
        };
        const auto actual_kind = composedTargetKind(*found->second);
        if (!actual_kind.empty() && actual_kind != declared_kind)
            mismatch("kind", declared_kind, actual_kind);
        const auto &arguments = reference.type.value("arguments", Json::object());
        const auto declared_topology = arguments.value("topology", std::string{"two_d"});
        const auto actual_topology = composedTargetTopology(*found->second);
        if (declared_topology != actual_topology)
            mismatch("topology", declared_topology, actual_topology);
    }
    // A history read of a target that keeps none would read nothing. The
    // runtime refuses it too, but only by text and only after every pass is
    // compiled; the graph names the node and port here.
    for (const auto &read : lowered.history_reads) {
        const auto found = targets.find(read.target);
        if (found != targets.end() && !found->second->value("history", false))
            throw GraphRegistryError("graph.history_unavailable@1",
                {{"node", read.node}, {"port", read.port}, {"target", read.target}});
    }
    // LP-0b: place each run once its anchor exists. A run may be anchored at
    // a pass another run places, so place what can be placed until nothing
    // moves; the first run left is refused by name.
    std::vector<const RenderGraphPassRun *> pending;
    for (const auto &run : lowered.placed_runs) pending.push_back(&run);
    while (!pending.empty()) {
        const auto ready = std::find_if(pending.begin(), pending.end(), [&](const RenderGraphPassRun *run) {
            return !findRenderPassAnchorMatches(composed, run->anchor).empty();
        });
        const auto next = ready != pending.end() ? ready : pending.begin();
        placeRenderGraphPassRun(composed, **next);
        pending.erase(next);
    }
    // LP-1a (graph.loop expansion) and LP-0d (pass_replacements, variant
    // removals) continue here.
}
}
