#include "frameplan.hpp"

#include "graphcomposition.hpp"

#include <algorithm>
#include <deque>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace Pelican {

namespace {

struct EdgeOrigin {
    std::string_view reason;
    std::string resource;
    // Node index, then reads / after / before entry within that node.
    // Planner insertion order is deliberately different: all reads first.
    std::pair<size_t, size_t> diagnostic_order;
};

struct Edge {
    size_t from = 0;
    size_t to = 0;
    std::vector<EdgeOrigin> origins;
};

struct PlannerEdges {
    // Lookup only: iteration order comes from edges, never these maps.
    std::vector<std::unordered_map<size_t, size_t>> edge_by_target;
    std::vector<Edge> edges;
    std::vector<FramePlanBarrier> barriers;
};

// The planner's refusals, by name (GraphRegistryError: a code and the
// subject's names).
[[noreturn]] void refuse(std::string_view code, nlohmann::json detail) {
    throw GraphRegistryError(std::string{code}, std::move(detail));
}

void validateUniqueNames(const FrameGraphDefinition &definition) {
    std::unordered_set<std::string> names;
    for (const auto &node : definition.nodes) {
        if (node.name.empty()) {
            refuse("render_graph.node_name_empty@1",
                {{"graph", definition.name}});
        }
        if (!names.insert(node.name).second) {
            refuse("render_graph.duplicate_node@1",
                {{"graph", definition.name},
                             {"node", node.name}});
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
                refuse("render_graph.unknown_resource@1",
                {{"graph", definition.name},
                     {"node", node.name},
                     {"resource", resource}});
            }
        }
        for (const auto &resource : node.reads_history) {
            if (std::find(definition.history_resources.begin(), definition.history_resources.end(),
                          resource) == definition.history_resources.end()) {
                refuse("render_graph.unknown_history_resource@1",
                {{"graph", definition.name},
                     {"node", node.name},
                     {"resource", resource}});
            }
        }
    }
}

void addEdge(PlannerEdges &planner_edges, size_t from, size_t to,
             const FrameGraphDefinition &definition, EdgeOrigin origin,
             bool validate_references) {
    if (from == to) {
        if (!validate_references) return;
        refuse("render_graph.self_dependency@1",
                {{"graph", definition.name},
                         {"node", definition.nodes[from].name}});
    }
    const auto [entry, inserted] =
        planner_edges.edge_by_target[from].emplace(to, planner_edges.edges.size());
    if (inserted) planner_edges.edges.push_back(Edge{from, to, {}});
    planner_edges.edges[entry->second].origins.push_back(std::move(origin));
}

void addBarrier(PlannerEdges &planner_edges, std::string kind,
                size_t from, size_t to, const std::string &resource,
                const FrameGraphDefinition &definition) {
    const auto barrier = FramePlanBarrier{
        std::move(kind),
        resource,
        definition.nodes[from].name,
        definition.nodes[to].name,
    };
    const auto exists = std::find_if(
        planner_edges.barriers.begin(), planner_edges.barriers.end(),
        [&barrier](const auto &existing) {
            return existing.resource == barrier.resource &&
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
            // This is a barrier on an existing ordered pair, not another
            // declaration-order read dependency (or diagnostic origin).
            addBarrier(planner_edges, "read_after_write", edge.from, edge.to, written, definition);
        }
    }
}

// Planning refuses unknown / self references and records the barriers; the
// order-conflict diagnostic needs neither (structure validation names the bad
// references, and barriers are not part of its detail).
enum class EdgeUse { planning, diagnostics };

