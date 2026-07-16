#include "frameplanner.hpp"
#include "renderingpassjsonhelpers.hpp"
#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Pelican {

namespace {

struct Edge {
    size_t from = 0;
    size_t to = 0;
};

struct PlannerEdges {
    std::vector<std::vector<bool>> exists;
    std::vector<Edge> edges;
    std::vector<FramePlanBarrier> barriers;
};

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

void appendUnique(std::vector<std::string> &values, std::string value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(std::move(value));
    }
}

void appendUnique(std::vector<std::string> &values, const std::vector<std::string> &more_values) {
    for (const auto &value : more_values) {
        appendUnique(values, value);
    }
}

void appendNodes(std::vector<FrameGraphNodeDefinition> &nodes,
                 std::vector<FrameGraphNodeDefinition> more_nodes) {
    for (auto &node : more_nodes) {
        nodes.push_back(std::move(node));
    }
}

std::string requireString(const nlohmann::json &json, std::string_view field, std::string_view context) {
    if (!json.contains(field) || !json.at(field).is_string()) {
        throw std::runtime_error("Frame graph " + std::string{context} + " requires string field: " +
                                 std::string{field});
    }
    return json.at(field).get<std::string>();
}

std::vector<std::string> parseStringList(const nlohmann::json &json, std::string_view context) {
    if (json.is_null()) {
        return {};
    }
    if (json.is_string()) {
        return {json.get<std::string>()};
    }
    if (!json.is_array()) {
        throw std::runtime_error("Frame graph " + std::string{context} + " must be a string or string array");
    }

    std::vector<std::string> values;
    values.reserve(json.size());
    for (const auto &entry : json) {
        if (!entry.is_string()) {
            throw std::runtime_error("Frame graph " + std::string{context} + " entries must be strings");
        }
        values.push_back(entry.get<std::string>());
    }
    return values;
}

std::vector<std::string> parseOptionalStringList(const nlohmann::json &json, std::string_view field,
                                                 std::string_view context) {
    if (!json.contains(field)) {
        return {};
    }
    return parseStringList(json.at(field), std::string{context} + "." + std::string{field});
}

void splitHistoryReads(const std::vector<std::string> &authored,
                       std::vector<std::string> &reads,
                       std::vector<std::string> &reads_history) {
    constexpr std::string_view suffix = "@history";
    for (const auto &resource : authored) {
        if (resource.ends_with(suffix)) {
            const auto name = resource.substr(0, resource.size() - suffix.size());
            if (name.empty() || name.find('@') != std::string::npos) {
                throw std::runtime_error("Invalid frame graph history resource: " + resource);
            }
            appendUnique(reads_history, name);
        } else {
            if (resource.find('@') != std::string::npos) {
                throw std::runtime_error("Unknown frame graph resource qualifier: " + resource);
            }
            appendUnique(reads, resource);
        }
    }
}

bool loadOpReadsExistingColor(const nlohmann::json &pass_json, bool ui_pass) {
    if (!pass_json.contains("color_load_op")) {
        return ui_pass;
    }
    if (!pass_json.at("color_load_op").is_string()) {
        throw std::runtime_error("Frame graph pass color_load_op must be a string");
    }
    return lowerAscii(pass_json.at("color_load_op").get<std::string>()) == "load";
}

bool depthLoadOpReadsExistingDepth(const nlohmann::json &pass_json) {
    if (!pass_json.contains("depth_load_op")) {
        return false;
    }
    if (!pass_json.at("depth_load_op").is_string()) {
        throw std::runtime_error("Frame graph pass depth_load_op must be a string");
    }
    return lowerAscii(pass_json.at("depth_load_op").get<std::string>()) == "load";
}

std::vector<std::string> parseOutputColors(const nlohmann::json &output_json) {
    if (!output_json.contains("color")) {
        throw std::runtime_error("Frame graph pass output requires color field");
    }
    return parseStringList(output_json.at("color"), "pass.output.color");
}

std::vector<std::string> parseOutputDepth(const nlohmann::json &output_json) {
    if (!output_json.contains("depth")) {
        throw std::runtime_error("Frame graph pass output requires depth field");
    }
    return parseStringList(output_json.at("depth"), "pass.output.depth");
}

