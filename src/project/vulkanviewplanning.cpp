#include "vulkanviewplanning.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {

void requireNonEmpty(std::string_view value,
                     std::string_view subject) {
    if (value.empty()) {
        throw std::runtime_error(
            std::string{subject} + " must not be empty");
    }
}

bool hasCapability(const TargetEndpoint &endpoint,
                   std::string_view capability) {
    return std::find(endpoint.capabilities.begin(),
                     endpoint.capabilities.end(),
                     capability) != endpoint.capabilities.end();
}

std::optional<std::uint32_t> endpointUnsignedFact(
    const TargetEndpoint &endpoint, std::string_view name) {
    const auto found = std::find_if(
        endpoint.facts.begin(), endpoint.facts.end(),
        [&](const TargetFact &fact) {
            return fact.name == name;
        });
    if (found == endpoint.facts.end()) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    const auto *begin = found->value.data();
    const auto *end = begin + found->value.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        throw std::runtime_error(
            "target endpoint fact '" + found->name +
            "' must be an unsigned integer on endpoint '" +
            endpoint.id + "'");
    }
    return value;
}

bool isViewDependentNode(LogicalGraphNodeKind kind) {
    return kind == LogicalGraphNodeKind::render ||
           kind == LogicalGraphNodeKind::compute ||
           kind == LogicalGraphNodeKind::anchor ||
           kind == LogicalGraphNodeKind::snapshot_copy ||
           kind == LogicalGraphNodeKind::output_transform;
}

bool isMultiviewRasterNode(LogicalGraphNodeKind kind) {
    return kind == LogicalGraphNodeKind::render ||
           kind == LogicalGraphNodeKind::output_transform;
}

std::uint32_t viewMask(std::uint32_t view_count) {
    if (view_count >= 32) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return (std::uint32_t{1} << view_count) - 1u;
}

std::set<std::string, std::less<>> canonicalViewNodeSet(
    std::vector<std::string> nodes,
    const std::map<std::string, const LogicalGraphNode *,
                   std::less<>> &known_nodes,
    std::string_view subject,
    bool require_render_scope) {
    std::set<std::string, std::less<>> result;
    for (auto &name : nodes) {
        requireNonEmpty(name, subject);
        const auto known = known_nodes.find(name);
        if (known == known_nodes.end()) {
            throw std::runtime_error(
                std::string{subject} +
                " references unknown logical node: " + name);
        }
        if (require_render_scope &&
            !isMultiviewRasterNode(known->second->kind)) {
            throw std::runtime_error(
                std::string{subject} +
                " may contain only render or output nodes: " +
                name);
        }
        if (!result.insert(name).second) {
            throw std::runtime_error(
                std::string{subject} +
                " contains duplicate logical node: " + name);
        }
    }
    return result;
}

} // namespace

std::string_view vulkanScopeViewExecutionName(
    VulkanScopeViewExecution execution) {
    switch (execution) {
    case VulkanScopeViewExecution::single_view:
        return "single_view";
    case VulkanScopeViewExecution::sequential:
        return "sequential";
    case VulkanScopeViewExecution::multiview:
        return "multiview";
    }
    throw std::runtime_error(
        "unknown Vulkan scope view execution");
}

const VulkanNodeViewExecutionPlan &
ResolvedVulkanViewExecutionPlan::requireNode(
    std::string_view name) const {
    const auto found = std::lower_bound(
        nodes.begin(), nodes.end(), name,
        [](const VulkanNodeViewExecutionPlan &node,
           std::string_view key) {
            return node.node < key;
        });
    if (found == nodes.end() || found->node != name) {
        throw std::runtime_error(
            "Vulkan view execution plan has no logical node: " +
            std::string{name});
    }
    return *found;
}