PlannerEdges buildEdges(const FrameGraphDefinition &definition,
                        const std::unordered_map<std::string, size_t> &node_index,
                        EdgeUse use = EdgeUse::planning) {
    const bool validate_references = use == EdgeUse::planning;
    const bool record_barriers = use == EdgeUse::planning;
    PlannerEdges planner_edges;
    planner_edges.edge_by_target.resize(definition.nodes.size());

    std::unordered_map<std::string, size_t> last_writer;
    for (size_t i = 0; i < definition.nodes.size(); ++i) {
        const auto &node = definition.nodes[i];
        for (size_t entry = 0; entry < node.reads.size(); ++entry) {
            const auto &resource = node.reads[entry];
            auto found = last_writer.find(resource);
            if (found != last_writer.end()) {
                addEdge(planner_edges, found->second, i, definition,
                        {"reads", resource, {i, entry}}, validate_references);
                if (record_barriers)
                    addBarrier(planner_edges, "read_after_write", found->second, i, resource, definition);
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
        for (size_t entry = 0; entry < node.after.size(); ++entry) {
            const auto &after = node.after[entry];
            const auto found = node_index.find(after);
            if (found == node_index.end()) {
                if (!validate_references) continue;
                refuse("render_graph.order_reference_unknown@1",
                {{"graph", definition.name},
                                 {"node", node.name},
                                 {"field", "after"},
                                 {"reference", after}});
            }
            addEdge(planner_edges, found->second, i, definition,
                    {"after", {}, {i, node.reads.size() + entry}}, validate_references);
        }
        for (size_t entry = 0; entry < node.before.size(); ++entry) {
            const auto &before = node.before[entry];
            const auto found = node_index.find(before);
            if (found == node_index.end()) {
                if (!validate_references) continue;
                refuse("render_graph.order_reference_unknown@1",
                {{"graph", definition.name},
                                 {"node", node.name},
                                 {"field", "before"},
                                 {"reference", before}});
            }
            addEdge(planner_edges, i, found->second, definition,
                    {"before", {}, {i, node.reads.size() + node.after.size() + entry}}, validate_references);
        }
    }

    if (record_barriers) addBarriersForOrderedResourceEdges(planner_edges, definition);
    return planner_edges;
}

std::vector<std::vector<bool>> transitiveClosure(const PlannerEdges &planner_edges) {
    const auto count = planner_edges.edge_by_target.size();
    std::vector<std::vector<bool>> reachability(count, std::vector<bool>(count, false));
    for (const auto &edge : planner_edges.edges) reachability[edge.from][edge.to] = true;
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
                    refuse("render_graph.ambiguous_writes@1",
                {{"graph", definition.name},
                         {"resource", resource},
                         {"first", definition.nodes[lhs].name},
                         {"second", definition.nodes[rhs].name}});
                }
            }
        }
    }
}

void addWriteAfterWriteBarriers(
    PlannerEdges &planner_edges,
    const FrameGraphDefinition &definition,
    const std::vector<size_t> &order) {
    std::unordered_map<std::string, size_t> last_writer;
    for (const auto node_index : order) {
        for (const auto &resource :
             definition.nodes[node_index].writes) {
            if (resource.empty()) continue;
            const auto previous = last_writer.find(resource);
            if (previous != last_writer.end()) {
                // Writer ordering is validated before this point. The edge
                // may be transitive, but Vulkan still needs an explicit
                // memory dependency between consecutive physical writers.
                addBarrier(planner_edges, "write_after_write",
                           previous->second, node_index, resource,
                           definition);
            }
            last_writer[resource] = node_index;
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
        refuse("render_graph.cycle@1",
                {{"graph", definition.name}});
    }
    return order;
}

// The rules a graph holds whatever its final order: names, known resources,
// the after / before references, no cycle. Two unordered writers are judged
// once the order is final (planWith).
void requireStructure(const FrameGraphDefinition &definition) {
    validateUniqueNames(definition);
    validateKnownResources(definition);
    const auto planner_edges = buildEdges(definition, buildNodeIndex(definition));
    (void)topologicalOrder(definition, planner_edges);
}

FramePlan planWith(const FrameGraphDefinition &definition) {
    validateUniqueNames(definition);
    validateKnownResources(definition);
    const auto node_index = buildNodeIndex(definition);
    auto planner_edges = buildEdges(definition, node_index);
    const auto reachability = transitiveClosure(planner_edges);
    validateWritesAreOrdered(definition, reachability);
    const auto order = topologicalOrder(definition, planner_edges);
    addWriteAfterWriteBarriers(planner_edges, definition, order);

    FramePlan plan;
    plan.name = definition.name;
    plan.barriers = planner_edges.barriers;
    plan.nodes.reserve(order.size());

    for (size_t order_index = 0; order_index < order.size(); ++order_index) {
        const auto node_index_value = order[order_index];
        const auto &node_def = definition.nodes[node_index_value];
        plan.nodes.push_back(FramePlanNode{
            node_def.name,
            node_def.kind,
            node_def.declaration_index,
            order_index,
            node_def.reads,
            node_def.reads_history,
            node_def.writes,
            node_def.snapshot_after,
            node_def.byte_size,
            node_def.material_filter,
            node_def.material_variant,
            node_def.view_family,
            node_def.view_select,
            node_def.once,
        });
    }
    return plan;
}

} // namespace

