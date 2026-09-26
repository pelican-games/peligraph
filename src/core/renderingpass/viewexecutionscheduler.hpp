#pragma once

#include "framegraphruntime.hpp"
#include "../../project/viewselect.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
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
    std::string view_family{
        mainRenderViewFamilyId};
    // LP-Va: the scope selects family views with view_select and none of
    // this frame's views match. The invocation records nothing; execution
    // treats the node like an inactive one (completed, no scope begun).
    bool no_selected_view = false;

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

struct LogicalFrameViewFamilyCardinality {
    std::string_view family_id;
    std::uint32_t view_count = 1;
    // LP-Va: the axis coordinates of each view in execution order. Required
    // (one entry per view) only when a scope of this family has a
    // view_select; an authored family leaves the entries empty.
    std::span<const RenderViewAxisCoordinates> view_axes{};
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
    std::span<const LogicalFrameViewFamilyCardinality>
        view_families) {
    std::map<std::string_view, std::uint32_t,
             std::less<>>
        view_count_by_family;
    std::map<std::string_view,
             std::span<const RenderViewAxisCoordinates>,
             std::less<>>
        view_axes_by_family;
    for (const auto &family : view_families) {
        view_axes_by_family.emplace(
            family.family_id, family.view_axes);
        validateRenderViewFamilyId(
            family.family_id,
            "logical-frame schedule family");
        if (family.view_count == 0) {
            throw std::runtime_error(
                "logical-frame view-family schedule requires at least "
                "one view in family '" +
                std::string{family.family_id} + "'");
        }
        if (!view_count_by_family
                 .emplace(family.family_id,
                          family.view_count)
                 .second) {
            throw std::runtime_error(
                "logical-frame view-family schedule contains duplicate "
                "family '" +
                std::string{family.family_id} + "'");
        }
    }
    const auto main_family =
        view_count_by_family.find(
            mainRenderViewFamilyId);
    if (main_family ==
        view_count_by_family.end()) {
        throw std::runtime_error(
            "logical-frame view-family schedule requires '$main'");
    }
    const auto logical_view_count =
        main_family->second;
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
    struct ScopeFamily {
        std::string_view family_id;
        std::uint32_t view_count = 1;
        VulkanScopeViewExecution execution =
            VulkanScopeViewExecution::single_view;
        std::uint32_t execution_count = 1;
        // LP-Va: set for a scope with a view_select; holds the matching
        // family view indices in execution order (possibly none).
        std::optional<std::vector<std::uint32_t>>
            selected_views;
    };
    std::vector<ScopeFamily> scope_families;
    scope_families.reserve(
        target_plan.scopes.size());
    for (std::size_t scope_index = 0;
         scope_index < target_plan.scopes.size();
         ++scope_index) {
        const auto &scope = target_plan.scopes[scope_index];
        if (scope.nodes.empty()) {
            throw std::runtime_error(
                "physical target plan contains an empty scope: " +
                scope.id);
        }
        std::optional<std::string_view>
            scope_family_id;
        for (const auto &name : scope.nodes) {
            const auto found =
                node_by_name.find(name);
            if (found == node_by_name.end()) {
                throw std::runtime_error(
                    "physical target plan contains a scope node that "
                    "is absent from the compiled frame graph: " +
                    name);
            }
            const auto &family =
                nodes[found->second].view_family;
            if (scope_family_id &&
                *scope_family_id != family) {
                throw std::runtime_error(
                    "physical target-plan scope mixes view families: " +
                    scope.id);
            }
            scope_family_id = family;
            if (nodes[found->second].view_select !=
                scope.view_select) {
                throw std::runtime_error(
                    "physical target-plan scope view_select does not "
                    "match node '" +
                    name + "': " + scope.id);
            }
        }
        const auto family =
            view_count_by_family.find(
                *scope_family_id);
        if (family ==
            view_count_by_family.end()) {
            throw std::runtime_error(
                "logical frame does not provide view family '" +
                std::string{*scope_family_id} +
                "' required by scope '" + scope.id + "'");
        }
        const bool main_scope =
            *scope_family_id ==
            mainRenderViewFamilyId;
        const auto scope_logical_view_count =
            main_scope
                ? logical_view_count
                : family->second;
        VulkanScopeViewExecution execution =
            scope.view_execution;
        std::uint32_t execution_count = 1;
        if (main_scope) {
            execution_count =
                scope.view_execution ==
                        VulkanScopeViewExecution::sequential
                    ? scope_logical_view_count
                    : 1u;
            const auto expected_view_count =
                scope.view_execution ==
                        VulkanScopeViewExecution::single_view
                    ? 1u
                    : scope_logical_view_count;
            if (scope.execution_count !=
                    execution_count ||
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
        } else {
            // A secondary-family physical scope describes one scalar
            // rendering instance. Runtime family cardinality expands that
            // template sequentially, so the target compiler does not need to
            // know how many shadow/reflection views a provider will supply.
            if (scope.view_execution !=
                    VulkanScopeViewExecution::single_view ||
                scope.view_count != 1 ||
                scope.execution_count != 1 ||
                scope.view_mask != 0) {
                throw std::runtime_error(
                    "secondary view-family physical scope must be a "
                    "single-view template: " +
                    scope.id);
            }
            if (scope_logical_view_count > 1) {
                execution =
                    VulkanScopeViewExecution::sequential;
                execution_count =
                    scope_logical_view_count;
            }
        }
        std::optional<std::vector<std::uint32_t>>
            selected_views;
        if (!scope.view_select.empty()) {
            if (main_scope) {
                throwViewSelectRefusal(
                    viewSelectPrimaryFamilyCode,
                    {{"pass", scope.nodes.front()},
                     {"family", *scope_family_id}});
            }
            // The selection is resolved against this frame's views: a
            // runtime axis such as `light` may match a different count
            // every frame. Views stay addressed by their family index, so
            // layers, frame UBO slots, and culled draws are unchanged.
            // logical_view_count keeps the family size even for one match;
            // single_view would make snapshot_copy use layer 0.
            const auto axes =
                view_axes_by_family.at(*scope_family_id);
            static const RenderViewAxisCoordinates no_axes;
            selected_views.emplace();
            for (std::uint32_t view = 0;
                 view < scope_logical_view_count;
                 ++view) {
                const auto &coordinates =
                    view < axes.size() ? axes[view] : no_axes;
                if (const auto missing = missingViewSelectAxis(
                        coordinates, scope.view_select)) {
                    throwViewSelectRefusal(
                        viewSelectAxisUnknownCode,
                        {{"pass", scope.nodes.front()},
                         {"family", *scope_family_id},
                         {"axis", *missing}});
                }
                if (renderViewMatchesSelect(
                        coordinates, scope.view_select)) {
                    selected_views->push_back(view);
                }
            }
            execution =
                VulkanScopeViewExecution::sequential;
            execution_count = static_cast<std::uint32_t>(
                selected_views->size());
        }
        scope_families.push_back(
            ScopeFamily{
                *scope_family_id,
                scope_logical_view_count,
                execution,
                execution_count,
                std::move(selected_views)});
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
    std::vector<bool> scheduled_nodes(
        nodes.size(), false);
    for (std::size_t scope_index = 0;
         scope_index < target_plan.scopes.size();
         ++scope_index) {
        const auto &scope =
            target_plan.scopes[scope_index];
        const auto &scope_family =
            scope_families[scope_index];
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
            if (scheduled_nodes[found->second]) {
                throw std::runtime_error(
                    "physical target-plan scopes schedule a compiled "
                    "frame-graph node more than once: " +
                    name);
            }
            scheduled_nodes[found->second] = true;
            scope_nodes.push_back(found->second);
        }
        const auto execution_count =
            scope_family.execution_count;
        if (scope_family.selected_views &&
            scope_family.selected_views->empty()) {
            for (std::size_t scope_node_index = 0;
                 scope_node_index < scope_nodes.size();
                 ++scope_node_index) {
                result.push_back(
                    LogicalFrameNodeInvocation{
                        .node_index =
                            scope_nodes[scope_node_index],
                        .scope_index = scope_index,
                        .scope_node_index =
                            scope_node_index,
                        .scope_node_count =
                            scope_nodes.size(),
                        .execution =
                            scope_family.execution,
                        .logical_view_count =
                            scope_family.view_count,
                        .view_index = 0,
                        .execution_index = 0,
                        .execution_count = 1,
                        .view_family =
                            std::string{
                                scope_family.family_id},
                        .no_selected_view = true,
                    });
            }
            continue;
        }
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
                            scope_family.execution,
                        .logical_view_count =
                            scope_family
                                .view_count,
                        .view_index =
                            scope_family.selected_views
                                ? (*scope_family.selected_views)
                                      [execution_index]
                            : scope_family.execution ==
                                    VulkanScopeViewExecution::
                                        sequential
                                ? execution_index
                                : 0u,
                        .execution_index =
                            execution_index,
                        .execution_count =
                            execution_count,
                        .view_family =
                            std::string{
                                scope_family
                                    .family_id},
                    });
            }
        }
    }
    for (std::size_t node_index = 0;
         node_index < nodes.size(); ++node_index) {
        if (!scheduled_nodes[node_index]) {
            throw std::runtime_error(
                "physical target plan has no scope for frame-graph "
                "node: " +
                nodes[node_index].name);
        }
    }
    return result;
}

inline std::vector<LogicalFrameNodeInvocation>
buildLogicalFrameViewFamilySchedule(
    std::span<const FrameGraphExecutionNode> nodes,
    const VulkanTargetPlan &target_plan,
    std::uint32_t logical_view_count) {
    const std::array families{
        LogicalFrameViewFamilyCardinality{
            mainRenderViewFamilyId,
            logical_view_count}};
    return buildLogicalFrameViewFamilySchedule(
        nodes, target_plan, families);
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
        const bool main_family =
            invocation.view_family ==
            mainRenderViewFamilyId;
        if (main_family &&
            invocation.logical_view_count !=
                logical_view_count) {
            throw std::runtime_error(
                "per-view schedule mixes logical-view cardinalities");
        }
        if (!main_family) {
            if (view_index == 0) {
                result.push_back(
                    invocation);
            }
            continue;
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
