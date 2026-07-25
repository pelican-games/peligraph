#pragma once

#include "logicalrendergraph.hpp"
#include "targetplanning.hpp"
#include "xrtargetpolicy.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

inline constexpr std::string_view vulkanMultiviewCapability =
    "pelican.vulkan.multiview@1";
inline constexpr std::string_view vulkanMaxMultiviewViewCountFact =
    "pelican.vulkan.max_multiview_view_count@1";

enum class VulkanScopeViewExecution : std::uint8_t {
    single_view,
    sequential,
    multiview,
};

std::string_view vulkanScopeViewExecutionName(
    VulkanScopeViewExecution execution);

struct VulkanViewExecutionPlanRequest {
    std::uint32_t view_count = 1;
    XrViewExecutionPreference preference =
        XrViewExecutionPreference::automatic;
    // View-independent nodes execute once and may feed every view. This is
    // intended for work such as a shared shadow pass.
    std::vector<std::string> view_independent_nodes;
    // Eligibility is explicit because both the implementation and its shader
    // contract must support multiview.
    std::vector<std::string> multiview_capable_nodes;
};

struct VulkanViewExecutionPlan {
    std::uint32_t view_count = 1;
    XrViewExecutionPreference requested =
        XrViewExecutionPreference::automatic;
    bool endpoint_supports_multiview = false;
    std::uint32_t max_multiview_view_count = 0;
    bool uses_multiview = false;
    bool mixed_execution = false;
    std::string reason;
};

struct VulkanNodeViewExecutionPlan {
    std::string node;
    VulkanScopeViewExecution execution =
        VulkanScopeViewExecution::single_view;
    std::uint32_t view_count = 1;
    std::uint32_t execution_count = 1;
    std::uint32_t view_mask = 0;
    std::string reason;
};

struct ResolvedVulkanViewExecutionPlan {
    VulkanViewExecutionPlan summary;
    // Canonical node-name order.
    std::vector<VulkanNodeViewExecutionPlan> nodes;
    std::vector<PlanningDecision> decisions;

    const VulkanNodeViewExecutionPlan &requireNode(
        std::string_view name) const;
};

// Pure target-policy resolution. It does not mutate the logical graph or
// create Vulkan resources.
ResolvedVulkanViewExecutionPlan resolveVulkanViewExecutionPlan(
    const CompiledLogicalRenderGraph &graph,
    const TargetEndpoint &endpoint,
    const std::optional<VulkanViewExecutionPlanRequest> &request =
        std::nullopt);

} // namespace Pelican