std::vector<FramePlanUnreadOutput> findFramePlanUnreadOutputs(
    const FramePlan &plan,
    const std::unordered_map<std::string, std::vector<std::string>>
        &image_usage) {
    const auto has = [](const std::vector<std::string> &names,
                        std::string_view name) {
        return std::find(names.begin(), names.end(), name) != names.end();
    };
    std::vector<const FramePlanNode *> nodes;
    nodes.reserve(plan.nodes.size());
    std::set<std::string, std::less<>> read_as_history;
    for (const auto &node : plan.nodes) {
        nodes.push_back(&node);
        read_as_history.insert(node.reads_history.begin(),
                               node.reads_history.end());
    }
    std::stable_sort(nodes.begin(), nodes.end(),
                     [](const FramePlanNode *left, const FramePlanNode *right) {
                         return left->order < right->order;
                     });

    std::vector<FramePlanUnreadOutput> result;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto &node = *nodes[index];
        std::set<std::string, std::less<>> reported;
        for (const auto &resource : node.writes) {
            if (resource.empty() || resource == "display" ||
                reported.contains(resource)) {
                continue;
            }
            const auto usage = image_usage.find(resource);
            if (usage == image_usage.end() ||
                has(usage->second, "TRANSFER_SRC")) {
                continue;
            }
            if (has(usage->second, "DEPTH_STENCIL_ATTACHMENT") &&
                std::any_of(node.writes.begin(), node.writes.end(),
                            [&](const std::string &other) {
                                return !other.empty() && other != resource;
                            })) {
                continue;
            }
            if (read_as_history.contains(resource)) continue;
            const auto consumed = std::any_of(
                nodes.begin() + static_cast<std::ptrdiff_t>(index) + 1,
                nodes.end(), [&](const FramePlanNode *later) {
                    return has(later->reads, resource) ||
                           has(later->writes, resource);
                });
            if (consumed) continue;
            reported.insert(resource);
            result.push_back(FramePlanUnreadOutput{node.name, resource});
        }
    }
    return result;
}

std::string framePlanNodeKindName(FramePlanNodeKind kind) {
    switch (kind) {
    case FramePlanNodeKind::render:
        return "render";
    case FramePlanNodeKind::compute:
        return "compute";
    case FramePlanNodeKind::acceleration_structure:
        return "acceleration_structure";
    case FramePlanNodeKind::anchor:
        return "anchor";
    case FramePlanNodeKind::snapshot_copy:
        return "snapshot_copy";
    case FramePlanNodeKind::output_transform:
        return "output_transform";
    }
    throw std::runtime_error("unknown frame plan node kind");
}

FramePlan planFrameGraph(const FrameGraphDefinition &definition) {
    return planWith(definition);
}

void requireFrameGraphStructure(const FrameGraphDefinition &definition) {
    requireStructure(definition);
}

std::vector<std::string> framePlanOrder(const FramePlan &plan) {
    std::vector<std::string> order;
    order.reserve(plan.nodes.size());
    for (const auto &node : plan.nodes) {
        order.push_back(node.name);
    }
    return order;
}

void validateFramePlanStructure(const FramePlan &plan) {
    const auto refuse = [](std::string_view code,
                           const nlohmann::json &fields) {
        throw std::runtime_error(std::string{code} + ": " +
                                 fields.dump());
    };
    const auto node_count = plan.nodes.size();
    std::unordered_set<std::string_view> names;
    std::vector<bool> order_seen(node_count, false);
    names.reserve(node_count);
    for (const auto &node : plan.nodes) {
        if (!names.insert(node.name).second) {
            refuse(framePlanDuplicateNodeCode,
                   {{"graph", plan.name}, {"node", node.name}});
        }
        if (node.order >= node_count || order_seen[node.order]) {
            refuse(framePlanOrderGapCode,
                   {{"graph", plan.name},
                    {"node", node.name},
                    {"order", node.order},
                    {"node_count", node_count}});
        }
        order_seen[node.order] = true;
    }
    for (std::size_t index = 0; index < node_count; ++index) {
        const auto &node = plan.nodes[index];
        if (node.order != index) {
            refuse(framePlanOrderMismatchCode,
                   {{"graph", plan.name},
                    {"node", node.name},
                    {"order", node.order},
                    {"index", index}});
        }
    }
}

