#pragma once

#include "framegraphruntime.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// One command-recording invocation produced from the physical scope plan.
// Scheduling is scope-execution-major: every node in a fused scope records for
// one view before the next sequential view begins. This is required to keep a
// native dynamic-rendering scope open across all of its nodes.
struct LogicalFrameNodeInvocation {
    std::size_t node_index = 0;
    std::size_t scope_index = 0;
    std::size_t scope_node_index = 0;
    std::size_t scope_node_count = 1;
    VulkanScopeViewExecution execution =
        VulkanScopeViewExecution::single_view;
    std::uint32_t logical_view_count = 1;
    std::uint32_t view_index = 0;
    std::uint32_t execution_index = 0;
    std::uint32_t execution_count = 1;

    bool firstExecution() const noexcept {
        return execution_index == 0;
    }
    bool lastExecution() const noexcept {
        return execution_index + 1 == execution_count;
    }
    bool beginsScopeExecution() const noexcept {
        return scope_node_index == 0;
    }
    bool endsScopeExecution() const noexcept {
        return scope_node_index + 1 ==
               scope_node_count;
    }
};

// Maps one logical view execution onto source and destination image-array
// ranges. It is shared by external depth today and remains generic for future
// compositor/video exports.
struct ViewLayerCopyPlan {
    std::uint32_t source_base_array_layer = 0;
    std::uint32_t destination_base_array_layer = 0;
    std::uint32_t array_layers = 1;

    bool operator==(const ViewLayerCopyPlan &) const =
        default;
};

inline ViewLayerCopyPlan planViewLayerCopy(
    std::uint32_t source_array_layers,
    std::uint32_t destination_base_array_layer,
    std::uint32_t destination_array_layers,
    std::uint32_t view_index,
    std::uint32_t logical_view_count,
    bool view_family) {
    if (source_array_layers == 0 ||
        destination_array_layers == 0 ||
        logical_view_count == 0 ||
        view_index >= logical_view_count) {
        throw std::runtime_error(
            "view-layer copy has an invalid image or logical-view "
            "range");
    }
    if (view_family) {
        if (source_array_layers <
                logical_view_count ||
            destination_array_layers <
                logical_view_count) {
            throw std::runtime_error(
                "view-family copy does not preserve every logical "
                "view layer");
        }
        return {
            .source_base_array_layer = 0,
            .destination_base_array_layer =
                destination_base_array_layer,
            .array_layers = logical_view_count,
        };
    }
    return {
        .source_base_array_layer =
            source_array_layers >= logical_view_count
                ? view_index
                : 0u,
        .destination_base_array_layer =
            destination_base_array_layer,
        .array_layers = 1,
    };
}