FrameGraphNodeDefinition parseRenderNodeFromJson(const nlohmann::json &pass_json, size_t declaration_index) {
    if (!pass_json.is_object()) {
        throw std::runtime_error("Frame graph pass entries must be objects");
    }
    FrameGraphNodeDefinition node;
    node.name = requireString(pass_json, "name", "pass");
    node.declaration_index = declaration_index;
    node.after = parseOptionalStringList(pass_json, "after", "pass");
    node.before = parseOptionalStringList(pass_json, "before", "pass");

    const auto type = pass_json.value("type", std::string{});
    if (type == "canonical_anchor") {
        node.kind = FramePlanNodeKind::anchor;
        return node;
    }
    if (type == "snapshot_copy") {
        node.kind = FramePlanNodeKind::snapshot_copy;
        node.reads = {requireString(pass_json, "source", "snapshot copy")};
        node.writes = {requireString(pass_json, "destination", "snapshot copy")};
        node.snapshot_after = requireString(pass_json, "snapshot_after", "snapshot copy");
        return node;
    }
    if (!pass_json.contains("output") || !pass_json.at("output").is_object()) {
        throw std::runtime_error("Frame graph pass requires output object");
    }
    splitHistoryReads(parseOptionalStringList(pass_json, "input", "pass"),
                      node.reads, node.reads_history);
    node.kind = type == "output_transform" ? FramePlanNodeKind::output_transform
                                            : FramePlanNodeKind::render;
    const bool ui_pass = type == "ui";
    const auto &output = pass_json.at("output");
    const auto color_outputs = parseOutputColors(output);
    const auto depth_outputs = parseOutputDepth(output);
    appendUnique(node.writes, color_outputs);
    appendUnique(node.writes, depth_outputs);

    if (loadOpReadsExistingColor(pass_json, ui_pass)) {
        appendUnique(node.reads, color_outputs);
    }
    if (depthLoadOpReadsExistingDepth(pass_json)) {
        appendUnique(node.reads, depth_outputs);
    }

    return node;
}

FrameGraphNodeDefinition parseComputeNodeFromJson(const nlohmann::json &task_json, size_t declaration_index) {
    if (!task_json.is_object()) {
        throw std::runtime_error("Frame graph compute_tasks entries must be objects");
    }

    FrameGraphNodeDefinition node;
    node.name = requireString(task_json, "name", "compute task");
    node.kind = FramePlanNodeKind::compute;
    node.declaration_index = declaration_index;
    splitHistoryReads(parseOptionalStringList(task_json, "reads", "compute task"),
                      node.reads, node.reads_history);
    node.writes = parseOptionalStringList(task_json, "writes", "compute task");
    node.after = parseOptionalStringList(task_json, "after", "compute task");
    node.before = parseOptionalStringList(task_json, "before", "compute task");
    return node;
}

std::vector<std::string> parseDeclaredRenderTargets(const nlohmann::json &json) {
    std::vector<std::string> resources;
    if (!json.contains("render_targets")) {
        return resources;
    }
    const auto &targets = json.at("render_targets");
    if (!targets.is_array()) {
        throw std::runtime_error("Frame graph render_targets must be an array");
    }
    for (const auto &target : targets) {
        if (!target.is_object()) {
            throw std::runtime_error("Frame graph render_targets entries must be objects");
        }
        appendUnique(resources, requireString(target, "name", "render target"));
    }
    return resources;
}

std::vector<std::string> parseHistoryRenderTargets(const nlohmann::json &json) {
    std::vector<std::string> resources;
    if (!json.contains("render_targets")) return resources;
    const auto &targets = json.at("render_targets");
    if (!targets.is_array()) throw std::runtime_error("Frame graph render_targets must be an array");
    for (const auto &target : targets) {
        if (!target.is_object()) throw std::runtime_error("Frame graph render_targets entries must be objects");
        if (target.contains("history") && !target.at("history").is_boolean()) {
            throw std::runtime_error("Frame graph render target history must be a boolean");
        }
        if (target.value("history", false)) {
            appendUnique(resources, requireString(target, "name", "render target"));
        }
    }
    return resources;
}

