#include "rendergraphcomposition.hpp"
#include "featurecompose.hpp"
#include "graphpartition.hpp"
#include "graphloopexpression.hpp"

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

// LP-3a: one draw.* node as the step it draws (the contract has checked the
// parameters).
MaterialDrawStep drawStepFromNode(const Json &node) {
    const auto op = node.at("op").get<std::string>();
    const auto parameters = node.value("parameters", Json::object());
    if (op == "draw.material") return MaterialDrawStep::material();
    if (op == "draw.variant") return MaterialDrawStep::named(parameters.at("variant").get<std::string>());
    MaterialDrawRenderStateOverride state;
    const auto &render_state = parameters.value("render_state", Json::object());
    if (const auto blend = render_state.value("blend", std::string{}); !blend.empty())
        state.blend = blend == "opaque" ? SurfaceBlendMode::opaque
            : blend == "blend" ? SurfaceBlendMode::blend : SurfaceBlendMode::additive;
    if (const auto cull = render_state.value("cull", std::string{}); !cull.empty())
        state.cull = cull == "none" ? SurfaceCullMode::none
            : cull == "front" ? SurfaceCullMode::front : SurfaceCullMode::back;
    if (render_state.contains("depth_test")) state.depth_test = render_state.at("depth_test").get<bool>();
    if (render_state.contains("depth_write")) state.depth_write = render_state.at("depth_write").get<bool>();
    if (const auto compare = render_state.value("depth_compare", std::string{}); !compare.empty()) {
        static const std::map<std::string, SurfaceDepthCompare> names{
            {"never", SurfaceDepthCompare::never}, {"less", SurfaceDepthCompare::less},
            {"equal", SurfaceDepthCompare::equal}, {"less_equal", SurfaceDepthCompare::less_equal},
            {"greater", SurfaceDepthCompare::greater}, {"not_equal", SurfaceDepthCompare::not_equal},
            {"greater_equal", SurfaceDepthCompare::greater_equal}, {"always", SurfaceDepthCompare::always}};
        state.depth_compare = names.at(compare);
    }
    std::vector<std::string> defines;
    for (const auto &define : parameters.value("defines", Json::array())) defines.push_back(define.get<std::string>());
    return MaterialDrawStep::overridden(state, std::move(defines));
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

// LP-Vb: a graph render node takes its region tags as `region_tags` (the node
// contracts' name); the pass and task JSON read them from `regions`
// (parseOptionalRegionTags). Both may be written: they merge, in order, once.
void moveRegionTags(Json &parameters) {
    const auto tags = parameters.find("region_tags");
    if (tags == parameters.end()) return;
    if (!tags->is_array()) throw std::invalid_argument("render region_tags must be an array");
    auto regions = parameters.value("regions", Json::array());
    if (!regions.is_array()) throw std::invalid_argument("render regions must be an array");
    for (const auto &tag : *tags)
        if (std::find(regions.begin(), regions.end(), tag) == regions.end()) regions.push_back(tag);
    parameters.erase("region_tags");
    parameters["regions"] = std::move(regions);
}

// LP-0e: a render.shadow_depth / render.snapshot_copy node as the rendering
// section's pass of that type, named by its id. `attach(port, after)` names
// the resource on a socket's edge (adding to `after` what the pass waits for);
// `graph_pass(name)` says whether a name is a pass this graph lowers here, so
// a copy's snapshot_after among the graph's passes also orders the graph's
// own declaration of them (the frame planner reads only after / before and
// the data; snapshot_after is an execution edge).
Json lowerRenderPassKindNode(const Json &node, Json parameters,
                             const std::function<std::string(const std::string &, Json &)> &attach,
                             const std::function<bool(const std::string &)> &graph_pass) {
    const auto op = node.at("op").get<std::string>();
    moveRegionTags(parameters);
    auto after = parameters.value("after", Json::array());
    if (!after.is_array()) throw std::invalid_argument("render after must be an array");
    parameters["name"] = node.at("id");
    if (op == "render.shadow_depth") {
        parameters["type"] = "shadow_depth";
        // As the feature writes it: no color attachment.
        parameters["output"] = Json{{"color", nullptr}, {"depth", attach("depth", after)}};
    } else {
        parameters["type"] = "snapshot_copy";
        parameters["source"] = attach("source", after);
        const auto destination = attach("destination", after);
        parameters["destination"] = destination;
        // The snapshot is what the feature's copies name: their destination.
        parameters["snapshot"] = destination;
        const auto snapshot_after = parameters.at("snapshot_after").get<std::string>();
        if (graph_pass(snapshot_after) && std::find(after.begin(), after.end(), snapshot_after) == after.end())
            after.push_back(snapshot_after);
    }
    if (!after.empty()) parameters["after"] = std::move(after);
    return parameters;
}
}