inline std::vector<LogicalFrameNodeInvocation>
buildLogicalFrameViewFamilySchedule(
    std::span<const FrameGraphExecutionNode> nodes,
    const VulkanTargetPlan &target_plan,
    std::uint32_t logical_view_count) {
    if (logical_view_count == 0) {
        throw std::runtime_error(
            "logical-frame view-family schedule requires at least one view");
    }
    if (target_plan.view_execution_plan.view_count !=
        logical_view_count) {
        throw std::runtime_error(
            "logical-frame view-family schedule does not match the "
            "physical target-plan view count");
    }

    std::map<std::string_view, std::size_t, std::less<>>
        node_by_name;
    for (std::size_t node_index = 0;
         node_index < nodes.size(); ++node_index) {
        if (!node_by_name
                 .emplace(nodes[node_index].name,
                          node_index)
                 .second) {
            throw std::runtime_error(
                "compiled frame graph contains a duplicate node: " +
                nodes[node_index].name);
        }
    }

    std::map<std::string_view, std::size_t, std::less<>>
        scope_by_node;
    for (std::size_t scope_index = 0;
         scope_index < target_plan.scopes.size();
         ++scope_index) {
        const auto &scope = target_plan.scopes[scope_index];
        const auto expected_execution_count =
            scope.view_execution ==
                    VulkanScopeViewExecution::sequential
                ? logical_view_count
                : 1u;
        const auto expected_view_count =
            scope.view_execution ==
                    VulkanScopeViewExecution::single_view
                ? 1u
                : logical_view_count;
        if (scope.execution_count !=
                expected_execution_count ||
            scope.view_count != expected_view_count ||
            (scope.view_execution ==
                     VulkanScopeViewExecution::multiview
                 ? scope.view_mask == 0
                 : scope.view_mask != 0)) {
            throw std::runtime_error(
                "physical scope has an inconsistent view-execution "
                "contract: " +
                scope.id);
        }
        for (const auto &node : scope.nodes) {
            if (!scope_by_node.emplace(node, scope_index).second) {
                throw std::runtime_error(
                    "physical target plan assigns a node to more than "
                    "one scope: " +
                    node);
            }
        }
    }

    std::vector<LogicalFrameNodeInvocation> result;
    std::size_t expected_node_index = 0;
    for (std::size_t scope_index = 0;
         scope_index < target_plan.scopes.size();
         ++scope_index) {
        const auto &scope =
            target_plan.scopes[scope_index];
        std::vector<std::size_t> scope_nodes;
        scope_nodes.reserve(scope.nodes.size());
        for (const auto &name : scope.nodes) {
            const auto found = node_by_name.find(name);
            if (found == node_by_name.end()) {
                throw std::runtime_error(
                    "physical target plan contains a scope node that "
                    "is absent from the compiled frame graph: " +
                    name);
            }
            if (found->second != expected_node_index) {
                throw std::runtime_error(
                    "physical target-plan scopes are not an exact "
                    "ordered partition of the compiled frame graph at: " +
                    name);
            }
            scope_nodes.push_back(found->second);
            ++expected_node_index;
        }
        const auto execution_count =
            scope.view_execution ==
                    VulkanScopeViewExecution::sequential
                ? logical_view_count
                : 1u;
        for (std::uint32_t execution_index = 0;
             execution_index < execution_count;
             ++execution_index) {
            for (std::size_t scope_node_index = 0;
                 scope_node_index < scope_nodes.size();
                 ++scope_node_index) {
                result.push_back(
                    LogicalFrameNodeInvocation{
                        .node_index =
                            scope_nodes[
                                scope_node_index],
                        .scope_index = scope_index,
                        .scope_node_index =
                            scope_node_index,
                        .scope_node_count =
                            scope_nodes.size(),
                        .execution =
                            scope.view_execution,
                        .logical_view_count =
                            logical_view_count,
                        .view_index =
                            scope.view_execution ==
                                    VulkanScopeViewExecution::
                                        sequential
                                ? execution_index
                                : 0u,
                        .execution_index =
                            execution_index,
                        .execution_count =
                            execution_count,
                    });
            }
        }
    }
    if (expected_node_index != nodes.size()) {
        const auto &node =
            nodes[expected_node_index];
        if (!scope_by_node.contains(node.name)) {
            throw std::runtime_error(
                "physical target plan has no scope for frame-graph "
                "node: " +
                node.name);
        }
        throw std::runtime_error(
            "physical target-plan scopes are not an exact ordered "
            "partition of the compiled frame graph");
    }
    return result;
}

// Selects the scope-complete command-recording work for one view when the
// logical-frame target acquires and submits each view separately. Multiview
// work cannot be represented by that target contract and must use the full
// view-family schedule instead.
inline std::vector<LogicalFrameNodeInvocation>
selectLogicalFrameSequentialViewSchedule(
    std::span<const LogicalFrameNodeInvocation> schedule,
    std::uint32_t view_index,
    std::uint32_t logical_view_count) {
    if (logical_view_count == 0 ||
        view_index >= logical_view_count) {
        throw std::runtime_error(
            "per-view schedule selected an invalid logical view");
    }

    std::vector<LogicalFrameNodeInvocation> result;
    for (const auto &invocation : schedule) {
        if (invocation.logical_view_count !=
            logical_view_count) {
            throw std::runtime_error(
                "per-view schedule mixes logical-view cardinalities");
        }
        switch (invocation.execution) {
        case VulkanScopeViewExecution::single_view:
            if (view_index == 0) {
                result.push_back(invocation);
            }
            break;
        case VulkanScopeViewExecution::sequential:
            if (invocation.view_index == view_index) {
                result.push_back(invocation);
            }
            break;
        case VulkanScopeViewExecution::multiview:
            throw std::runtime_error(
                "per-view target cannot execute a multiview scope");
        }
    }
    return result;
}

} // namespace Pelican