std::vector<std::string> parseDeclaredBuffers(const nlohmann::json &json) {
    std::vector<std::string> resources;
    if (!json.contains("buffers")) {
        return resources;
    }
    const auto &buffers = json.at("buffers");
    if (!buffers.is_array()) {
        throw std::runtime_error("Frame graph buffers must be an array");
    }
    for (const auto &buffer : buffers) {
        if (buffer.is_string()) {
            appendUnique(resources, buffer.get<std::string>());
            continue;
        }
        if (buffer.is_object()) {
            appendUnique(resources, requireString(buffer, "name", "buffer"));
            continue;
        }
        throw std::runtime_error("Frame graph buffers entries must be strings or objects");
    }
    return resources;
}

std::size_t formatBlockBytes(std::string_view format) {
    if (format == "R8_UNORM") return 1;
    if (format == "R16_SFLOAT" || format == "D16_UNORM") return 2;
    if (format == "R8G8B8A8_UNORM" || format == "R8G8B8A8_SRGB" ||
        format == "B8G8R8A8_UNORM" || format == "B8G8R8A8_SRGB" ||
        format == "D32_SFLOAT" || format == "R32_SFLOAT" ||
        format == "R16G16_SFLOAT") return 4;
    if (format == "R16G16B16A16_SFLOAT") return 8;
    if (format == "R32G32B32A32_SFLOAT") return 16;
    return 0;
}

std::unordered_map<std::string, std::size_t> parseRenderTargetByteSizes(const nlohmann::json &json) {
    std::unordered_map<std::string, std::size_t> sizes;
    if (!json.contains("render_targets") || !json.at("render_targets").is_array()) return sizes;
    for (const auto &target : json.at("render_targets")) {
        if (!target.is_object() || !target.contains("width") || !target.contains("height")) continue;
        const auto name = requireString(target, "name", "render target");
        const auto format = requireString(target, "format", "render target '" + name + "'");
        const auto bytes = formatBlockBytes(format);
        if (bytes == 0) continue;
        sizes.emplace(name, target.at("width").get<std::size_t>() *
                                target.at("height").get<std::size_t>() * bytes);
    }
    return sizes;
}

void assignSnapshotByteSizes(std::vector<FrameGraphNodeDefinition> &nodes,
                             const std::unordered_map<std::string, std::size_t> &sizes) {
    for (auto &node : nodes) {
        if (node.kind != FramePlanNodeKind::snapshot_copy) continue;
        const auto found = sizes.find(node.writes.front());
        if (found == sizes.end() || found->second == 0) {
            throw std::runtime_error("snapshot copy byte size is unavailable for '" +
                                     node.writes.front() + "'");
        }
        node.byte_size = found->second;
    }
}

std::vector<FrameGraphNodeDefinition> parseRenderNodes(const nlohmann::json &graph_json,
                                                       size_t &declaration_index) {
    std::vector<FrameGraphNodeDefinition> nodes;
    if (!graph_json.contains("passes")) {
        return nodes;
    }
    const auto &passes = graph_json.at("passes");
    if (!passes.is_array()) {
        throw std::runtime_error("Frame graph passes must be an array");
    }
    nodes.reserve(passes.size());
    for (const auto &pass_json : passes) {
        nodes.push_back(parseRenderNodeFromJson(pass_json, declaration_index++));
    }
    return nodes;
}

std::vector<FrameGraphNodeDefinition> parseComputeNodes(const nlohmann::json &graph_json,
                                                        size_t &declaration_index) {
    std::vector<FrameGraphNodeDefinition> nodes;
    if (!graph_json.contains("compute_tasks")) {
        return nodes;
    }
    const auto &tasks = graph_json.at("compute_tasks");
    if (!tasks.is_array()) {
        throw std::runtime_error("Frame graph compute_tasks must be an array");
    }
    nodes.reserve(tasks.size());
    for (const auto &task_json : tasks) {
        nodes.push_back(parseComputeNodeFromJson(task_json, declaration_index++));
    }
    return nodes;
}

std::string renderTargetResourceName(GlobalRenderTargetId id) {
    if (isSwapchainRenderTarget(id)) {
        return "swapchain";
    }
    if (isConcreteRenderTarget(id)) {
        return "rt:" + std::to_string(id.value);
    }
    return {};
}

FramePlanNodeKind passKind(const PassDefinition &) {
    return FramePlanNodeKind::render;
}

