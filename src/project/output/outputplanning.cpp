#include "outputplanning.hpp"

#include "../logicalrendergraph.hpp"
#include "../targetrenderplanning.hpp"

#include <algorithm>
#include <map>
#include <optional>

namespace Pelican {
namespace {

bool writesResource(const LogicalResourceUse &use, std::string_view target) {
    return use.output_value && use.output_value->resource == target &&
           (use.access == LogicalAccessMode::write ||
            use.access == LogicalAccessMode::read_write);
}

bool nodeIs(const TargetLoweringNode &node, std::string_view name) {
    return node.logical.name == name ||
           std::find(node.source_nodes.begin(), node.source_nodes.end(), name) !=
               node.source_nodes.end();
}

} // namespace

void validateOutputSinkWriters(OutputSinkTable &table,
                               const VulkanTargetPlan &plan) {
    if (plan.graph.empty() || plan.lowering_graph.nodes.empty()) return;
    std::map<std::string, const TargetLoweringNode *, std::less<>> by_name;
    for (const auto &node : plan.lowering_graph.nodes)
        by_name.emplace(node.logical.name, &node);
    const auto order = physicalNodeOrderOf(plan.scopes);
    for (auto &sink : table.sinks) {
        if (!sink.enabled || !sink.error_code.empty() ||
            !outputSinkGraphMatches(sink.graph, plan.graph))
            continue;
        bool source_seen = false;
        std::string later_writer;
        for (const auto &node_name : order.nodes) {
            const auto found = by_name.find(node_name);
            if (found == by_name.end()) continue;
            const auto &node = *found->second;
            const bool writes_target =
                std::any_of(node.logical.uses.begin(), node.logical.uses.end(),
                            [&](const LogicalResourceUse &use) {
                                return writesResource(use, sink.target);
                            });
            if (!writes_target) continue;
            if (nodeIs(node, sink.source_node)) {
                source_seen = true;
                continue;
            }
            if (source_seen) {
                later_writer = node.logical.name;
                break;
            }
        }
        // No writer for the concrete target in this plan means the plan does
        // not carry the data to decide; leave the sink alone rather than
        // invent a refusal. A source that is not a pass writer cannot be
        // checked here either; lowering has already refused it.
        if (source_seen && !later_writer.empty()) {
            sink.error_code = "output.source_overwritten@1";
            sink.error_message = "target '" + sink.target +
                                 "' is written after the source node '" +
                                 sink.source_node + "' by '" + later_writer + "'";
        }
    }
}

} // namespace Pelican