ResolvedVulkanViewExecutionPlan resolveVulkanViewExecutionPlan(
    const CompiledLogicalRenderGraph &graph,
    const TargetEndpoint &endpoint,
    const std::optional<VulkanViewExecutionPlanRequest>
        &authored_request) {
    const auto request = authored_request.value_or(
        VulkanViewExecutionPlanRequest{});
    if (request.view_count == 0 ||
        request.view_count > 32) {
        throw std::runtime_error(
            "Vulkan view execution view_count must be in [1, 32]");
    }
    if (request.view_count == 1 &&
        request.preference ==
            XrViewExecutionPreference::require_multiview) {
        throw std::runtime_error(
            "pelican.plan.multiview_required_unavailable@1: "
            "multiview requires view_count greater than one");
    }

    std::map<std::string, const LogicalGraphNode *, std::less<>>
        known_nodes;
    for (const auto &node : graph.nodes) {
        known_nodes.emplace(node.name, &node);
    }
    const auto independent = canonicalViewNodeSet(
        request.view_independent_nodes, known_nodes,
        "view-independent node declaration", false);
    const auto capable = canonicalViewNodeSet(
        request.multiview_capable_nodes, known_nodes,
        "multiview-capable node declaration", true);
    for (const auto &name : independent) {
        if (capable.contains(name)) {
            throw std::runtime_error(
                "logical node cannot be both view-independent and "
                "multiview-capable: " +
                name);
        }
    }

    const auto endpoint_has_capability =
        hasCapability(endpoint, vulkanMultiviewCapability);
    const auto max_view_count =
        endpointUnsignedFact(
            endpoint, vulkanMaxMultiviewViewCountFact);
    if (endpoint_has_capability && !max_view_count) {
        throw std::runtime_error(
            "target endpoint advertises '" +
            std::string{vulkanMultiviewCapability} +
            "' without required fact '" +
            std::string{vulkanMaxMultiviewViewCountFact} + "'");
    }
    const auto endpoint_can_execute =
        endpoint_has_capability &&
        max_view_count.value_or(0) >= request.view_count;

    std::vector<std::string> incompatible_nodes;
    std::size_t view_dependent_count = 0;
    for (const auto &[name, node] : known_nodes) {
        if (!isViewDependentNode(node->kind) ||
            independent.contains(name)) {
            continue;
        }
        ++view_dependent_count;
        if (!capable.contains(name)) {
            incompatible_nodes.push_back(name);
        }
    }
    if (request.preference ==
            XrViewExecutionPreference::require_multiview) {
        if (view_dependent_count == 0) {
            throw std::runtime_error(
                "pelican.plan.multiview_required_unavailable@1: "
                "logical graph has no view-dependent render scope");
        }
        if (!endpoint_has_capability) {
            throw std::runtime_error(
                "pelican.plan.multiview_required_unavailable@1: "
                "endpoint '" +
                endpoint.id + "' lacks capability '" +
                std::string{vulkanMultiviewCapability} + "'");
        }
        if (!endpoint_can_execute) {
            throw std::runtime_error(
                "pelican.plan.multiview_required_unavailable@1: "
                "endpoint '" +
                endpoint.id + "' supports at most " +
                std::to_string(max_view_count.value_or(0)) +
                " views, but " +
                std::to_string(request.view_count) +
                " were requested");
        }
        if (!incompatible_nodes.empty()) {
            std::ostringstream detail;
            for (std::size_t index = 0;
                 index < incompatible_nodes.size(); ++index) {
                if (index != 0) detail << ", ";
                detail << incompatible_nodes[index];
            }
            throw std::runtime_error(
                "pelican.plan.multiview_required_unavailable@1: "
                "view-dependent node(s) lack a multiview-capable "
                "implementation: " +
                detail.str());
        }
    }

    ResolvedVulkanViewExecutionPlan result;
    result.summary.view_count = request.view_count;
    result.summary.requested = request.preference;
    result.summary.endpoint_supports_multiview =
        endpoint_has_capability;
    result.summary.max_multiview_view_count =
        max_view_count.value_or(0);
    result.nodes.reserve(known_nodes.size());

    bool has_sequential = false;
    for (const auto &[name, node] : known_nodes) {
        auto execution =
            VulkanScopeViewExecution::single_view;
        std::string reason =
            "node is view-independent or has no render-view "
            "execution";
        if (request.view_count > 1 &&
            isViewDependentNode(node->kind) &&
            !independent.contains(name)) {
            const auto allow_multiview =
                request.preference !=
                    XrViewExecutionPreference::sequential &&
                endpoint_can_execute &&
                capable.contains(name);
            if (allow_multiview) {
                execution =
                    VulkanScopeViewExecution::multiview;
                result.summary.uses_multiview = true;
                reason =
                    "implementation and endpoint support the requested "
                    "multiview cardinality";
            } else {
                execution =
                    VulkanScopeViewExecution::sequential;
                has_sequential = true;
                if (request.preference ==
                    XrViewExecutionPreference::sequential) {
                    reason =
                        "sequential execution was explicitly requested";
                } else if (!capable.contains(name)) {
                    reason =
                        "node has no multiview-capable implementation";
                } else if (!endpoint_has_capability) {
                    reason =
                        "endpoint does not advertise multiview";
                } else {
                    reason =
                        "requested view count exceeds the endpoint "
                        "multiview limit";
                }
            }
        } else if (independent.contains(name)) {
            reason =
                "node was explicitly declared view-independent";
        }
        result.nodes.push_back(VulkanNodeViewExecutionPlan{
            .node = name,
            .execution = execution,
            .view_count =
                execution ==
                        VulkanScopeViewExecution::single_view
                    ? 1u
                    : request.view_count,
            .execution_count =
                execution ==
                        VulkanScopeViewExecution::sequential
                    ? request.view_count
                    : 1u,
            .view_mask =
                execution ==
                        VulkanScopeViewExecution::multiview
                    ? viewMask(request.view_count)
                    : 0u,
            .reason = reason,
        });
        result.decisions.push_back(PlanningDecision{
            "pelican.plan.scope_view_execution@1",
            name,
            std::string{
                vulkanScopeViewExecutionName(execution)},
            std::move(reason),
        });
    }
    result.summary.mixed_execution =
        result.summary.uses_multiview && has_sequential;

    std::string selected;
    if (request.view_count == 1) {
        selected = "single_view";
        result.summary.reason =
            "one logical view requires one physical execution";
    } else if (result.summary.mixed_execution) {
        selected = "mixed";
        result.summary.reason =
            "eligible scopes use multiview and remaining "
            "view-dependent scopes use sequential execution";
    } else if (result.summary.uses_multiview) {
        selected = "multiview";
        result.summary.reason =
            "all view-dependent scopes have multiview-capable "
            "implementations";
    } else {
        selected = "sequential";
        if (request.preference ==
            XrViewExecutionPreference::sequential) {
            result.summary.reason =
                "sequential execution was explicitly requested";
        } else if (!endpoint_has_capability) {
            result.summary.reason =
                "endpoint does not advertise multiview";
        } else if (!endpoint_can_execute) {
            result.summary.reason =
                "requested view count exceeds the endpoint "
                "multiview limit";
        } else {
            result.summary.reason =
                "no view-dependent scope declared a "
                "multiview-capable implementation";
        }
    }
    result.decisions.push_back(PlanningDecision{
        "pelican.plan.view_execution_selected@1",
        graph.name, std::move(selected), result.summary.reason,
    });
    return result;
}

} // namespace Pelican