FrameGraphNodeDefinition makeRenderNodeDefinition(const PassDefinition &pass, size_t declaration_index) {
    FrameGraphNodeDefinition node;
    node.name = pass.name;
    node.kind = passKind(pass);
    node.declaration_index = declaration_index;
    for (size_t i = 0; i < pass.input_targets.size(); ++i) {
        if (pass.input_target_history.at(i)) {
            appendUnique(node.reads_history, renderTargetResourceName(pass.input_targets[i]));
        } else {
            appendUnique(node.reads, renderTargetResourceName(pass.input_targets[i]));
        }
    }
    appendUnique(node.reads, pass.input_buffers);
    for (const auto target : pass.output_color) {
        appendUnique(node.writes, renderTargetResourceName(target));
    }
    appendUnique(node.writes, renderTargetResourceName(pass.output_depth));

    if (pass.color_load_op == vk::AttachmentLoadOp::eLoad) {
        for (const auto target : pass.output_color) {
            appendUnique(node.reads, renderTargetResourceName(target));
        }
    }
    if (pass.depth_load_op == vk::AttachmentLoadOp::eLoad) {
        appendUnique(node.reads, renderTargetResourceName(pass.output_depth));
    }
    return node;
}

void validateUniqueNames(const FrameGraphDefinition &definition) {
    std::unordered_set<std::string> names;
    for (const auto &node : definition.nodes) {
        if (node.name.empty()) {
            throw std::runtime_error("Frame graph node name must not be empty");
        }
        if (!names.insert(node.name).second) {
            throw std::runtime_error("Duplicate frame graph node name: " + node.name);
        }
    }
}

std::unordered_map<std::string, size_t> buildNodeIndex(const FrameGraphDefinition &definition) {
    std::unordered_map<std::string, size_t> node_index;
    for (size_t i = 0; i < definition.nodes.size(); ++i) {
        node_index.emplace(definition.nodes[i].name, i);
    }
    return node_index;
}

std::unordered_set<std::string> buildKnownResources(const FrameGraphDefinition &definition) {
    std::unordered_set<std::string> resources;
    resources.insert("swapchain");
    for (const auto &resource : definition.declared_resources) {
        if (!resource.empty()) {
            resources.insert(resource);
        }
    }
    for (const auto &node : definition.nodes) {
        for (const auto &resource : node.writes) {
            if (!resource.empty()) {
                resources.insert(resource);
            }
        }
    }
    return resources;
}

void validateKnownResources(const FrameGraphDefinition &definition) {
    const auto resources = buildKnownResources(definition);
    for (const auto &node : definition.nodes) {
        for (const auto &resource : node.reads) {
            if (resource.empty()) {
                continue;
            }
            if (resources.find(resource) == resources.end()) {
                throw std::runtime_error("Unknown resource reference in frame graph node " + node.name + ": " +
                                         resource);
            }
        }
        for (const auto &resource : node.reads_history) {
            if (std::find(definition.history_resources.begin(), definition.history_resources.end(),
                          resource) == definition.history_resources.end()) {
                throw std::runtime_error("Unknown or non-history resource reference in frame graph node " +
                                         node.name + ": " + resource + "@history");
            }
        }
    }
}

void addEdge(PlannerEdges &planner_edges, size_t from, size_t to) {
    if (from == to) {
        throw std::runtime_error("Frame graph contains a self dependency");
    }
    if (!planner_edges.exists[from][to]) {
        planner_edges.exists[from][to] = true;
        planner_edges.edges.push_back(Edge{from, to});
    }
}

void addDataEdge(PlannerEdges &planner_edges, size_t from, size_t to, const std::string &resource,
                 const FrameGraphDefinition &definition) {
    addEdge(planner_edges, from, to);
    const auto barrier = FramePlanBarrier{
        "read_after_write",
        resource,
        definition.nodes[from].name,
        definition.nodes[to].name,
    };
    const auto exists = std::find_if(planner_edges.barriers.begin(), planner_edges.barriers.end(),
                                     [&barrier](const auto &existing) {
                                         return existing.kind == barrier.kind &&
                                                existing.resource == barrier.resource &&
                                                existing.from == barrier.from &&
                                                existing.to == barrier.to;
                                     });
    if (exists == planner_edges.barriers.end()) {
        planner_edges.barriers.push_back(barrier);
    }
}