void requireFrameNodeOrderAgreesWithDependencies(const FrameGraphDefinition &graph,
                                                 const std::vector<std::size_t> &listed) {
    if (listed.size() < 2) return;
    // Preserve the diagnostic boundary: unknown/self references are left to
    // structure validation, rather than displacing an order-conflict detail.
    const auto planner_edges = buildEdges(graph, buildNodeIndex(graph), EdgeUse::diagnostics);
    // References into the shared edges and their origins, not rebuilt edges.
    using OriginIndex = std::pair<std::size_t, std::size_t>;
    std::vector<std::vector<OriginIndex>> waits(graph.nodes.size());
    const auto origin = [&](const OriginIndex &index) -> const EdgeOrigin & {
        return planner_edges.edges[index.first].origins[index.second];
    };
    for (std::size_t edge = 0; edge < planner_edges.edges.size(); ++edge) {
        const auto &value = planner_edges.edges[edge];
        for (std::size_t entry = 0; entry < value.origins.size(); ++entry)
            waits[value.from].emplace_back(edge, entry);
    }
    for (auto &outgoing : waits) {
        std::stable_sort(outgoing.begin(), outgoing.end(), [&](const auto &lhs, const auto &rhs) {
            return origin(lhs).diagnostic_order < origin(rhs).diagnostic_order;
        });
    }
    std::unordered_map<std::size_t, std::size_t> position;
    for (std::size_t i = 0; i < listed.size(); ++i) position.emplace(listed[i], i);
    for (std::size_t later = 1; later < listed.size(); ++later) {
        // Breadth first from the node the list puts later: an earlier listed
        // node it reaches must run after it.
        const auto start = listed[later];
        std::vector<std::optional<std::pair<std::size_t, std::size_t>>> came_from(graph.nodes.size());
        std::vector<bool> seen(graph.nodes.size(), false);
        std::deque<std::size_t> frontier{start};
        seen[start] = true;
        std::optional<std::size_t> reached;
        while (!frontier.empty() && !reached) {
            const auto current = frontier.front();
            frontier.pop_front();
            for (std::size_t edge = 0; edge < waits[current].size(); ++edge) {
                const auto next = planner_edges.edges[waits[current][edge].first].to;
                if (seen[next]) continue;
                seen[next] = true;
                came_from[next] = std::pair{current, edge};
                if (const auto found = position.find(next); found != position.end() && found->second < later) {
                    reached = next;
                    break;
                }
                frontier.push_back(next);
            }
        }
        if (!reached) continue;
        std::vector<std::pair<std::size_t, std::size_t>> path; // (from, edge index)
        for (auto at = *reached; at != start; at = came_from[at]->first) path.push_back(*came_from[at]);
        std::reverse(path.begin(), path.end());
        const auto &step = origin(waits[path.front().first][path.front().second]);
        nlohmann::json fields{{"graph", graph.name},
                              {"first", graph.nodes[*reached].name},
                              {"second", graph.nodes[start].name},
                              {"reason", std::string{step.reason}}};
        if (step.reason == "reads") fields["resource"] = step.resource;
        nlohmann::json via = nlohmann::json::array();
        for (std::size_t i = 1; i < path.size(); ++i) {
            const auto &node = graph.nodes[path[i].first];
            via.push_back(node.name);
            if ((node.kind == FramePlanNodeKind::anchor || node.kind == FramePlanNodeKind::output_transform) &&
                fields.at("reason") != "canonical_order") {
                fields["reason"] = "canonical_order";
                fields.erase("resource");
                fields["anchor"] = node.name;
            }
        }
        if (!via.empty()) fields["via"] = std::move(via);
        throw GraphRegistryError("frame_node_order.conflicts_dependency@1", std::move(fields));
    }
}

} // namespace Pelican