LoweredRenderGraphDocument lowerRenderGraphDocumentForComposition(const Json &document,
    const std::set<std::string> &inactive_scopes, const GraphModuleRegistry &modules,
    const std::string &default_pass_family) {
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
    // LP-3a: a draw_loop expands nothing; its body's draw.* nodes become the
    // draw steps of the material pass its `draws` reach (draw_loop_passes).
    std::map<std::string, std::string> draw_loop_passes;
    const auto drawLoop = [&](const std::string &loop) {
        const auto found = graph.loops.find(loop);
        return found != graph.loops.end() && found->second.shape.set == GraphLoopSet::draw_items;
    };
    for (const auto &[id, expansion] : graph.loops) {
        if (drawLoop(id)) continue;
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
        if (origin == graph.origins.end() || origin->second.loop.empty() || !origin->second.loop_branch ||
            !loop_index.contains(origin->second.loop))
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
        bool declared = false;
    };
    std::vector<std::string> graph_pass_order;
    std::map<std::string, std::pair<Json, Json>> graph_pass_edges;
    std::map<std::string, std::string> graph_pass_family;
    std::vector<PendingPlacement> pending_placements;
    std::set<std::string> resolving;
    std::function<std::string(const Json &)> resourceName = [&](const Json &endpoint) {
        // LP-1b: a carried input, and one element of a loop's `p[]`, are
        // named by placeholders each copy (each consumer) replaces.
        if (const auto carry = endpoint.find(graphLoopCarryMarker); carry != endpoint.end())
            return graphLoopCarryResource(carry->at("loop").get<std::string>(), carry->at("input").get<std::string>());
        if (const auto read = endpoint.find(graphLoopOutputMarker);
            read != endpoint.end() && read->contains("element") && !read->at("element").is_null())
            return read->at("loop").get<std::string>() + "/$element/" + read->at("receiver").get<std::string>() + "/" +
                   read->at("input").get<std::string>();
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
    // The loop outputs a port's value comes from, however far up the chain of
    // passes it was handed down (LP-1a looked at the port's own edge only).
    // A carried input stops the walk: its first value is the loop's to read.
    std::function<void(const Json &, std::vector<Json> &)> loopReads = [&](const Json &endpoint,
                                                                            std::vector<Json> &found) {
        if (endpoint.contains(graphLoopCarryMarker)) return;
        if (const auto read = endpoint.find(graphLoopOutputMarker); read != endpoint.end()) {
            auto entry = *read;
            entry["resource"] = resourceName(endpoint);
            found.push_back(std::move(entry));
            return;
        }
        const auto id = endpoint.at("node").get<std::string>();
        const auto node = nodes.find(id);
        if (node == nodes.end()) return;
        const auto op = node->second->at("op").get<std::string>();
        if (op == "render.buffer" || op == "render.attribute" || op == "render.image" || op == "render.target_ref")
            return;
        const auto inputs = node->second->find("inputs");
        const auto port = endpoint.at("port").get<std::string>();
        if (inputs == node->second->end() || !inputs->contains(port)) return;
        loopReads(inputs->at(port), found);
    };
    const auto recordLoopReads = [&](const std::string &consumer, const Json &endpoint) {
        std::vector<Json> found;
        loopReads(endpoint, found);
        for (const auto &read : found) {
            RenderGraphLoopOutputRead entry{consumer, read.at("loop").get<std::string>(),
                                            read.at("port").get<std::string>(), read.at("resource").get<std::string>(),
                                            read.value("element", Json{}), read.value("receiver", std::string{}),
                                            read.value("input", std::string{}), read.value("receiver_loop", false)};
            const auto duplicate = std::find_if(lowered.loop_output_reads.begin(), lowered.loop_output_reads.end(),
                [&](const RenderGraphLoopOutputRead &other) {
                    return other.consumer == entry.consumer && other.resource == entry.resource;
                });
            if (duplicate == lowered.loop_output_reads.end()) lowered.loop_output_reads.push_back(std::move(entry));
        }
    };
    // A lowered graphics pass joins a loop body, waits for its anchor, or is
    // appended to its family (LP-3a: a render.material_pass the same way).
    const auto landGraphicsPass = [&](const std::string &id, Json parameters, RenderGraphLoopBody *body) {
        const auto family = parameters.value("pass_set", default_pass_family);
        parameters.erase("pass_set");
        if (!graph.node_scopes.at(id).empty()) {
            if (body != nullptr) body->isolated.emplace_back(family, id);
            else config["target_planning"]["graphs"][family]["nodes"][id]["isolate"] = true;
        }
        if (body != nullptr) {
            body->passes.emplace_back(family, std::move(parameters));
            return;
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
                // LP-3b: placed among the declared passes, before any feature.
                pending.declared = insert.value("declared", false);
            } else {
                pending.anchor = parameters.at("canonical_anchor").get<std::string>();
            }
            parameters.erase("insert");
            parameters.erase("canonical_anchor");
            pending.pass = std::move(parameters);
            pending_placements.push_back(std::move(pending));
            return;
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
    };
    for (const auto &node : graph.nodes) {
        const auto id = node.at("id").get<std::string>();
        if (inactive(id)) continue;
        // LP-0e: a render.shadow_depth / render.snapshot_copy node is the pass
        // of that type (lowerRenderPassKindNode), placed like any graph pass.
        if (const auto op = node.at("op").get<std::string>();
            op == "render.shadow_depth" || op == "render.snapshot_copy") {
            auto *const pass_body = bodyOf(id);
            auto parameters = node.at("parameters");
            // An active pass need not wait for one that no longer executes.
            for (const auto *key : {"after", "before"})
                if (const auto order = parameters.find(key); order != parameters.end() && order->is_array())
                    order->erase(std::remove_if(order->begin(), order->end(), [&](const Json &name) {
                        return name.is_string() && inactive(name.get<std::string>());
                    }), order->end());
            const auto attach = [&](const std::string &port, Json &after) {
                const auto &endpoint = node.at("inputs").at(port);
                recordLoopReads(id, endpoint);
                const auto producer = endpoint.at("node").get<std::string>();
                const auto &producer_op = nodes.at(producer)->at("op");
                const bool other_body = bodyOf(producer) != nullptr && bodyOf(producer) != pass_body;
                if (!other_body && producer_op != "render.buffer" && producer_op != "render.attribute" &&
                    producer_op != "render.image" && producer_op != "render.target_ref" &&
                    std::find(after.begin(), after.end(), producer) == after.end())
                    after.push_back(producer);
                return resourceName(endpoint);
            };
            const auto graph_pass = [&](const std::string &name) {
                const auto found = nodes.find(name);
                if (found == nodes.end() || inactive(name) || bodyOf(name) != pass_body) return false;
                const auto &producer_op = found->second->at("op");
                return producer_op != "render.buffer" && producer_op != "render.attribute" &&
                       producer_op != "render.image" && producer_op != "render.target_ref" &&
                       producer_op != "render.compute";
            };
            landGraphicsPass(id, lowerRenderPassKindNode(node, std::move(parameters), attach, graph_pass), pass_body);
            continue;
        }
        auto *const body = bodyOf(id);
        // LP-3a: a draw_loop body's step. The expander admits a draw.* node
        // nowhere else; the steps are read off the body below.
        if (const auto origin = graph.origins.find(id); node.at("op").get<std::string>().starts_with("draw.") &&
            origin != graph.origins.end() && drawLoop(origin->second.loop))
            continue;
        // A contract can be registered without a backend that runs it. Say so
        // by name instead of lowering an unknown operation as if it were a pass.
        static const std::set<std::string> lowerable{"render.buffer", "render.attribute",
            "render.image", "render.target_ref", "render.compute", "render.fullscreen", "render.raster",
            "render.material_pass"};
        if (!lowerable.contains(node.at("op").get<std::string>()))
            throw GraphRegistryError("graph.operation_unexecutable@1",
                {{"node", id}, {"op", node.at("op").get<std::string>()},
                 {"backend", "pelican.render"}});
        auto parameters = node.value("parameters", Json::object());
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
        if (node.at("op") == "render.material_pass") {
            // LP-3a: the material pass JSON of the rendering section, with
            // its attachments taken from its sockets and its name from the id.
            auto pass = parameters;
            moveRegionTags(pass);
            pass["name"] = id;
            pass["type"] = "material";
            auto after = pass.value("after", Json::array());
            Json output{{"color", Json::array()}, {"depth", nullptr}};
            const auto attach = [&](const std::string &port) {
                const auto &endpoint = node.at("inputs").at(port);
                const auto producer = endpoint.at("node").get<std::string>();
                recordLoopReads(id, endpoint);
                const auto op = nodes.at(producer)->at("op").get<std::string>();
                const bool other_body = bodyOf(producer) != nullptr && bodyOf(producer) != body;
                if (!other_body && op != "render.buffer" && op != "render.attribute" && op != "render.image" &&
                    op != "render.target_ref" && std::find(after.begin(), after.end(), producer) == after.end())
                    after.push_back(producer);
                return resourceName(endpoint);
            };
            for (const auto &port : renderMaterialPassColorPorts(parameters)) output["color"].push_back(attach(port));
            output["depth"] = attach("depth");
            pass["output"] = std::move(output);
            if (!after.empty()) pass["after"] = std::move(after);
            // Its draws: a draw_loop's, or today's drawing when unconnected.
            if (const auto draws = node.at("inputs").find("draws"); draws != node.at("inputs").end()) {
                const auto marker = draws->find(graphLoopOutputMarker);
                if (marker == draws->end() || !drawLoop(marker->value("loop", std::string{})))
                    throw std::invalid_argument("render.material_pass draws come from a graph.loop over draw_items: " + id);
                const auto loop = marker->at("loop").get<std::string>();
                draw_loop_passes.emplace(loop, id);
                // `where` is the pass's material_filter (the queue's tag
                // filter, shared by its stable id); require_all is the loop's.
                if (const auto &where = graph.loops.at(loop).shape.draw_where;
                    where && (!where->include.empty() || !where->exclude.empty())) {
                    Json filter = Json::object();
                    if (!where->include.empty()) filter["include"] = where->include;
                    if (!where->exclude.empty()) filter["exclude"] = where->exclude;
                    pass["material_filter"] = std::move(filter);
                }
            }
            landGraphicsPass(id, std::move(pass), body);
            continue;
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
        moveRegionTags(parameters);
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
        // LP-1d: images read as the pass's numbered inputs (binding "input"),
        // by input_index; the graph module checked the numbers run from 0.
        std::map<std::int64_t, std::string> numbered;
        auto after = parameters.value("after", Json::array());
        if (!after.is_array()) throw std::invalid_argument("render after must be an array");
        for (const auto &[port, declaration] : ports.items()) {
            for (const auto &[key, value] : declaration.items())
                if (key != "element" && key != "domain" && key != "access" && key != "kind" && key != "type" &&
                    key != "binding" && key != "view" && key != "sampling" && key != "subresource" &&
                    key != "history" && key != "input_index" &&
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
            recordLoopReads(id, endpoint);
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
            if (graphics && declaration.value("binding", std::string{}) == "input") {
                // No resource port: the shader reads it by its input number
                // (inputTexture, PELICAN_DECLARE_INPUT_<i>, a variadic group).
                numbered.emplace(declaration.at("input_index").get<std::int64_t>(), resource);
                continue;
            }
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
            // The numbered inputs first, in their order; then what the
            // resource ports read, which binds by port name wherever it lands.
            Json input = Json::array();
            for (const auto &[index, resource] : numbered) input.push_back(resource);
            for (const auto &resource : reads) input.push_back(resource);
            parameters["input"] = std::move(input);
            parameters["output"] = std::move(attachments);
            parameters["type"] = node.at("op") == "render.fullscreen" ? "fullscreen" : "raster";
            landGraphicsPass(id, std::move(parameters), body);
        } else (body != nullptr ? body->compute_tasks : config["compute_tasks"]).push_back(std::move(parameters));
    }
    lowered.graph_node_count = graph_pass_order.size() + config["compute_tasks"].size();
    lowered.graph_declarations.insert(graph_pass_order.begin(), graph_pass_order.end());
    for (const auto &task : config["compute_tasks"]) lowered.graph_declarations.insert(task.at("name").get<std::string>());
    // LP-1b: the first value of each carried input that is a render resource
    // (a scalar carry is the values partition's).
    for (const auto &[id, expansion] : graph.loops) {
        if (drawLoop(id)) continue;
        auto &loop = lowered.loops.at(loop_index.at(id));
        for (const auto &pair : expansion.shape.carry) {
            const auto found = expansion.inputs.find(pair.to);
            if (found == expansion.inputs.end()) continue;
            auto endpoint = *found;
            endpoint.erase(graphLoopCarryMarker);
            if (!nodes.contains(endpoint.at("node").get<std::string>())) continue;
            RenderGraphLoopCarriedInput input{resourceName(endpoint), {}, {}};
            std::vector<Json> reads;
            loopReads(endpoint, reads);
            if (!reads.empty()) {
                input.loop = reads.front().at("loop").get<std::string>();
                input.port = reads.front().at("port").get<std::string>();
            }
            loop.carried.emplace(pair.to, std::move(input));
        }
    }
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
            for (const auto &[to, input] : loop.carried)
                if (!input.loop.empty()) edge(input.loop, loop.shape.node);
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
        if (drawLoop(id)) continue;
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
    // LP-3a: each draw_loop's branch table. A branch's steps are its body's
    // draw.* chain: from the body's `draws` output back along `after` edges,
    // then first to last.
    for (const auto &[id, expansion] : graph.loops) {
        if (!drawLoop(id)) continue;
        const auto pass = draw_loop_passes.find(id);
        // The material pass is in an inactive instance: nothing draws.
        if (pass == draw_loop_passes.end()) continue;
        GraphDrawLoop loop;
        loop.loop = id;
        loop.pass = pass->second;
        loop.contract = *materialPassContractFromName(expansion.shape.draw_contract);
        loop.where = expansion.shape.draw_where;
        for (std::size_t branch = 0; branch < expansion.branches.size(); ++branch) {
            GraphDrawLoop::Branch entry{expansion.shape.branches.at(branch).draw_when, std::nullopt};
            if (const auto &body = expansion.branches[branch]) {
                MaterialDrawStepList steps;
                auto endpoint = body->outputs.contains("draws") ? body->outputs.at("draws") : Json{};
                std::set<std::string> seen;
                while (endpoint.is_object() && endpoint.contains("node")) {
                    const auto step_id = endpoint.at("node").get<std::string>();
                    const auto found = nodes.find(step_id);
                    if (found == nodes.end() || !seen.insert(step_id).second) break;
                    steps.push_back(drawStepFromNode(*found->second));
                    const auto inputs = found->second->find("inputs");
                    endpoint = inputs != found->second->end() && inputs->contains("after") ? inputs->at("after") : Json{};
                }
                std::reverse(steps.begin(), steps.end());
                entry.steps = std::move(steps);
            }
            loop.branches.push_back(std::move(entry));
        }
        lowered.draw_loops.push_back(std::move(loop));
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
                        candidate.anchor == pending.anchor && candidate.declared == pending.declared;
                });
            if (run == lowered.placed_runs.end())
                lowered.placed_runs.push_back({pending.node, pending.family, pending.after, pending.anchor,
                                               {std::move(pending.pass)}, pending.declared});
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
    // A layer count bound from a feature parameter may be a signed integer
    // (an edit or an inline document writes one); it counts the same (LP-0e).
    if (layers != target.end() && layers->is_number_integer() && layers->get<std::int64_t>() > 1)
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

namespace {
// LP-1b: a refusal that names one element's copy of a body node
// (`<loop>/<element>/<node>`) says where it came from, in the shape the frame
// plan's `loop_origin` has.
std::optional<RenderLoopOrigin> loopCopyOrigin(const std::string &name, const std::vector<RenderGraphLoop> &loops) {
    for (const auto &loop : loops) {
        const auto &id = loop.shape.node;
        if (name.size() <= id.size() + 1 || !name.starts_with(id) || name[id.size()] != '/') continue;
        const auto rest = name.substr(id.size() + 1);
        const auto slash = rest.find('/');
        if (slash == std::string::npos || slash == 0 || slash > 18 || slash + 1 == rest.size()) continue;
        const auto digits = rest.substr(0, slash);
        if (!std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
        return RenderLoopOrigin{id, std::stoll(digits), rest.substr(slash + 1)};
    }
    return std::nullopt;
}
} // namespace

namespace {
// LP-0b: place each run once its anchor exists. A run may be anchored at a
// pass another run places, so place what can be placed until nothing moves;
// the first run left is refused by name.
void placeRenderGraphPassRunList(Json &composed, std::vector<const RenderGraphPassRun *> pending) {
    while (!pending.empty()) {
        const auto ready = std::find_if(pending.begin(), pending.end(), [&](const RenderGraphPassRun *run) {
            return !findRenderPassAnchorMatches(composed, run->anchor).empty();
        });
        const auto next = ready != pending.end() ? ready : pending.begin();
        placeRenderGraphPassRun(composed, **next);
        pending.erase(next);
    }
}
} // namespace

void placeRenderGraphPassRuns(Json &composed, const LoweredRenderGraphDocument &lowered) {
    std::vector<const RenderGraphPassRun *> pending;
    for (const auto &run : lowered.placed_runs)
        if (!run.declared) pending.push_back(&run);
    placeRenderGraphPassRunList(composed, std::move(pending));
}

void placeDeclaredRenderGraphPassRuns(Json &config, LoweredRenderGraphDocument &lowered) {
    std::vector<const RenderGraphPassRun *> pending;
    for (const auto &run : lowered.placed_runs)
        if (run.declared) pending.push_back(&run);
    if (pending.empty()) return;
    placeRenderGraphPassRunList(config, std::move(pending));
    std::erase_if(lowered.placed_runs, [](const RenderGraphPassRun &run) { return run.declared; });
}

namespace {
void resolveDrawLoops(Json &composed, const LoweredRenderGraphDocument &lowered, RenderGraphLoopResolution &resolution);
} // namespace

RenderGraphLoopResolution resolveGraphDocumentAfterComposition(Json &composed, const LoweredRenderGraphDocument &lowered,
                                                               const RenderGraphFeatureContext &features) {
    for (const auto &reference : lowered.target_refs) checkTargetReference(composed, reference);
    for (const auto &read : lowered.history_reads) checkHistoryRead(composed, read);
    // LP-1a: graph.loop expansion. LP-0d (pass_replacements) continues after
    // it; the variant's removals ran before the runs were placed
    // (withoutExcludedGraphDependents).
    //
    // LP-1b: whatever refuses a node of one element's copy here -- a
    // render.target_ref (LP-0a), a history read, an expression -- names the
    // copy (`<loop>/<element>/<node>`); the refusal adds where the copy came
    // from, as `loop_origin` {loop, element, node}, in this one place rather
    // than at each check.
    RenderGraphLoopResolution resolution;
    try {
        resolution = expandRenderGraphLoops(composed, lowered, features);
        resolveDrawLoops(composed, lowered, resolution);
    } catch (const GraphRegistryError &error) {
        const auto &detail = error.detail();
        const auto node = detail.is_object() ? detail.find("node") : detail.end();
        if (node == detail.end() || !node->is_string() || detail.contains("loop_origin")) throw;
        const auto origin = loopCopyOrigin(node->get<std::string>(), lowered.loops);
        if (!origin) throw;
        auto extended = detail;
        extended["loop_origin"] = Json{{"loop", origin->loop}, {"element", origin->element}, {"node", origin->node}};
        throw GraphRegistryError(error.code(), std::move(extended));
    }
    // LP-0d: the composer left out the replacements whose node the variant
    // made inactive; the feature keeps its own pass in that variant.
    applyRenderFeaturePassReplacements(composed, lowered, resolution.passes, features);
    return resolution;
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
    // LP-0e: a snapshot copy names its two images in fields of their own.
    if (declaration.value("type", std::string{}) == "snapshot_copy")
        for (const auto *key : {"source", "destination"})
            if (const auto found = declaration.find(key); found != declaration.end()) add(*found);
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
    // LP-0e: a snapshot copy writes its destination.
    if (declaration.value("type", std::string{}) == "snapshot_copy")
        if (const auto destination = declaration.find("destination");
            destination != declaration.end() && destination->is_string())
            resources.insert(destination->get<std::string>());
    return resources;
}

void appendOrder(Json &declaration, const char *key, const std::string &name) {
    auto &list = declaration[key];
    if (list.is_null()) list = Json::array();
    if (std::find(list.begin(), list.end(), name) == list.end()) list.push_back(name);
}

// A pass or task by name, and its family ("" for a task).
using NamedDeclaration = std::pair<std::string, std::string>;

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
    // LP-1b: body output port -> the resource this element's value is (a
    // carried value resolved), and what each resource is written by.
    std::map<std::string, std::string> outputs;
    std::map<std::string, std::vector<NamedDeclaration>> writers;
};

struct LoopPlan {
    const RenderGraphLoop *loop = nullptr;
    std::vector<GraphLoopElement> elements;
    std::vector<LoopCopy> copies;
    // LP-1b: the first value of each carried input (by `to`), and what wrote
    // it when another loop did.
    std::map<std::string, std::string> carried_first;
    std::map<std::string, std::vector<NamedDeclaration>> carried_first_writers;
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
// another loop's output (LP-1b: or whose carried input starts from it), or
// which is ordered after it, comes later. Ties and cycles keep the id order.
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
        for (const auto &[to, input] : loops[i].carried)
            if (const auto found = index.find(input.loop); found != index.end() && found->second != i)
                predecessors[i].insert(found->second);
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

// LP-1b: `mip_levels_of` / `layers_of` of the composed targets. A target the
// runtime widens -- one a shadow_depth pass draws, whose layers follow the
// lights -- or whose mip chain follows its extent is not known here.
bool drawnByShadowDepth(const Json &composed, const std::string &target) {
    const auto families = composed.find("rendering_passes");
    if (families == composed.end() || !families->is_array()) return false;
    for (const auto &family : *families)
        for (const auto &pass : family.value("passes", Json::array())) {
            if (!pass.is_object() || pass.value("type", std::string{}) != "shadow_depth") continue;
            const auto output = pass.find("output");
            if (output != pass.end() && declarationWrites(Json{{"output", *output}}).contains(target)) return true;
        }
    return false;
}

GraphLoopConstants composedConstants(const Json &composed) {
    return [&composed](std::string_view function, const std::string &target) -> std::int64_t {
        const auto *found = findComposedTarget(composed, target);
        if (found == nullptr)
            throw GraphLoopExpressionError(graphLoopExpressionNotStatic, "no composed target " + target);
        if (drawnByShadowDepth(composed, target))
            throw GraphLoopExpressionError(graphLoopExpressionNotStatic, target + " is widened at run time");
        const bool mips = function == "mip_levels_of";
        const auto value = found->find(mips ? "mip_levels" : "layers");
        if (value == found->end())
            return mips ? 1 : (found->value("dimension", std::string{"2d"}) == "cube" ? 6 : 1);
        if (!value->is_number_integer())
            throw GraphLoopExpressionError(graphLoopExpressionNotStatic,
                                           target + "'s " + std::string{function} + " follows its extent");
        return value->get<std::int64_t>();
    };
}

// LP-1b: every `$expr` / `$format` in one copy's declaration, evaluated for
// its element. `extent_scale` is the one real place; a `$format` is a string.
// A refusal names the declaration (the copy) and the authored pointer.
void evaluateExpressions(Json &value, const std::string &owner, const GraphLoopExpressionValues &values,
                         const GraphLoopConstants &constants, bool real_place = false) {
    if (isGraphLoopExpression(value)) {
        const auto kind = isGraphLoopFormat(value) ? GraphLoopExpressionKind::format
            : real_place ? GraphLoopExpressionKind::real : GraphLoopExpressionKind::integer;
        const auto pointer = value.value("$pointer", std::string{});
        try {
            value = evaluateGraphLoopExpression(value, kind, values, constants);
        } catch (const GraphLoopExpressionError &error) {
            throw graphLoopExpressionInvalid(owner, pointer, error.reason());
        }
        return;
    }
    if (value.is_object()) {
        for (auto member = value.begin(); member != value.end(); ++member)
            evaluateExpressions(member.value(), owner, values, constants, member.key() == "extent_scale");
    } else if (value.is_array()) {
        for (auto &child : value) evaluateExpressions(child, owner, values, constants);
    }
}

// Copy one element's body (step 2), with the element's values (LP-1b): its
// expressions evaluated, and each resource `substitutions` names -- a carried
// input's placeholder, another loop's output the body reads -- the value this
// element receives.
LoopCopy copyLoopBody(const Json &composed, const std::string &loop, const RenderGraphLoopBody &body,
                      const GraphLoopElement &element, std::int64_t count, const GraphLoopConstants &constants,
                      const std::map<std::string, std::string> &substitutions) {
    LoopCopy copy;
    copy.element = element;
    copy.prefix = loop + "/" + std::to_string(element.element) + "/";
    const auto &from = body.prefix;
    const auto &to = copy.prefix;
    const GraphLoopExpressionValues values{element.element, static_cast<std::int64_t>(element.index), count};
    const auto prepare = [&](Json &declaration) {
        renameTemplateStrings(declaration, from, to);
        for (const auto &[placeholder, resource] : substitutions) repointResource(declaration, placeholder, resource);
        evaluateExpressions(declaration, declaration.value("name", std::string{}), values, constants);
    };
    for (const auto &[family, pass] : body.passes) {
        auto renamed = pass;
        prepare(renamed);
        copy.passes.emplace_back(family, std::move(renamed));
    }
    for (const auto &[list, source] : {std::pair{&copy.compute_tasks, &body.compute_tasks},
                                       std::pair{&copy.render_targets, &body.render_targets},
                                       std::pair{&copy.buffers, &body.buffers}})
        for (const auto &declaration : *source) {
            auto renamed = declaration;
            prepare(renamed);
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
    for (const auto &[port, resource] : body.outputs) {
        auto value = renamedTemplateName(resource, from, to);
        if (const auto found = substitutions.find(value); found != substitutions.end()) value = found->second;
        copy.outputs.emplace(port, std::move(value));
    }
    for (const auto &[family, pass] : copy.passes)
        for (const auto &resource : declarationWrites(pass))
            copy.writers[resource].emplace_back(pass.at("name").get<std::string>(), family);
    for (const auto &task : copy.compute_tasks)
        for (const auto &resource : declarationWrites(task))
            copy.writers[resource].emplace_back(task.at("name").get<std::string>(), std::string{});
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

// A declaration of a copy that names `resource` waits on what wrote it.
void waitOnWriters(LoopCopy &copy, const std::string &resource, const std::vector<NamedDeclaration> &writers) {
    const auto wait = [&](Json &declaration, const std::string &family) {
        if (!declarationResources(declaration).contains(resource)) return;
        for (const auto &[writer, writer_family] : writers)
            if (mayOrderAgainst(family, writer_family, writer_family.empty()))
                appendOrder(declaration, "after", writer);
    };
    for (auto &[family, pass] : copy.passes) wait(pass, family);
    for (auto &task : copy.compute_tasks) wait(task, {});
}

// LP-1b: the value of a loop's output `port`: the last element's (no
// `element`), or one element's. Empty when no element has it: a loop with no
// element that ran, unless `port` is a carry's `from` (then the carried value
// passes through), or an element no copy has. A value whose resource is empty
// is not a render resource (a scalar output) and is left to the values.
struct LoopValue {
    std::string resource;
    std::vector<NamedDeclaration> writers;
};

std::optional<LoopValue> loopOutputValue(const LoopPlan &plan, const std::string &port,
                                         std::optional<std::int64_t> element) {
    std::optional<std::size_t> chosen;
    if (element) {
        for (std::size_t i = 0; i < plan.copies.size(); ++i)
            if (plan.copies[i].element.element == *element) chosen = i;
        if (!chosen) return std::nullopt;
    } else if (!plan.copies.empty()) {
        chosen = plan.copies.size() - 1;
    }
    if (!chosen) {
        for (const auto &pair : plan.loop->shape.carry) {
            if (pair.from != port) continue;
            const auto found = plan.carried_first.find(pair.to);
            if (found == plan.carried_first.end()) return LoopValue{};
            const auto writers = plan.carried_first_writers.find(found->second);
            return LoopValue{found->second, writers != plan.carried_first_writers.end()
                                                ? writers->second : std::vector<NamedDeclaration>{}};
        }
        return std::nullopt;
    }
    const auto &copy = plan.copies[*chosen];
    const auto output = copy.outputs.find(port);
    if (output == copy.outputs.end()) return LoopValue{};
    LoopValue value{output->second, {}};
    // What wrote it: this element, or -- a value an element passed through --
    // the latest element before it that did, or what wrote the first value.
    for (auto i = *chosen + 1; i-- > 0 && value.writers.empty();)
        if (const auto found = plan.copies[i].writers.find(value.resource); found != plan.copies[i].writers.end())
            value.writers = found->second;
    if (value.writers.empty())
        if (const auto found = plan.carried_first_writers.find(value.resource); found != plan.carried_first_writers.end())
            value.writers = found->second;
    return value;
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

// Step 1: the length of the loop's set. A range's count (LP-1b: evaluated
// with the composed config's constants). LP-Vb: a view axis's length, as the
// family's provider gives it for what was composed; an axis the provider
// knows only per frame cannot be expanded when the graph is recompiled.
std::int64_t loopSetLength(const GraphLoopShape &shape, const GraphLoopConstants &constants,
                           const RenderGraphFeatureContext &features) {
    if (shape.set != GraphLoopSet::view_axis) return graphLoopCount(shape, constants);
    if (!features.view_axis_length)
        throw std::runtime_error("graph.loop " + shape.node + " runs over the view axis " + shape.view_family +
                                 " " + shape.view_axis +
                                 ", and this composition has no view family providers to size it");
    const auto length = features.view_axis_length(shape.view_family, shape.view_axis);
    switch (length.kind) {
    case RenderGraphViewAxisKind::unknown:
        throw GraphRegistryError("graph.view_select_axis_unknown@1",
                                 {{"pass", shape.node}, {"family", shape.view_family}, {"axis", shape.view_axis}});
    case RenderGraphViewAxisKind::runtime:
        throw GraphRegistryError("graph.loop_tier_unsupported@1", {{"node", shape.node}, {"reason", "set_unsupported"}});
    case RenderGraphViewAxisKind::compile_time:
        break;
    }
    return static_cast<std::int64_t>(length.length);
}

// LP-1e: a loop's run of passes with no place of its own (no insert, no
// canonical_anchor) and no graph pass to stand by joins the family's scene
// passes, as an unplaced graph pass does (canonicalizePasses): before the
// family's first canonical anchor -- so before output_transform too. It still
// stands after the last pass it waits on (an after of a copy, a before naming
// a copy or the loop) and before the first pass that waits on it.
void insertAmongScenePasses(Json &list, std::vector<Json> run, const std::string &loop,
                            const std::function<bool(const Json &)> &depends) {
    std::set<std::string> names{loop};
    std::set<std::string> waited;
    for (const auto &pass : run) {
        names.insert(pass.at("name").get<std::string>());
        for (const auto &name : orderNames(pass, "after")) waited.insert(name);
    }
    auto scene_end = list.size();
    auto dependent = list.size();
    std::size_t floor = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto &pass = list.at(i);
        const auto type = pass.value("type", std::string{});
        if (scene_end == list.size() && (type == "canonical_anchor" || type == "output_transform")) scene_end = i;
        const auto before = orderNames(pass, "before");
        if (waited.contains(pass.value("name", std::string{})) ||
            std::any_of(before.begin(), before.end(), [&](const std::string &name) { return names.contains(name); }))
            floor = i + 1;
        if (dependent == list.size() && depends(pass)) dependent = i;
    }
    const auto position = std::min(dependent, std::max(scene_end, floor));
    for (std::size_t i = 0; i < run.size(); ++i)
        list.insert(list.begin() + static_cast<Json::difference_type>(position + i), std::move(run[i]));
}

// A copy of a consumer: its name and, when it is a loop body's, its element.
struct ConsumerCopy {
    std::string name;
    std::optional<GraphLoopElement> element;
    std::int64_t count = 0;
};
} // namespace

RenderGraphLoopResolution expandRenderGraphLoops(Json &composed, const LoweredRenderGraphDocument &lowered,
                                                 const RenderGraphFeatureContext &features) {
    RenderGraphLoopResolution resolution;
    if (lowered.loops.empty()) return resolution;
    const auto constants = composedConstants(composed);
    std::set<std::string> loop_ids;
    for (const auto &loop : lowered.loops) loop_ids.insert(loop.shape.node);
    // Who reads each loop's output: a graph pass or task by name, or a node of
    // another loop's body template (copied per element below).
    std::map<std::string, std::vector<const RenderGraphLoopOutputRead *>> reads_by_loop;
    for (const auto &read : lowered.loop_output_reads) reads_by_loop[read.loop].push_back(&read);
    // The copies of each template name, once made.
    std::map<std::string, std::vector<ConsumerCopy>> copied_names;
    const auto consumerCopies = [&](const std::string &consumer) {
        const auto found = copied_names.find(consumer);
        return found != copied_names.end() ? found->second : std::vector<ConsumerCopy>{{consumer, std::nullopt, 0}};
    };

    // Step 1: the elements, and the limits.
    std::vector<LoopPlan> plans(lowered.loops.size());
    std::map<std::string, std::size_t> plan_index;
    std::size_t graph_nodes = lowered.graph_node_count;
    for (std::size_t i = 0; i < lowered.loops.size(); ++i) {
        const auto &loop = lowered.loops[i];
        auto &plan = plans[i];
        plan.loop = &loop;
        plan_index.emplace(loop.shape.node, i);
        // LP-1b: a count written as an expression, with the composed
        // config's constants; LP-Vb: a view axis's length.
        plan.elements = graphLoopElements(loop.shape, loopSetLength(loop.shape, constants, features), constants);
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

    // LP-1b: the value a reader gets from a loop's output: `p` the last
    // element's, `p[]` the element its edge names, evaluated with the
    // receiving element's values when the receiver is a loop. Refused by name
    // when there is no such value.
    const auto readValue = [&](const RenderGraphLoopOutputRead &read,
                               const GraphLoopExpressionValues &receiver) -> std::optional<LoopValue> {
        const auto &plan = plans.at(plan_index.at(read.loop));
        std::optional<std::int64_t> element;
        if (read.element.is_number_integer()) {
            element = read.element.get<std::int64_t>();
        } else if (!read.element.is_null()) {
            try {
                element = evaluateGraphLoopExpression(read.element, GraphLoopExpressionKind::integer,
                                                      read.receiver_loop ? receiver : GraphLoopExpressionValues{},
                                                      constants).get<std::int64_t>();
            } catch (const GraphLoopExpressionError &error) {
                throw graphLoopExpressionInvalid(read.receiver, read.element.value("$pointer", std::string{}),
                                                 error.reason());
            }
        }
        auto value = loopOutputValue(plan, read.port, element);
        if (!value) {
            if (element)
                throw GraphRegistryError("graph.loop_element_unknown@1",
                    {{"node", read.receiver}, {"port", read.input}, {"element", *element}});
            throw GraphRegistryError("graph.loop_output_empty@1", {{"node", read.loop}, {"port", read.port}});
        }
        if (value->resource.empty()) return std::nullopt;
        return value;
    };
    const auto inLoopBody = [&](const std::string &consumer) {
        return std::any_of(loop_ids.begin(), loop_ids.end(),
                           [&](const std::string &loop) { return consumer.starts_with(loop + "/$"); });
    };

    for (const auto index : loopDependencyOrder(lowered)) {
        auto &plan = plans[index];
        const auto &loop = *plan.loop;
        const auto &id = loop.shape.node;
        const auto count = static_cast<std::int64_t>(plan.elements.size());
        // LP-1b: what this loop's bodies read of other loops' outputs. Each
        // copy resolves them for its own element as it is made, so what it
        // passes on (a carried value, an output) is a resource, never a
        // placeholder.
        std::vector<const RenderGraphLoopOutputRead *> body_reads;
        for (const auto &read : lowered.loop_output_reads)
            if (read.consumer.starts_with(id + "/$")) body_reads.push_back(&read);
        // LP-1b: the first value of each carried input: the loop's edge, or
        // the last element of the loop the edge reads (expanded before this
        // one), which the elements that read it wait on.
        for (const auto &[to, input] : loop.carried) {
            if (input.loop.empty()) {
                plan.carried_first.emplace(to, input.resource);
                continue;
            }
            const auto &source = plans.at(plan_index.at(input.loop));
            const auto value = loopOutputValue(source, input.port, std::nullopt);
            if (!value) throw GraphRegistryError("graph.loop_output_empty@1", {{"node", input.loop}, {"port", input.port}});
            plan.carried_first.emplace(to, value->resource);
            if (!value->writers.empty()) plan.carried_first_writers.emplace(value->resource, value->writers);
        }
        auto carried = plan.carried_first;
        // Step 2: a copy per element that has a body. An element with no body
        // passes the carried values on.
        for (const auto &element : plan.elements) {
            const auto &body = loop.branches.at(element.branch);
            if (!body) continue;
            std::map<std::string, std::string> substitutions;
            for (const auto &[to, value] : carried) substitutions.emplace(graphLoopCarryResource(id, to), value);
            std::vector<LoopValue> waits;
            const GraphLoopExpressionValues values{element.element, static_cast<std::int64_t>(element.index), count};
            for (const auto *read : body_reads) {
                if (!read->consumer.starts_with(body->prefix)) continue;
                auto value = readValue(*read, values);
                if (!value) continue;
                substitutions.insert_or_assign(read->resource, value->resource);
                waits.push_back(std::move(*value));
            }
            plan.copies.push_back(copyLoopBody(composed, id, *body, element, count, constants, substitutions));
            auto &copy = plan.copies.back();
            for (const auto &pair : loop.shape.carry)
                if (const auto found = copy.outputs.find(pair.from); found != copy.outputs.end())
                    carried[pair.to] = found->second;
            for (const auto &[resource, writers] : plan.carried_first_writers) waitOnWriters(copy, resource, writers);
            for (const auto &value : waits) waitOnWriters(copy, value.resource, value.writers);
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
                copied_names[body->prefix + name.substr(copy.prefix.size())].push_back({name, element, count});
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
            for (const auto &consumer : consumerCopies(read->consumer)) dependents.insert(consumer.name);
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
            // none, among the scene's passes (LP-1e).
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
            insertAmongScenePasses(list, std::move(passes), id, depends);
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

    // Every other reader of a loop output -- a graph pass or task outside the
    // loops -- reads its value and waits on what writes it: `p` the last
    // element's, `p[]` (LP-1b) the element its edge names.
    auto declarations = composedDeclarations(composed);
    for (const auto &read : lowered.loop_output_reads) {
        if (!plan_index.contains(read.loop) || inLoopBody(read.consumer)) continue;
        for (const auto &consumer : consumerCopies(read.consumer)) {
            const auto value = readValue(read, {});
            if (!value) continue;
            const auto found = declarations.find(consumer.name);
            if (found == declarations.end()) continue;
            auto &declaration = *found->second.declaration;
            if (read.resource != value->resource) repointResource(declaration, read.resource, value->resource);
            for (const auto &[writer, writer_family] : value->writers)
                if (writer != consumer.name && mayOrderAgainst(found->second.family, writer_family, writer_family.empty()))
                    appendOrder(declaration, "after", writer);
        }
    }
    return resolution;
}

namespace {
// LP-3a: a draw_loop runs inside its material pass, so ordering against the
// loop is ordering against the pass; and it reports no element until a
// generation knows the materials (get_frame_plan fills them in).
void resolveDrawLoops(Json &composed, const LoweredRenderGraphDocument &lowered, RenderGraphLoopResolution &resolution) {
    if (lowered.draw_loops.empty()) return;
    std::map<std::string, std::string> pass_of;
    for (const auto &loop : lowered.draw_loops) pass_of.emplace(loop.loop, loop.pass);
    for (auto &[name, entry] : composedDeclarations(composed)) {
        (void)name;
        for (const auto *key : {"after", "before"}) {
            const auto list = entry.declaration->find(key);
            if (list == entry.declaration->end() || !list->is_array()) continue;
            for (auto &item : *list)
                if (item.is_string())
                    if (const auto found = pass_of.find(item.get<std::string>()); found != pass_of.end())
                        item = found->second;
        }
    }
    for (const auto &loop : lowered.draw_loops) {
        RenderGraphLoopReport report{loop.loop, graphLoopTierName(GraphLoopTier::draw_loop), 0, {}, {}, loop.pass};
        resolution.loops.push_back(std::move(report));
    }
    std::sort(resolution.loops.begin(), resolution.loops.end(),
              [](const RenderGraphLoopReport &lhs, const RenderGraphLoopReport &rhs) { return lhs.node < rhs.node; });
}

// --- LP-0d: what a variant excluded, and pass_replacements.

void dropOrderNames(Json &declaration, const std::set<std::string> &names) {
    for (const auto *key : {"after", "before"}) {
        const auto list = declaration.find(key);
        if (list == declaration.end() || !list->is_array()) continue;
        list->erase(std::remove_if(list->begin(), list->end(), [&](const Json &name) {
            return name.is_string() && names.contains(name.get<std::string>());
        }), list->end());
    }
}

// Every pass of every family, then every compute task, in declared order.
void forEachComposedDeclaration(Json &composed, const std::function<void(Json &, const std::string &)> &visit) {
    if (const auto families = composed.find("rendering_passes"); families != composed.end() && families->is_array())
        for (auto &family : *families) {
            const auto name = family.value("name", std::string{});
            if (const auto passes = family.find("passes"); passes != family.end() && passes->is_array())
                for (auto &pass : *passes)
                    if (pass.is_object()) visit(pass, name);
        }
    if (const auto tasks = composed.find("compute_tasks"); tasks != composed.end() && tasks->is_array())
        for (auto &task : *tasks)
            if (task.is_object()) visit(task, {});
}

void eraseDeclarations(Json &composed, const std::set<std::string> &names) {
    const auto named = [&](const Json &declaration) {
        return declaration.is_object() && names.contains(declaration.value("name", std::string{}));
    };
    if (const auto families = composed.find("rendering_passes"); families != composed.end() && families->is_array())
        for (auto &family : *families)
            if (const auto passes = family.find("passes"); passes != family.end() && passes->is_array())
                passes->erase(std::remove_if(passes->begin(), passes->end(), named), passes->end());
    if (const auto tasks = composed.find("compute_tasks"); tasks != composed.end() && tasks->is_array())
        tasks->erase(std::remove_if(tasks->begin(), tasks->end(), named), tasks->end());
}

// A name that leaves the config leaves target_planning's nodes with it.
void forgetPlanningNodes(Json &composed, const std::set<std::string> &names) {
    const auto planning = composed.find("target_planning");
    if (planning == composed.end() || !planning->is_object()) return;
    const auto graphs = planning->find("graphs");
    if (graphs == planning->end() || !graphs->is_object()) return;
    for (auto &graph : *graphs) {
        const auto nodes = graph.find("nodes");
        if (nodes == graph.end() || !nodes->is_object()) continue;
        for (const auto &name : names) nodes->erase(name);
    }
}

using NameReplacement = std::function<std::optional<std::vector<std::string>>(const std::string &graph,
                                                                              const std::string &name)>;

// frame_node_order's lists: a name `replace` maps is replaced by what it maps
// to (a name already listed is not listed twice); mapped to nothing, it leaves.
void rewriteFrameNodeOrder(Json &composed, const NameReplacement &replace) {
    const auto order = composed.find("frame_node_order");
    if (order == composed.end() || !order->is_object()) return;
    for (auto &[graph, names] : order->items()) {
        if (!names.is_array()) continue;
        std::set<std::string> listed;
        for (const auto &name : names)
            if (name.is_string()) listed.insert(name.get<std::string>());
        Json rewritten = Json::array();
        std::set<std::string> written;
        for (const auto &name : names) {
            if (!name.is_string()) {
                rewritten.push_back(name);
                continue;
            }
            const auto replacement = replace(graph, name.get<std::string>());
            if (!replacement) {
                if (written.insert(name.get<std::string>()).second) rewritten.push_back(name);
                continue;
            }
            for (const auto &entry : *replacement)
                if (!listed.contains(entry) && written.insert(entry).second) rewritten.push_back(entry);
        }
        names = std::move(rewritten);
    }
}

} // namespace

LoweredRenderGraphDocument withoutExcludedGraphDependents(Json &composed, const LoweredRenderGraphDocument &lowered,
                                                          const RenderGraphFeatureContext &features,
                                                          std::vector<RenderGraphDeactivatedNode> &deactivated) {
    auto active = lowered;
    if (features.excluded_passes.empty() && features.excluded_resources.empty() &&
        features.excluded_replacements.empty())
        return active;
    std::set<std::string> inactive;
    const auto deactivate = [&](const std::string &node, const std::string &feature, const char *reason) {
        if (!inactive.insert(node).second) return false;
        deactivated.push_back({node, feature, reason});
        return true;
    };
    const auto excludedPass = [&](const std::string &name) -> const std::string * {
        const auto found = features.excluded_passes.find(name);
        return found != features.excluded_passes.end() ? &found->second : nullptr;
    };
    const auto excludedResource = [&](const std::string &name) -> const std::string * {
        const auto found = features.excluded_resources.find(name);
        return found != features.excluded_resources.end() ? &found->second : nullptr;
    };
    const auto replacedExcluded = [&](const std::string &node) -> const std::string * {
        for (const auto &replacement : features.excluded_replacements)
            if (replacement.node == node) return &replacement.feature;
        return nullptr;
    };
    const auto resourceOf = [&](const Json &declaration) -> const std::string * {
        for (const auto &resource : declarationResources(declaration))
            if (const auto *feature = excludedResource(resource)) return feature;
        return nullptr;
    };
    const auto orderOf = [&](const Json &declaration) -> const std::string * {
        for (const auto *key : {"after", "before"})
            for (const auto &name : orderNames(declaration, key))
                if (const auto *feature = excludedPass(name)) return feature;
        // LP-0e: the pass a snapshot copy copies after.
        if (const auto snapshot = declaration.find("snapshot_after");
            snapshot != declaration.end() && snapshot->is_string())
            if (const auto *feature = excludedPass(snapshot->get<std::string>())) return feature;
        return nullptr;
    };
    // Why a graph pass or task goes: it replaces a pass of the feature, its
    // anchor is one, it names a target of the feature, an edge names a pass
    // of it -- the first that holds.
    const auto judge = [&](const std::string &name, const Json &declaration, const std::string *anchor_feature) {
        if (const auto *feature = replacedExcluded(name)) deactivate(name, *feature, "pass_replacement");
        else if (anchor_feature != nullptr) deactivate(name, *anchor_feature, "insert");
        else if (const auto *feature = resourceOf(declaration)) deactivate(name, *feature, "target_ref");
        else if (const auto *feature = orderOf(declaration)) deactivate(name, *feature, "order");
    };
    forEachComposedDeclaration(composed, [&](Json &declaration, const std::string &) {
        const auto name = declaration.value("name", std::string{});
        if (lowered.graph_declarations.contains(name)) judge(name, declaration, nullptr);
    });
    for (const auto &run : lowered.placed_runs)
        for (const auto &pass : run.passes)
            judge(pass.at("name").get<std::string>(), pass, excludedPass(run.anchor));
    for (const auto &loop : lowered.loops) {
        const auto &id = loop.shape.node;
        const std::string *resource = nullptr;
        const std::string *order = nullptr;
        for (const auto &branch : loop.branches) {
            if (!branch) continue;
            for (const auto &reference : branch->target_refs)
                if (resource == nullptr) resource = excludedResource(reference.name);
            for (const auto &[family, pass] : branch->passes) {
                if (resource == nullptr) resource = resourceOf(pass);
                if (order == nullptr) order = orderOf(pass);
            }
            for (const auto &task : branch->compute_tasks) {
                if (resource == nullptr) resource = resourceOf(task);
                if (order == nullptr) order = orderOf(task);
            }
        }
        for (const auto &[to, input] : loop.carried)
            if (resource == nullptr) resource = excludedResource(input.resource);
        for (const auto *names : {&loop.shape.after, &loop.shape.before})
            for (const auto &name : *names)
                if (order == nullptr) order = excludedPass(name);
        const auto *anchor = loop.shape.placement ? excludedPass(loop.shape.placement->anchor) : nullptr;
        if (const auto *feature = replacedExcluded(id)) deactivate(id, *feature, "pass_replacement");
        else if (anchor != nullptr) deactivate(id, *anchor, "insert");
        else if (resource != nullptr) deactivate(id, *resource, "target_ref");
        else if (order != nullptr) deactivate(id, *order, "order");
    }
    // A loop made inactive has no output: whoever reads one, or carries from
    // one, goes too.
    std::set<std::string> loop_ids;
    for (const auto &loop : lowered.loops) loop_ids.insert(loop.shape.node);
    const auto loopOfBody = [&](const std::string &consumer) -> std::optional<std::string> {
        for (const auto &id : loop_ids)
            if (consumer.starts_with(id + "/$")) return id;
        return std::nullopt;
    };
    const auto featureOf = [&](const std::string &node) {
        for (const auto &entry : deactivated)
            if (entry.node == node) return entry.feature;
        return std::string{};
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto &read : lowered.loop_output_reads) {
            if (!inactive.contains(read.loop)) continue;
            changed = deactivate(loopOfBody(read.consumer).value_or(read.consumer), featureOf(read.loop),
                                 "loop_output") || changed;
        }
        for (const auto &loop : lowered.loops)
            for (const auto &[to, input] : loop.carried)
                if (!input.loop.empty() && inactive.contains(input.loop))
                    changed = deactivate(loop.shape.node, featureOf(input.loop), "loop_output") || changed;
        // A place next to a pass made inactive is gone with it.
        for (const auto &run : lowered.placed_runs) {
            if (!inactive.contains(run.anchor)) continue;
            for (const auto &pass : run.passes)
                changed = deactivate(pass.at("name").get<std::string>(), featureOf(run.anchor), "insert") || changed;
        }
        for (const auto &loop : lowered.loops)
            if (loop.shape.placement && inactive.contains(loop.shape.placement->anchor))
                changed = deactivate(loop.shape.node, featureOf(loop.shape.placement->anchor), "insert") || changed;
    }

    // What is left: no inactive node, no check of a reference to an excluded
    // target, and no edge to what is gone.
    std::set<std::string> gone = inactive;
    for (const auto &[name, feature] : features.excluded_passes) gone.insert(name);
    std::size_t removed = 0;
    for (const auto &name : inactive)
        if (lowered.graph_declarations.contains(name)) ++removed;
    eraseDeclarations(composed, inactive);
    forgetPlanningNodes(composed, inactive);
    rewriteFrameNodeOrder(composed, [&](const std::string &, const std::string &name) {
        return inactive.contains(name) ? std::optional<std::vector<std::string>>{std::vector<std::string>{}}
                                       : std::nullopt;
    });
    forEachComposedDeclaration(composed, [&](Json &declaration, const std::string &) {
        dropOrderNames(declaration, gone);
    });
    for (auto &run : active.placed_runs) {
        run.passes.erase(std::remove_if(run.passes.begin(), run.passes.end(), [&](const Json &pass) {
            return inactive.contains(pass.at("name").get<std::string>());
        }), run.passes.end());
        for (auto &pass : run.passes) dropOrderNames(pass, gone);
        if (!run.passes.empty()) run.node = run.passes.front().at("name").get<std::string>();
    }
    std::erase_if(active.placed_runs, [](const RenderGraphPassRun &run) { return run.passes.empty(); });
    std::erase_if(active.loops, [&](const RenderGraphLoop &loop) { return inactive.contains(loop.shape.node); });
    for (auto &loop : active.loops) {
        for (auto *names : {&loop.shape.after, &loop.shape.before})
            std::erase_if(*names, [&](const std::string &name) { return gone.contains(name); });
        for (auto &branch : loop.branches) {
            if (!branch) continue;
            for (auto &[family, pass] : branch->passes) dropOrderNames(pass, gone);
            for (auto &task : branch->compute_tasks) dropOrderNames(task, gone);
        }
    }
    std::erase_if(active.loop_output_reads, [&](const RenderGraphLoopOutputRead &read) {
        const auto body = loopOfBody(read.consumer);
        return inactive.contains(read.loop) || inactive.contains(read.consumer) || (body && inactive.contains(*body));
    });
    std::erase_if(active.target_refs, [&](const RenderGraphTargetReference &reference) {
        return excludedResource(reference.name) != nullptr;
    });
    std::erase_if(active.history_reads, [&](const RenderGraphHistoryRead &read) {
        return inactive.contains(read.node) || excludedResource(read.target) != nullptr;
    });
    // LP-3a: a draw_loop draws inside its render.material_pass; with the pass
    // gone it has nothing to draw into.
    std::erase_if(active.draw_loops, [&](const GraphDrawLoop &loop) { return inactive.contains(loop.pass); });
    for (const auto &name : inactive) active.graph_declarations.erase(name);
    active.graph_node_count -= std::min(removed, active.graph_node_count);
    return active;
}

void applyRenderFeaturePassReplacements(Json &composed, const LoweredRenderGraphDocument &lowered,
                                        const std::map<std::string, RenderLoopOrigin> &loop_passes,
                                        const RenderGraphFeatureContext &features) {
    if (features.replacements.empty()) return;
    std::set<std::string> loop_ids;
    for (const auto &loop : lowered.loops) loop_ids.insert(loop.shape.node);
    // One node may take the place of several passes (planar's mip chain):
    // each node once, over every entry that names it, in the order named.
    std::vector<std::string> nodes;
    std::map<std::string, std::vector<const RenderFeaturePassReplacement *>> entries_of;
    for (const auto &replacement : features.replacements) {
        if (!entries_of.contains(replacement.node)) nodes.push_back(replacement.node);
        entries_of[replacement.node].push_back(&replacement);
    }
    // What a declaration writes: its attachments, its writes, a copy's
    // destination.
    const auto writesOf = [](const Json &declaration) {
        auto writes = declarationWrites(declaration);
        if (const auto destination = declaration.find("destination");
            destination != declaration.end() && destination->is_string())
            writes.insert(destination->get<std::string>());
        return writes;
    };
    for (const auto &node : nodes) {
        const auto &entries = entries_of.at(node);
        auto declarations = composedDeclarations(composed);
        for (const auto *entry : entries) {
            const auto owner = features.feature_passes.find(entry->pass);
            if (owner == features.feature_passes.end() || owner->second != entry->feature ||
                !declarations.contains(entry->pass))
                throw GraphRegistryError("render_feature.pass_replacement_unknown@1",
                                         {{"feature", entry->feature}, {"pass", entry->pass}});
        }
        const bool loop = loop_ids.contains(node);
        if (!loop && !lowered.graph_declarations.contains(node))
            throw GraphRegistryError("graph.node_missing@1", {{"node", node},
                                                              {"feature", entries.front()->feature},
                                                              {"pass", entries.front()->pass}});
        const auto isMember = [&](const std::string &name) {
            if (!loop) return name == node;
            const auto found = loop_passes.find(name);
            return found != loop_passes.end() && found->second.loop == node;
        };
        // The node's passes and tasks in declared order: family by family,
        // then the tasks (declared after every pass).
        std::vector<NamedDeclaration> members;
        forEachComposedDeclaration(composed, [&](Json &declaration, const std::string &family) {
            const auto name = declaration.value("name", std::string{});
            if (isMember(name)) members.emplace_back(name, family);
        });
        std::set<std::string> member_names;
        std::set<std::string> written;
        for (const auto &[name, family] : members) {
            member_names.insert(name);
            const auto writes = writesOf(*declarations.at(name).declaration);
            written.insert(writes.begin(), writes.end());
        }
        std::set<std::string> replaced;
        std::map<std::string, Json> replaced_declarations;
        std::map<std::string, std::string> replaced_family;
        std::map<std::string, std::vector<std::string>> replaced_by_family; // "" for tasks
        for (const auto *entry : entries) {
            const auto &found = declarations.at(entry->pass);
            for (const auto &resource : writesOf(*found.declaration))
                if (!written.contains(resource))
                    throw std::runtime_error("render feature '" + entry->feature + "' pass_replacements['" +
                                             entry->pass + "']: '" + node + "' no longer writes resource '" +
                                             resource + "'");
            replaced.insert(entry->pass);
            replaced_declarations.emplace(entry->pass, *found.declaration);
            replaced_family.emplace(entry->pass, found.family);
            replaced_by_family[found.family].push_back(entry->pass);
        }
        // The node's passes (tasks) of each replaced family move to where the
        // first replaced one stood, and the replaced ones leave.
        std::map<std::string, std::vector<std::string>> moving_by_family;
        for (const auto &[family, passes] : replaced_by_family) {
            auto &moving = moving_by_family[family];
            for (const auto &[name, member_family] : members)
                if (member_family == family) moving.push_back(name);
            if (moving.empty()) {
                const bool has_pass = std::any_of(members.begin(), members.end(),
                                                  [](const NamedDeclaration &member) { return !member.second.empty(); });
                if (!family.empty() && has_pass)
                    throw GraphRegistryError("graph.insert_family_mismatch@1",
                                             {{"node", node}, {"anchor", passes.front()}});
                throw std::runtime_error("render feature '" + entries.front()->feature + "' pass_replacements['" +
                                         passes.front() + "']: '" + node + "' has no " +
                                         (family.empty() ? "compute task" : "pass") + " to take its place");
            }
            Json *list = nullptr;
            if (family.empty()) {
                list = &composed.at("compute_tasks");
            } else {
                for (auto &candidate : composed.at("rendering_passes"))
                    if (candidate.value("name", std::string{}) == family) list = &candidate.at("passes");
            }
            const auto moves = [&](const std::string &name) {
                return std::find(moving.begin(), moving.end(), name) != moving.end();
            };
            std::vector<Json> moved;
            for (const auto &declaration : *list)
                if (moves(declaration.value("name", std::string{}))) moved.push_back(declaration);
            Json rebuilt = Json::array();
            bool placed = false;
            for (auto &declaration : *list) {
                const auto name = declaration.value("name", std::string{});
                if (replaced.contains(name)) {
                    if (!placed)
                        for (auto &member : moved) rebuilt.push_back(std::move(member));
                    placed = true;
                    continue;
                }
                if (moves(name)) continue;
                rebuilt.push_back(std::move(declaration));
            }
            *list = std::move(rebuilt);
        }
        // Every name of a replaced pass now names the node: `after` its last
        // pass or task the holder may order against, `before` its first.
        const auto orderable = [&](const std::string &holder_family) {
            std::vector<std::string> names;
            for (const auto &[name, family] : members)
                if (mayOrderAgainst(holder_family, family, family.empty())) names.push_back(name);
            return names;
        };
        forEachComposedDeclaration(composed, [&](Json &declaration, const std::string &family) {
            const auto holder = declaration.value("name", std::string{});
            // A pass of the node itself stands where the replaced one stood,
            // so it keeps no edge to it.
            const bool member = member_names.contains(holder);
            const auto candidates = orderable(family);
            for (const auto *key : {"after", "before"}) {
                const auto list = declaration.find(key);
                if (list == declaration.end() || !list->is_array()) continue;
                Json rewritten = Json::array();
                for (const auto &item : *list) {
                    if (!item.is_string() || !replaced.contains(item.get<std::string>())) {
                        rewritten.push_back(item);
                        continue;
                    }
                    if (member || candidates.empty()) continue;
                    const auto &name = std::string_view{key} == "after" ? candidates.back() : candidates.front();
                    if (name != holder && std::find(rewritten.begin(), rewritten.end(), name) == rewritten.end())
                        rewritten.push_back(name);
                }
                *list = std::move(rewritten);
            }
            if (const auto snapshot = declaration.find("snapshot_after");
                snapshot != declaration.end() && snapshot->is_string() &&
                replaced.contains(snapshot->get<std::string>()) && !candidates.empty())
                *snapshot = candidates.back();
        });
        // The replaced passes' own edges go to the node's passes that took
        // their place: `after` to the first, `before` to the last.
        declarations = composedDeclarations(composed);
        for (const auto &[pass, declaration] : replaced_declarations) {
            const auto &moving = moving_by_family.at(replaced_family.at(pass));
            for (const auto *key : {"after", "before"}) {
                auto &target =
                    *declarations.at(std::string_view{key} == "after" ? moving.front() : moving.back()).declaration;
                for (const auto &name : orderNames(declaration, key))
                    if (!replaced.contains(name) && !member_names.contains(name)) appendOrder(target, key, name);
            }
        }
        rewriteFrameNodeOrder(composed, [&](const std::string &graph, const std::string &name) {
            if (!replaced.contains(name)) return std::optional<std::vector<std::string>>{};
            std::vector<std::string> names;
            for (const auto &[member, family] : members)
                if (family.empty() || family == graph) names.push_back(member);
            return std::optional<std::vector<std::string>>{std::move(names)};
        });
        forgetPlanningNodes(composed, replaced);
        // LP-Vb (design 14.7): a replaced pass that consumed a surface
        // resource leaves the consumer lists; what took its place is picked
        // by the composer like any pass the resolution stage added.
        if (features.surface_resource_contracts != nullptr && features.surface_resource_contracts->is_array())
            for (auto &contract : *features.surface_resource_contracts)
                for (const auto *key : {"material_consumers", "fullscreen_consumers"})
                    if (const auto list = contract.find(key); list != contract.end() && list->is_array())
                        list->erase(std::remove_if(list->begin(), list->end(),
                                                   [&](const Json &name) {
                                                       return name.is_string() &&
                                                              replaced.contains(name.get<std::string>());
                                                   }),
                                    list->end());
        // A surface resource the replaced pass produced is produced by the
        // node's last pass that writes it.
        if (features.surface_resource_contracts != nullptr && features.surface_resource_contracts->is_array())
            for (auto &contract : *features.surface_resource_contracts) {
                if (!contract.is_object() || !contract.contains("producer") || !contract.at("producer").is_string() ||
                    !replaced.contains(contract.at("producer").get<std::string>()))
                    continue;
                const auto resource = contract.value("resource", std::string{});
                std::string producer;
                for (const auto &[name, family] : members)
                    if (!family.empty() && writesOf(*declarations.at(name).declaration).contains(resource))
                        producer = name;
                if (producer.empty())
                    throw std::runtime_error("surface_resources contract '" +
                                             contract.value("contract", std::string{}) + "' producer '" +
                                             contract.at("producer").get<std::string>() + "' is replaced by '" +
                                             node + "', which no longer writes resource '" + resource + "'");
                contract["producer"] = producer;
            }
    }
}
}