void addBarriersForOrderedResourceEdges(PlannerEdges &planner_edges, const FrameGraphDefinition &definition) {
    for (const auto &edge : planner_edges.edges) {
        const auto &from = definition.nodes[edge.from];
        const auto &to = definition.nodes[edge.to];
        for (const auto &written : from.writes) {
            if (written.empty() || std::find(to.reads.begin(), to.reads.end(), written) == to.reads.end()) {
                continue;
            }
            addDataEdge(planner_edges, edge.from, edge.to, written, definition);
        }
    }
}

PlannerEdges buildEdges(const FrameGraphDefinition &definition,
                        const std::unordered_map<std::string, size_t> &node_index) {
    PlannerEdges planner_edges;
    planner_edges.exists.assign(definition.nodes.size(), std::vector<bool>(definition.nodes.size(), false));

    std::unordered_map<std::string, size_t> last_writer;
    for (size_t i = 0; i < definition.nodes.size(); ++i) {
        const auto &node = definition.nodes[i];
        for (const auto &resource : node.reads) {
            auto found = last_writer.find(resource);
            if (found != last_writer.end()) {
                addDataEdge(planner_edges, found->second, i, resource, definition);
            }
        }
        for (const auto &resource : node.writes) {
            if (!resource.empty()) {
                last_writer[resource] = i;
            }
        }
    }

    for (size_t i = 0; i < definition.nodes.size(); ++i) {
        const auto &node = definition.nodes[i];
        for (const auto &after : node.after) {
            const auto found = node_index.find(after);
            if (found == node_index.end()) {
                throw std::runtime_error("Frame graph after reference not found: " + after);
            }
            addEdge(planner_edges, found->second, i);
        }
        for (const auto &before : node.before) {
            const auto found = node_index.find(before);
            if (found == node_index.end()) {
                throw std::runtime_error("Frame graph before reference not found: " + before);
            }
            addEdge(planner_edges, i, found->second);
        }
    }

    addBarriersForOrderedResourceEdges(planner_edges, definition);
    return planner_edges;
}

std::vector<std::vector<bool>> transitiveClosure(std::vector<std::vector<bool>> reachability) {
    const auto count = reachability.size();
    for (size_t k = 0; k < count; ++k) {
        for (size_t i = 0; i < count; ++i) {
            if (!reachability[i][k]) {
                continue;
            }
            for (size_t j = 0; j < count; ++j) {
                reachability[i][j] = reachability[i][j] || reachability[k][j];
            }
        }
    }
    return reachability;
}

void validateWritesAreOrdered(const FrameGraphDefinition &definition,
                              const std::vector<std::vector<bool>> &reachability) {
    std::unordered_map<std::string, std::vector<size_t>> writers_by_resource;
    for (size_t i = 0; i < definition.nodes.size(); ++i) {
        for (const auto &resource : definition.nodes[i].writes) {
            if (!resource.empty()) {
                writers_by_resource[resource].push_back(i);
            }
        }
    }

    for (const auto &[resource, writers] : writers_by_resource) {
        for (size_t i = 0; i < writers.size(); ++i) {
            for (size_t j = i + 1; j < writers.size(); ++j) {
                const auto lhs = writers[i];
                const auto rhs = writers[j];
                if (!reachability[lhs][rhs] && !reachability[rhs][lhs]) {
                    throw std::runtime_error("Ambiguous writes-writes dependency for resource " + resource +
                                             " between " + definition.nodes[lhs].name + " and " +
                                             definition.nodes[rhs].name);
                }
            }
        }
    }
}

std::vector<size_t> topologicalOrder(const FrameGraphDefinition &definition, const PlannerEdges &planner_edges) {
    const auto count = definition.nodes.size();
    std::vector<size_t> indegree(count, 0);
    std::vector<std::vector<size_t>> adjacency(count);
    for (const auto &edge : planner_edges.edges) {
        adjacency[edge.from].push_back(edge.to);
        ++indegree[edge.to];
    }

    std::set<std::pair<size_t, size_t>> ready;
    for (size_t i = 0; i < count; ++i) {
        if (indegree[i] == 0) {
            ready.insert({definition.nodes[i].declaration_index, i});
        }
    }

    std::vector<size_t> order;
    order.reserve(count);
    while (!ready.empty()) {
        const auto [unused_index, node_index] = *ready.begin();
        (void)unused_index;
        ready.erase(ready.begin());
        order.push_back(node_index);
        for (const auto next : adjacency[node_index]) {
            --indegree[next];
            if (indegree[next] == 0) {
                ready.insert({definition.nodes[next].declaration_index, next});
            }
        }
    }

    if (order.size() != count) {
        throw std::runtime_error("Cycle detected in frame graph: " + definition.name);
    }
    return order;
}

