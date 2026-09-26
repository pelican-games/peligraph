#include "rendergraphcomposition.hpp"
#include "featurecompose.hpp"
#include "graphpartition.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>

#include <stdexcept>
#include <set>
#include <functional>
#include <string_view>

namespace Pelican {
namespace {
using Json = nlohmann::json;
void forbid(const Json &parameters, std::initializer_list<const char *> fields) {
    if (!parameters.is_object()) throw std::invalid_argument("render node parameters must be an object");
    for (const auto field : fields)
        if (parameters.contains(field))
            throw std::invalid_argument(std::string("render graph derives field from connections: ") + field);
}

std::vector<std::string> orderNames(const Json &declaration, const char *key) {
    std::vector<std::string> names;
    const auto found = declaration.find(key);
    if (found == declaration.end() || !found->is_array()) return names;
    for (const auto &name : *found)
        if (name.is_string()) names.push_back(name.get<std::string>());
    return names;
}

// A stable topological order of declarations by the after / before edges
// among them; ties keep the order given. A cycle keeps the rest in the order
// given (the frame planner refuses it). This is the order LP-0b gives a run.
std::vector<std::size_t> rankByOrderEdges(const std::vector<const Json *> &declarations) {
    std::map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < declarations.size(); ++i)
        index.emplace(declarations[i]->at("name").get<std::string>(), i);
    std::vector<std::set<std::size_t>> predecessors(declarations.size());
    for (std::size_t i = 0; i < declarations.size(); ++i) {
        for (const auto &name : orderNames(*declarations[i], "after"))
            if (const auto found = index.find(name); found != index.end() && found->second != i)
                predecessors[i].insert(found->second);
        for (const auto &name : orderNames(*declarations[i], "before"))
            if (const auto found = index.find(name); found != index.end() && found->second != i)
                predecessors[found->second].insert(i);
    }
    std::vector<std::size_t> order;
    std::vector<bool> ranked(declarations.size(), false);
    while (order.size() < declarations.size()) {
        std::size_t next = declarations.size();
        for (std::size_t i = 0; i < declarations.size() && next == declarations.size(); ++i)
            if (!ranked[i] && std::all_of(predecessors[i].begin(), predecessors[i].end(),
                                          [&](std::size_t before) { return ranked[before]; }))
                next = i;
        for (std::size_t i = 0; i < declarations.size() && next == declarations.size(); ++i)
            if (!ranked[i]) next = i;
        ranked[next] = true;
        order.push_back(next);
    }
    return order;
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
    // LP-1a: a node of a loop body template is lowered like any other node,
    // into its body instead of the config; the resolution stage copies the
    // body per element.
    std::map<std::string, std::size_t> loop_index;
    for (const auto &[id, expansion] : graph.loops) {
        RenderGraphLoop loop{expansion.shape, {}};
        for (const auto &branch : expansion.branches) {
            if (!branch) {
                loop.branches.emplace_back(std::nullopt);
                continue;
            }
            RenderGraphLoopBody body;
            body.definition = branch->definition;
            body.prefix = branch->prefix;
            loop.branches.emplace_back(std::move(body));
        }
        loop_index.emplace(id, lowered.loops.size());
        lowered.loops.push_back(std::move(loop));
    }
    const auto bodyOf = [&](const std::string &id) -> RenderGraphLoopBody * {
        const auto origin = graph.origins.find(id);
        if (origin == graph.origins.end() || origin->second.loop.empty() || !origin->second.loop_branch)
            return nullptr;
        auto &branch = lowered.loops.at(loop_index.at(origin->second.loop)).branches.at(*origin->second.loop_branch);
        return branch ? &*branch : nullptr;
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
    std::map<std::string, std::string> graph_pass_family;
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
        auto *const body = bodyOf(id);
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
            (body != nullptr ? body->buffers : config["buffers"]).push_back(std::move(parameters));
            continue;
        }
        if (node.at("op") == "render.target_ref") {
            // No declaration: the target, its aliasing and its history stay
            // with whoever declared it. Whether it exists is known only after
            // composition (resolveGraphDocumentAfterComposition).
            (body != nullptr ? body->target_refs : lowered.target_refs)
                .push_back({id, parameters.at("name").get<std::string>(), parameters.at("type")});
            continue;
        }
        if (node.at("op") == "render.image") {
            forbid(parameters, {"name", "kind"});
            parameters.erase("type");
            parameters["name"] = id;
            (body != nullptr ? body->render_targets : config["render_targets"]).push_back(std::move(parameters));
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
            if (history) (body != nullptr ? body->history_reads : lowered.history_reads).push_back({id, port, target});
            // LP-1a: a loop's output. Which copy writes it is known only after
            // composition, so the reader waits on that copy there, not on the
            // template node here.
            if (const auto read = endpoint.find(graphLoopOutputMarker); read != endpoint.end())
                lowered.loop_output_reads.push_back({id, read->at("loop").get<std::string>(),
                                                     read->at("port").get<std::string>(), target});
            const bool other_body = bodyOf(producer) != nullptr && bodyOf(producer) != body;
            if (!history && !other_body &&
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
            if (!graph.node_scopes.at(id).empty()) {
                if (body != nullptr) body->isolated.emplace_back(family, id);
                else config["target_planning"]["graphs"][family]["nodes"][id]["isolate"] = true;
            }
            if (body != nullptr) {
                body->passes.emplace_back(family, std::move(parameters));
                continue;
            }
            graph_pass_order.push_back(id);
            graph_pass_family.emplace(id, family);
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
        } else (body != nullptr ? body->compute_tasks : config["compute_tasks"]).push_back(std::move(parameters));
    }
    lowered.graph_node_count = graph_pass_order.size() + config["compute_tasks"].size();
    if (!lowered.loops.empty()) {
        // LP-1a: the graph's appended passes and its loops in one dependency
        // order (edges, after / before, reads of a loop's output, a body's
        // edges to an outside pass; ties in id order). The appended passes
        // are declared in it, and each loop remembers its neighbour there,
        // so its copies land where the order puts it.
        std::vector<std::string> units = graph_pass_order;
        for (const auto &loop : lowered.loops) units.push_back(loop.shape.node);
        std::sort(units.begin(), units.end());
        std::map<std::string, std::set<std::string>> predecessors;
        const auto isUnit = [&](const std::string &name) {
            return std::binary_search(units.begin(), units.end(), name);
        };
        const auto edge = [&](const std::string &before, const std::string &after) {
            if (before != after && isUnit(before) && isUnit(after)) predecessors[after].insert(before);
        };
        for (const auto &[id, edges] : graph_pass_edges) {
            for (const auto &name : edges.first)
                if (name.is_string()) edge(name.get<std::string>(), id);
            for (const auto &name : edges.second)
                if (name.is_string()) edge(id, name.get<std::string>());
        }
        for (const auto &read : lowered.loop_output_reads) {
            if (graph_pass_edges.contains(read.consumer)) edge(read.loop, read.consumer);
            for (const auto &loop : lowered.loops)
                if (read.consumer.starts_with(loop.shape.node + "/$")) edge(read.loop, loop.shape.node);
        }
        for (const auto &loop : lowered.loops) {
            for (const auto &name : loop.shape.after) edge(name, loop.shape.node);
            for (const auto &name : loop.shape.before) edge(loop.shape.node, name);
            for (const auto &body : loop.branches) {
                if (!body) continue;
                for (const auto &[family, pass] : body->passes)
                    for (const auto &name : orderNames(pass, "after")) edge(name, loop.shape.node);
            }
        }
        std::vector<std::string> ranked;
        std::set<std::string> done;
        while (ranked.size() < units.size()) {
            auto next = std::find_if(units.begin(), units.end(), [&](const std::string &unit) {
                return !done.contains(unit) &&
                       std::all_of(predecessors[unit].begin(), predecessors[unit].end(),
                                   [&](const std::string &before) { return done.contains(before); });
            });
            if (next == units.end())
                next = std::find_if(units.begin(), units.end(), [&](const std::string &unit) { return !done.contains(unit); });
            done.insert(*next);
            ranked.push_back(*next);
        }
        std::map<std::string, std::size_t> rank;
        for (std::size_t i = 0; i < ranked.size(); ++i) rank.emplace(ranked[i], i);
        if (auto families = config.find("rendering_passes"); families != config.end() && families->is_array())
            for (auto &family : *families) {
                auto &passes = family["passes"];
                std::vector<std::size_t> slots;
                std::vector<Json> graph_passes;
                for (std::size_t i = 0; i < passes.size(); ++i)
                    if (graph_pass_family.contains(passes.at(i).value("name", std::string{}))) {
                        slots.push_back(i);
                        graph_passes.push_back(passes.at(i));
                    }
                std::stable_sort(graph_passes.begin(), graph_passes.end(), [&](const Json &lhs, const Json &rhs) {
                    return rank.at(lhs.at("name").get<std::string>()) < rank.at(rhs.at("name").get<std::string>());
                });
                for (std::size_t i = 0; i < slots.size(); ++i) passes[slots[i]] = std::move(graph_passes[i]);
            }
        for (auto &loop : lowered.loops) {
            std::set<std::string> families;
            for (const auto &body : loop.branches)
                if (body)
                    for (const auto &[family, pass] : body->passes) families.insert(family);
            const auto own = rank.at(loop.shape.node);
            for (const auto &family : families) {
                std::optional<RenderGraphLoopPosition> position;
                for (std::size_t i = 0; i < ranked.size(); ++i) {
                    const auto found = graph_pass_family.find(ranked[i]);
                    if (found == graph_pass_family.end() || found->second != family) continue;
                    if (i < own) position = RenderGraphLoopPosition{true, ranked[i]};
                    else if (!position) {
                        position = RenderGraphLoopPosition{false, ranked[i]};
                        break;
                    } else break;
                }
                if (position) loop.positions.emplace(family, *position);
            }
        }
    }
    // LP-1a: each body's passes in the body's own dependency order, and what
    // each body output names.
    for (const auto &[id, expansion] : graph.loops) {
        auto &loop = lowered.loops.at(loop_index.at(id));
        for (std::size_t branch = 0; branch < loop.branches.size(); ++branch) {
            if (!loop.branches[branch]) continue;
            auto &body = *loop.branches[branch];
            std::vector<const Json *> declarations;
            for (const auto &[family, pass] : body.passes) declarations.push_back(&pass);
            std::vector<std::pair<std::string, Json>> ranked;
            for (const auto index : rankByOrderEdges(declarations)) ranked.push_back(std::move(body.passes[index]));
            body.passes = std::move(ranked);
            for (const auto &[port, endpoint] : expansion.branches.at(branch)->outputs.items()) {
                const auto producer = endpoint.at("node").get<std::string>();
                if (nodes.contains(producer)) body.outputs.emplace(port, resourceName(endpoint));
            }
        }
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

namespace {
const Json *findComposedTarget(const Json &composed, const std::string &name) {
    const auto found = composed.find("render_targets");
    if (found == composed.end() || !found->is_array()) return nullptr;
    for (const auto &target : *found)
        if (target.is_object() && target.value("name", std::string{}) == name) return &target;
    return nullptr;
}

void checkTargetReference(const Json &composed, const RenderGraphTargetReference &reference) {
    const auto *found = findComposedTarget(composed, reference.name);
    if (found == nullptr)
        throw GraphRegistryError("graph.target_ref_unknown@1",
            {{"node", reference.node}, {"name", reference.name}});
    // The declared type is only what connections are matched by; the
    // target's own declaration decides its image. The two may not say
    // different things. legacy_opaque_image states nothing to compare.
    const auto semantic = reference.type.at("semantic").get<std::string>();
    const auto declared_kind = semantic == "pelican.render.color_signal@1" ? std::string{"color"}
        : semantic == "pelican.render.depth@1" ? std::string{"depth"} : std::string{};
    if (declared_kind.empty()) return;
    const auto mismatch = [&](const char *field, const std::string &declared, const std::string &actual) {
        throw GraphRegistryError("graph.target_ref_type_mismatch@1",
            {{"node", reference.node}, {"name", reference.name}, {"field", field},
             {"declared", declared}, {"actual", actual}});
    };
    const auto actual_kind = composedTargetKind(*found);
    if (!actual_kind.empty() && actual_kind != declared_kind)
        mismatch("kind", declared_kind, actual_kind);
    const auto &arguments = reference.type.value("arguments", Json::object());
    const auto declared_topology = arguments.value("topology", std::string{"two_d"});
    const auto actual_topology = composedTargetTopology(*found);
    if (declared_topology != actual_topology)
        mismatch("topology", declared_topology, actual_topology);
}

// A history read of a target that keeps none would read nothing. The runtime
// refuses it too, but only by text and only after every pass is compiled; the
// graph names the node and port here.
void checkHistoryRead(const Json &composed, const RenderGraphHistoryRead &read) {
    const auto *found = findComposedTarget(composed, read.target);
    if (found != nullptr && !found->value("history", false))
        throw GraphRegistryError("graph.history_unavailable@1",
            {{"node", read.node}, {"port", read.port}, {"target", read.target}});
}
}

RenderGraphLoopResolution resolveGraphDocumentAfterComposition(Json &composed, const LoweredRenderGraphDocument &lowered) {
    for (const auto &reference : lowered.target_refs) checkTargetReference(composed, reference);
    for (const auto &read : lowered.history_reads) checkHistoryRead(composed, read);
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
    // LP-1a: graph.loop expansion. LP-0d (pass_replacements, variant
    // removals) continues after it.
    return expandRenderGraphLoops(composed, lowered);
}

namespace {
// --- LP-1a: the expand stage.

// Every string in a declaration that starts with `from` (a body template's
// prefix) is renamed to start with `to`. Only names the lowering made start
// with `<loop>/$<branch>/`: an id is a slash-free name, so neither an authored
// node nor another scope's expansion can spell it.
void renameTemplateStrings(Json &value, const std::string &from, const std::string &to) {
    if (value.is_string()) {
        auto &text = value.get_ref<std::string &>();
        if (text.starts_with(from)) text = to + text.substr(from.size());
        return;
    }
    if (value.is_array() || value.is_object())
        for (auto &child : value) renameTemplateStrings(child, from, to);
}

std::string renamedTemplateName(const std::string &name, const std::string &from, const std::string &to) {
    return name.starts_with(from) ? to + name.substr(from.size()) : name;
}

// Every string equal to `from` or `from@history` becomes `to` / `to@history`.
void repointResource(Json &value, const std::string &from, const std::string &to) {
    if (value.is_string()) {
        auto &text = value.get_ref<std::string &>();
        if (text == from) text = to;
        else if (text == from + "@history") text = to + "@history";
        return;
    }
    if (value.is_array() || value.is_object())
        for (auto &child : value) repointResource(child, from, to);
}

std::string resourceBase(const std::string &resource) {
    constexpr std::string_view history = "@history";
    return resource.ends_with(history) ? resource.substr(0, resource.size() - history.size()) : resource;
}

// The resources a lowered pass or task names, read or written.
std::set<std::string> declarationResources(const Json &declaration) {
    std::set<std::string> resources;
    const auto add = [&](const Json &name) {
        if (name.is_string()) resources.insert(resourceBase(name.get<std::string>()));
        else if (name.is_object() && name.contains("target") && name.at("target").is_string())
            resources.insert(resourceBase(name.at("target").get<std::string>()));
    };
    for (const auto *key : {"input", "reads", "writes"})
        if (const auto found = declaration.find(key); found != declaration.end() && found->is_array())
            for (const auto &name : *found) add(name);
    if (const auto output = declaration.find("output"); output != declaration.end() && output->is_object()) {
        if (const auto colors = output->find("color"); colors != output->end() && colors->is_array())
            for (const auto &name : *colors) add(name);
        if (const auto depth = output->find("depth"); depth != output->end()) add(*depth);
    }
    if (const auto ports = declaration.find("resource_ports"); ports != declaration.end() && ports->is_object())
        for (const auto &[port, binding] : ports->items()) {
            (void)port;
            if (binding.is_object() && binding.contains("resource")) add(binding.at("resource"));
        }
    if (const auto draw = declaration.find("draw"); draw != declaration.end() && draw->is_object())
        if (const auto index = draw->find("index_buffer"); index != draw->end()) add(*index);
    return resources;
}

// The resources a lowered pass or task writes.
std::set<std::string> declarationWrites(const Json &declaration) {
    std::set<std::string> resources;
    if (const auto writes = declaration.find("writes"); writes != declaration.end() && writes->is_array())
        for (const auto &name : *writes)
            if (name.is_string()) resources.insert(name.get<std::string>());
    if (const auto output = declaration.find("output"); output != declaration.end() && output->is_object()) {
        const auto add = [&](const Json &name) {
            if (name.is_string()) resources.insert(name.get<std::string>());
            else if (name.is_object() && name.contains("target") && name.at("target").is_string())
                resources.insert(name.at("target").get<std::string>());
        };
        if (const auto colors = output->find("color"); colors != output->end() && colors->is_array())
            for (const auto &name : *colors) add(name);
        if (const auto depth = output->find("depth"); depth != output->end()) add(*depth);
    }
    return resources;
}

void appendOrder(Json &declaration, const char *key, const std::string &name) {
    auto &list = declaration[key];
    if (list.is_null()) list = Json::array();
    if (std::find(list.begin(), list.end(), name) == list.end()) list.push_back(name);
}

// One element's copy of its branch body.
struct LoopCopy {
    GraphLoopElement element;
    // `<loop>/<element>/`.
    std::string prefix;
    std::vector<std::pair<std::string, Json>> passes;
    Json compute_tasks = Json::array();
    Json render_targets = Json::array();
    Json buffers = Json::array();
    // Reads or writes a resource the body does not own.
    bool external = false;
};

struct LoopPlan {
    const RenderGraphLoop *loop = nullptr;
    std::vector<GraphLoopElement> elements;
    std::vector<LoopCopy> copies;
};

// Where a pass or task of the composed config is: the family of a pass, or
// the task list.
struct ComposedDeclaration {
    Json *declaration = nullptr;
    // Empty for a compute task.
    std::string family;
};

std::map<std::string, ComposedDeclaration> composedDeclarations(Json &composed) {
    std::map<std::string, ComposedDeclaration> declarations;
    if (const auto families = composed.find("rendering_passes"); families != composed.end() && families->is_array())
        for (auto &family : *families) {
            const auto name = family.value("name", std::string{});
            if (!family.contains("passes") || !family.at("passes").is_array()) continue;
            for (auto &pass : family.at("passes"))
                if (pass.is_object() && pass.contains("name") && pass.at("name").is_string())
                    declarations.emplace(pass.at("name").get<std::string>(), ComposedDeclaration{&pass, name});
        }
    if (const auto tasks = composed.find("compute_tasks"); tasks != composed.end() && tasks->is_array())
        for (auto &task : *tasks)
            if (task.is_object() && task.contains("name") && task.at("name").is_string())
                declarations.emplace(task.at("name").get<std::string>(), ComposedDeclaration{&task, {}});
    return declarations;
}

// Whether a pass or task in `family` ("" for a task) may name `target` in its
// after / before. A pass family is its own frame graph and every graph holds
// every compute task, so a pass may name a pass of its own family or a task,
// and a task only a task.
bool mayOrderAgainst(const std::string &family, const std::string &target_family, bool target_is_task) {
    return target_is_task || (!family.empty() && family == target_family);
}

// The loops in the order their expansion has to run: a loop whose body reads
// another loop's output, or which is ordered after it, comes later. Ties and
// cycles keep the id order.
std::vector<std::size_t> loopDependencyOrder(const LoweredRenderGraphDocument &lowered) {
    const auto &loops = lowered.loops;
    std::map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < loops.size(); ++i) index.emplace(loops[i].shape.node, i);
    std::vector<std::set<std::size_t>> predecessors(loops.size());
    for (const auto &read : lowered.loop_output_reads) {
        const auto producer = index.find(read.loop);
        if (producer == index.end()) continue;
        for (std::size_t i = 0; i < loops.size(); ++i)
            if (read.consumer.starts_with(loops[i].shape.node + "/$") && i != producer->second)
                predecessors[i].insert(producer->second);
    }
    for (std::size_t i = 0; i < loops.size(); ++i) {
        for (const auto &name : loops[i].shape.after)
            if (const auto found = index.find(name); found != index.end() && found->second != i)
                predecessors[i].insert(found->second);
        for (const auto &name : loops[i].shape.before)
            if (const auto found = index.find(name); found != index.end() && found->second != i)
                predecessors[found->second].insert(i);
    }
    std::vector<std::size_t> order;
    std::vector<bool> done(loops.size(), false);
    while (order.size() < loops.size()) {
        std::size_t next = loops.size();
        for (std::size_t i = 0; i < loops.size() && next == loops.size(); ++i)
            if (!done[i] && std::all_of(predecessors[i].begin(), predecessors[i].end(),
                                        [&](std::size_t before) { return done[before]; }))
                next = i;
        for (std::size_t i = 0; i < loops.size() && next == loops.size(); ++i)
            if (!done[i]) next = i;
        done[next] = true;
        order.push_back(next);
    }
    return order;
}

// Copy one element's body (step 2). LP-1b evaluates the element's `$expr` /
// `$format` values, carry and per-element inputs here, on the copy, with the
// element, its index and the count.
LoopCopy copyLoopBody(const Json &composed, const std::string &loop, const RenderGraphLoopBody &body,
                      const GraphLoopElement &element) {
    LoopCopy copy;
    copy.element = element;
    copy.prefix = loop + "/" + std::to_string(element.element) + "/";
    const auto &from = body.prefix;
    const auto &to = copy.prefix;
    for (const auto &[family, pass] : body.passes) {
        auto renamed = pass;
        renameTemplateStrings(renamed, from, to);
        copy.passes.emplace_back(family, std::move(renamed));
    }
    for (const auto &[list, source] : {std::pair{&copy.compute_tasks, &body.compute_tasks},
                                       std::pair{&copy.render_targets, &body.render_targets},
                                       std::pair{&copy.buffers, &body.buffers}})
        for (const auto &declaration : *source) {
            auto renamed = declaration;
            renameTemplateStrings(renamed, from, to);
            list->push_back(std::move(renamed));
        }
    // The copy's references and history reads are checked under its names,
    // which are the names Studio sees.
    for (const auto &reference : body.target_refs)
        checkTargetReference(composed, {renamedTemplateName(reference.node, from, to), reference.name, reference.type});
    for (const auto &read : body.history_reads)
        checkHistoryRead(composed, {renamedTemplateName(read.node, from, to), read.port,
                                    renamedTemplateName(read.target, from, to)});
    std::set<std::string> owned;
    for (const auto *list : {&copy.render_targets, &copy.buffers})
        for (const auto &declaration : *list) owned.insert(declaration.at("name").get<std::string>());
    const auto touchesOutside = [&](const Json &declaration) {
        const auto resources = declarationResources(declaration);
        return std::any_of(resources.begin(), resources.end(),
                           [&](const std::string &resource) { return !owned.contains(resource); });
    };
    for (const auto &[family, pass] : copy.passes) copy.external = copy.external || touchesOutside(pass);
    for (const auto &task : copy.compute_tasks) copy.external = copy.external || touchesOutside(task);
    return copy;
}

// Step 3: `later` waits on `earlier`, where a pass may wait on a pass of its
// own family or a task, and a task on a task.
void chainCopies(const LoopCopy &earlier, LoopCopy &later) {
    for (auto &[family, pass] : later.passes) {
        for (const auto &[earlier_family, earlier_pass] : earlier.passes)
            if (earlier_family == family) appendOrder(pass, "after", earlier_pass.at("name").get<std::string>());
        for (const auto &task : earlier.compute_tasks) appendOrder(pass, "after", task.at("name").get<std::string>());
    }
    for (auto &task : later.compute_tasks)
        for (const auto &earlier_task : earlier.compute_tasks)
            appendOrder(task, "after", earlier_task.at("name").get<std::string>());
}

// Insert a run into a list of declarations right before the first one that
// depends on the loop, or at the end.
void insertBeforeDependent(Json &list, std::vector<Json> run, const std::function<bool(const Json &)> &depends) {
    auto position = list.size();
    for (std::size_t i = 0; i < list.size(); ++i)
        if (depends(list.at(i))) {
            position = i;
            break;
        }
    for (std::size_t i = 0; i < run.size(); ++i)
        list.insert(list.begin() + static_cast<Json::difference_type>(position + i), std::move(run[i]));
}
} // namespace

RenderGraphLoopResolution expandRenderGraphLoops(Json &composed, const LoweredRenderGraphDocument &lowered) {
    RenderGraphLoopResolution resolution;
    if (lowered.loops.empty()) return resolution;
    std::set<std::string> loop_ids;
    for (const auto &loop : lowered.loops) loop_ids.insert(loop.shape.node);
    // Who reads each loop's output: a graph pass or task by name, or a node of
    // another loop's body template (copied per element below).
    std::map<std::string, std::vector<const RenderGraphLoopOutputRead *>> reads_by_loop;
    for (const auto &read : lowered.loop_output_reads) reads_by_loop[read.loop].push_back(&read);
    // The copies of each template name, once made.
    std::map<std::string, std::vector<std::string>> copied_names;
    const auto consumerNames = [&](const std::string &consumer) {
        const auto found = copied_names.find(consumer);
        return found != copied_names.end() ? found->second : std::vector<std::string>{consumer};
    };

    // Step 1: the elements, and the limits.
    std::vector<LoopPlan> plans(lowered.loops.size());
    std::size_t graph_nodes = lowered.graph_node_count;
    for (std::size_t i = 0; i < lowered.loops.size(); ++i) {
        const auto &loop = lowered.loops[i];
        auto &plan = plans[i];
        plan.loop = &loop;
        // LP-1b: a count written as an expression is evaluated here (with the
        // composed config's constants); LP-Vb: a view axis's length.
        plan.elements = graphLoopElements(loop.shape, loop.shape.range_count);
        for (const auto &element : plan.elements) {
            const auto &body = loop.branches.at(element.branch);
            if (!body) continue;
            graph_nodes += body->passes.size() + body->compute_tasks.size();
            if (graph_nodes > graphLoopGraphNodeLimit)
                throw GraphRegistryError("graph.loop_expansion_limit@1",
                    {{"node", loop.shape.node}, {"count", graph_nodes}, {"limit", graphLoopGraphNodeLimit}});
        }
        RenderGraphLoopReport report{loop.shape.node, graphLoopTierName(GraphLoopTier::expand),
                                     plan.elements.size(), {}, {}};
        std::set<std::size_t> taken;
        for (const auto &element : plan.elements) {
            report.elements.push_back({element.element, element.index, element.branch});
            taken.insert(element.branch);
        }
        for (std::size_t branch = 0; branch < loop.branches.size(); ++branch)
            if (!taken.contains(branch)) report.unmatched_branches.push_back(branch);
        resolution.loops.push_back(std::move(report));
    }

    for (const auto index : loopDependencyOrder(lowered)) {
        auto &plan = plans[index];
        const auto &loop = *plan.loop;
        const auto &id = loop.shape.node;
        // Step 2: a copy per element that has a body.
        for (const auto &element : plan.elements) {
            const auto &body = loop.branches.at(element.branch);
            if (!body) continue;
            plan.copies.push_back(copyLoopBody(composed, id, *body, element));
            auto &copy = plan.copies.back();
            const auto origin = [&](const Json &declaration) {
                const auto name = declaration.at("name").get<std::string>();
                return std::pair{name, RenderLoopOrigin{id, element.element, name.substr(copy.prefix.size())}};
            };
            for (const auto &[family, pass] : copy.passes) resolution.passes.insert(origin(pass));
            for (const auto &task : copy.compute_tasks) resolution.passes.insert(origin(task));
            for (const auto &target : copy.render_targets) resolution.resources.insert(origin(target));
            for (const auto &buffer : copy.buffers) resolution.resources.insert(origin(buffer));
            const auto record = [&](const Json &declaration) {
                const auto name = declaration.at("name").get<std::string>();
                copied_names[body->prefix + name.substr(copy.prefix.size())].push_back(name);
            };
            for (const auto &[family, pass] : copy.passes) record(pass);
            for (const auto &task : copy.compute_tasks) record(task);
            for (const auto &[family, pass] : body->isolated)
                composed["target_planning"]["graphs"][family]["nodes"]
                        [renamedTemplateName(pass, body->prefix, copy.prefix)]["isolate"] = true;
        }
        // Step 3: elements that touch an outside resource run in element order.
        LoopCopy *previous = nullptr;
        for (auto &copy : plan.copies) {
            if (!copy.external) continue;
            if (previous != nullptr) chainCopies(*previous, copy);
            previous = &copy;
        }
        // Step 4: the loop's own after / before reach every copy.
        auto declarations = composedDeclarations(composed);
        const auto orderAgainst = [&](Json &declaration, const std::string &family, const char *key,
                                      const std::string &name) {
            const auto found = declarations.find(name);
            // A loop id or a name nothing declares: kept as written; the
            // loop ids are expanded below and the frame planner names the rest.
            if (loop_ids.contains(name) || found == declarations.end()) {
                if (!family.empty() || loop_ids.contains(name)) appendOrder(declaration, key, name);
                return;
            }
            if (mayOrderAgainst(family, found->second.family, found->second.family.empty()))
                appendOrder(declaration, key, name);
        };
        for (auto &copy : plan.copies) {
            for (auto &[family, pass] : copy.passes) {
                for (const auto &name : loop.shape.after) orderAgainst(pass, family, "after", name);
                for (const auto &name : loop.shape.before) orderAgainst(pass, family, "before", name);
            }
            for (auto &task : copy.compute_tasks) {
                for (const auto &name : loop.shape.after) orderAgainst(task, {}, "after", name);
                for (const auto &name : loop.shape.before) orderAgainst(task, {}, "before", name);
            }
        }
        // Step 5: the run, per family, in its dependency order with ties in
        // name order -- so the element order is the explicit edges', never the
        // spelling's (`<loop>/10/x` sorts before `<loop>/9/x`).
        std::map<std::string, std::vector<Json>> runs;
        std::vector<Json> tasks;
        auto &targets = composed["render_targets"];
        if (targets.is_null()) targets = Json::array();
        auto &buffers = composed["buffers"];
        if (buffers.is_null()) buffers = Json::array();
        for (auto &copy : plan.copies) {
            for (auto &[family, pass] : copy.passes) runs[family].push_back(std::move(pass));
            for (auto &task : copy.compute_tasks) tasks.push_back(std::move(task));
            for (auto &target : copy.render_targets) {
                if (findComposedTarget(composed, target.at("name").get<std::string>()) != nullptr)
                    throw std::runtime_error("graph.loop render target collides: " + target.at("name").get<std::string>());
                targets.push_back(std::move(target));
            }
            for (auto &buffer : copy.buffers) buffers.push_back(std::move(buffer));
        }
        const auto ranked = [](std::vector<Json> run) {
            std::sort(run.begin(), run.end(), [](const Json &lhs, const Json &rhs) {
                return lhs.at("name").get<std::string>() < rhs.at("name").get<std::string>();
            });
            std::vector<const Json *> declarations_in_run;
            for (const auto &declaration : run) declarations_in_run.push_back(&declaration);
            std::vector<Json> result;
            for (const auto position : rankByOrderEdges(declarations_in_run)) result.push_back(run[position]);
            return result;
        };
        // What waits on this loop: an after naming it or a reader of its
        // output, or a name the loop is before. The run is declared ahead of
        // the first of these so the canonical order agrees with the edges.
        std::set<std::string> dependents(loop.shape.before.begin(), loop.shape.before.end());
        for (const auto *read : reads_by_loop[id])
            for (const auto &name : consumerNames(read->consumer)) dependents.insert(name);
        const auto depends = [&](const Json &declaration) {
            if (!declaration.is_object()) return false;
            if (dependents.contains(declaration.value("name", std::string{}))) return true;
            const auto after = orderNames(declaration, "after");
            return std::find(after.begin(), after.end(), id) != after.end();
        };
        if (!tasks.empty()) {
            auto &list = composed["compute_tasks"];
            if (list.is_null()) list = Json::array();
            insertBeforeDependent(list, ranked(std::move(tasks)), depends);
        }
        for (auto &[family, run] : runs) {
            auto passes = ranked(std::move(run));
            if (loop.shape.placement) {
                placeRenderGraphPassRun(composed, RenderGraphPassRun{id, family, loop.shape.placement->after,
                                                                     loop.shape.placement->anchor, std::move(passes)});
                continue;
            }
            auto &families = composed["rendering_passes"];
            if (families.is_null()) families = Json::array();
            auto found = std::find_if(families.begin(), families.end(),
                                      [&](const Json &candidate) { return candidate.value("name", std::string{}) == family; });
            if (found == families.end()) {
                families.push_back({{"name", family}, {"passes", Json::array()}});
                found = std::prev(families.end());
            }
            auto &list = (*found)["passes"];
            // Next to the graph pass the lowering's order puts it by; with
            // none, before the first pass that waits on it, or at the end.
            if (const auto position = loop.positions.find(family); position != loop.positions.end()) {
                const auto neighbour = std::find_if(list.begin(), list.end(), [&](const Json &pass) {
                    return pass.value("name", std::string{}) == position->second.pass;
                });
                if (neighbour != list.end()) {
                    auto index = static_cast<std::size_t>(neighbour - list.begin()) + (position->second.after ? 1 : 0);
                    for (auto &pass : passes)
                        list.insert(list.begin() + static_cast<Json::difference_type>(index++), std::move(pass));
                    continue;
                }
            }
            insertBeforeDependent(list, std::move(passes), depends);
        }
    }

    // `<loop>` in any after / before is every copy of the loop the holder may
    // be ordered against; a loop with no copy leaves the list.
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> expanded; // loop -> (name, family)
    {
        auto declarations = composedDeclarations(composed);
        for (const auto &plan : plans)
            for (const auto &copy : plan.copies) {
                auto &names = expanded[plan.loop->shape.node];
                for (const auto &[name, origin] : resolution.passes)
                    if (name.starts_with(copy.prefix)) names.emplace_back(name, declarations.at(name).family);
            }
        for (auto &[name, entry] : declarations) {
            for (const auto *key : {"after", "before"}) {
                const auto list = entry.declaration->find(key);
                if (list == entry.declaration->end() || !list->is_array()) continue;
                Json rewritten = Json::array();
                bool changed = false;
                for (const auto &item : *list) {
                    if (!item.is_string() || !loop_ids.contains(item.get<std::string>())) {
                        rewritten.push_back(item);
                        continue;
                    }
                    changed = true;
                    for (const auto &[copy_name, copy_family] : expanded[item.get<std::string>()])
                        if (mayOrderAgainst(entry.family, copy_family, copy_family.empty()) &&
                            std::find(rewritten.begin(), rewritten.end(), copy_name) == rewritten.end())
                            rewritten.push_back(copy_name);
                }
                if (changed) *list = std::move(rewritten);
            }
        }
    }

    // Every reader of a loop output reads the last element that has a body,
    // and waits on what that copy writes it with.
    auto declarations = composedDeclarations(composed);
    for (const auto &read : lowered.loop_output_reads) {
        const auto plan = std::find_if(plans.begin(), plans.end(),
                                       [&](const LoopPlan &candidate) { return candidate.loop->shape.node == read.loop; });
        if (plan == plans.end()) continue;
        if (plan->copies.empty())
            throw GraphRegistryError("graph.loop_output_empty@1", {{"node", read.loop}, {"port", read.port}});
        const auto &last = plan->copies.back();
        const auto &body = *plan->loop->branches.at(last.element.branch);
        const auto output = body.outputs.find(read.port);
        if (output == body.outputs.end()) continue;
        const auto resource = renamedTemplateName(output->second, body.prefix, last.prefix);
        std::vector<std::pair<std::string, std::string>> writers; // (name, family)
        for (const auto &[name, origin] : resolution.passes) {
            if (!name.starts_with(last.prefix)) continue;
            const auto found = declarations.find(name);
            if (found == declarations.end()) continue;
            if (declarationWrites(*found->second.declaration).contains(resource))
                writers.emplace_back(name, found->second.family);
        }
        for (const auto &consumer : consumerNames(read.consumer)) {
            const auto found = declarations.find(consumer);
            if (found == declarations.end()) continue;
            auto &declaration = *found->second.declaration;
            if (read.resource != resource) repointResource(declaration, read.resource, resource);
            for (const auto &[writer, writer_family] : writers)
                if (writer != consumer && mayOrderAgainst(found->second.family, writer_family, writer_family.empty()))
                    appendOrder(declaration, "after", writer);
        }
    }
    return resolution;
}
}