std::vector<size_t> computeLevels(size_t node_count, const PlannerEdges &planner_edges,
                                  const std::vector<size_t> &order) {
    std::vector<size_t> levels(node_count, 0);
    for (const auto node_index : order) {
        for (const auto &edge : planner_edges.edges) {
            if (edge.to == node_index) {
                levels[node_index] = std::max(levels[node_index], levels[edge.from] + 1);
            }
        }
    }
    return levels;
}

} // namespace

std::string framePlanNodeKindName(FramePlanNodeKind kind) {
    switch (kind) {
    case FramePlanNodeKind::render:
        return "render";
    case FramePlanNodeKind::compute:
        return "compute";
    case FramePlanNodeKind::anchor:
        return "anchor";
    case FramePlanNodeKind::snapshot_copy:
        return "snapshot_copy";
    case FramePlanNodeKind::output_transform:
        return "output_transform";
    }
    throw std::runtime_error("unknown frame plan node kind");
}

FrameGraphDefinition makeFrameGraphDefinition(const RenderingPassDefinition &definition) {
    FrameGraphDefinition graph;
    graph.name = definition.name;
    graph.nodes.reserve(definition.passes.size());
    for (size_t i = 0; i < definition.passes.size(); ++i) {
        auto node = makeRenderNodeDefinition(definition.passes[i], i);
        appendUnique(graph.history_resources, node.reads_history);
        graph.nodes.push_back(std::move(node));
    }
    return graph;
}

FrameGraphDefinition parseFrameGraphDefinitionFromJson(const nlohmann::json &graph_json) {
    if (!graph_json.is_object()) {
        throw std::runtime_error("Frame graph definition must be an object");
    }

    FrameGraphDefinition graph;
    graph.name = graph_json.value("name", std::string{"frame_graph"});
    appendUnique(graph.declared_resources, parseDeclaredRenderTargets(graph_json));
    appendUnique(graph.history_resources, parseHistoryRenderTargets(graph_json));
    appendUnique(graph.declared_resources, parseDeclaredBuffers(graph_json));
    size_t declaration_index = 0;
    appendNodes(graph.nodes, parseRenderNodes(graph_json, declaration_index));
    appendNodes(graph.nodes, parseComputeNodes(graph_json, declaration_index));
    const auto local_sizes = parseRenderTargetByteSizes(graph_json);
    if (!local_sizes.empty()) assignSnapshotByteSizes(graph.nodes, local_sizes);
    return graph;
}

std::vector<FrameGraphDefinition> parseFrameGraphDefinitionsFromConfigJson(const nlohmann::json &config_json) {
    if (!config_json.is_object()) {
        throw std::runtime_error("Frame graph config must be an object");
    }

    std::vector<FrameGraphDefinition> graphs;
    if (!config_json.contains("rendering_passes")) {
        if (config_json.contains("passes") || config_json.contains("compute_tasks")) {
            graphs.push_back(parseFrameGraphDefinitionFromJson(config_json));
        }
        return graphs;
    }

    const auto &rendering_passes = config_json.at("rendering_passes");
    if (!rendering_passes.is_array()) {
        throw std::runtime_error("Frame graph rendering_passes must be an array");
    }

    const auto declared_targets = parseDeclaredRenderTargets(config_json);
    const auto history_targets = parseHistoryRenderTargets(config_json);
    const auto declared_buffers = parseDeclaredBuffers(config_json);
    const auto render_target_sizes = parseRenderTargetByteSizes(config_json);
    for (const auto &pass_set_json : rendering_passes) {
        auto graph = parseFrameGraphDefinitionFromJson(pass_set_json);
        appendUnique(graph.declared_resources, declared_targets);
        appendUnique(graph.history_resources, history_targets);
        appendUnique(graph.declared_resources, declared_buffers);
        size_t declaration_index = graph.nodes.size();
        appendNodes(graph.nodes, parseComputeNodes(config_json, declaration_index));
        assignSnapshotByteSizes(graph.nodes, render_target_sizes);
        graphs.push_back(std::move(graph));
    }

    if (graphs.empty() && config_json.contains("compute_tasks")) {
        FrameGraphDefinition graph;
        graph.name = "frame_graph";
        appendUnique(graph.declared_resources, declared_targets);
        appendUnique(graph.history_resources, history_targets);
        appendUnique(graph.declared_resources, declared_buffers);
        size_t declaration_index = 0;
        appendNodes(graph.nodes, parseComputeNodes(config_json, declaration_index));
        graphs.push_back(std::move(graph));
    }
    return graphs;
}

FramePlan planFrameGraph(const FrameGraphDefinition &definition) {
    validateUniqueNames(definition);
    validateKnownResources(definition);
    const auto node_index = buildNodeIndex(definition);
    const auto planner_edges = buildEdges(definition, node_index);
    const auto reachability = transitiveClosure(planner_edges.exists);
    validateWritesAreOrdered(definition, reachability);
    const auto order = topologicalOrder(definition, planner_edges);
    const auto levels_by_node = computeLevels(definition.nodes.size(), planner_edges, order);

    FramePlan plan;
    plan.name = definition.name;
    plan.barriers = planner_edges.barriers;
    plan.nodes.reserve(order.size());

    size_t max_level = 0;
    for (size_t order_index = 0; order_index < order.size(); ++order_index) {
        const auto node_index_value = order[order_index];
        const auto &node_def = definition.nodes[node_index_value];
        const auto level = levels_by_node[node_index_value];
        max_level = std::max(max_level, level);
        plan.nodes.push_back(FramePlanNode{
            node_def.name,
            node_def.kind,
            node_def.declaration_index,
            order_index,
            level,
            node_def.reads,
            node_def.reads_history,
            node_def.writes,
            node_def.snapshot_after,
            node_def.byte_size,
        });
    }

    plan.levels.resize(max_level + 1);
    for (const auto &node : plan.nodes) {
        plan.levels[node.level].push_back(node.name);
    }
    return plan;
}

std::vector<std::string> framePlanOrder(const FramePlan &plan) {
    std::vector<std::string> order;
    order.reserve(plan.nodes.size());
    for (const auto &node : plan.nodes) {
        order.push_back(node.name);
    }
    return order;
}

nlohmann::json framePlanToJson(const FramePlan &plan) {
    auto nodes_json = nlohmann::json::array();
    for (const auto &node : plan.nodes) {
        auto node_json = nlohmann::json{
            {"declaration_index", node.declaration_index},
            {"kind", framePlanNodeKindName(node.kind)},
            {"level", node.level},
            {"name", node.name},
            {"order", node.order},
            {"reads", node.reads},
            {"reads_history", node.reads_history},
            {"writes", node.writes},
        };
        if (node.kind == FramePlanNodeKind::snapshot_copy) {
            node_json["byte_size"] = node.byte_size;
            node_json["snapshot_after"] = node.snapshot_after;
            node_json["semantics"] = "fixed_once_before_transparency";
            node_json["sequential_refraction"] = false;
        }
        nodes_json.push_back(std::move(node_json));
    }

    auto barriers_json = nlohmann::json::array();
    for (const auto &barrier : plan.barriers) {
        barriers_json.push_back(nlohmann::json{
            {"from", barrier.from},
            {"kind", barrier.kind},
            {"resource", barrier.resource},
            {"to", barrier.to},
        });
    }

    auto result = nlohmann::json{
        {"barriers", barriers_json},
        {"graph", plan.name},
        {"levels", plan.levels},
        {"nodes", nodes_json},
        {"schema", "pelican.frame_plan"},
        {"version", 1},
    };
    if (plan.composition_metadata.is_object()) {
        for (auto field = plan.composition_metadata.begin();
             field != plan.composition_metadata.end(); ++field) {
            result[field.key()] = field.value();
        }
    }
    return result;
}

} // namespace Pelican
