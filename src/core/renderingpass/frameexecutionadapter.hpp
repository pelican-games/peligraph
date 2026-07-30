#pragma once

#include "frameplanner.hpp"
#include "../../project/executionplan.hpp"

#include <string>

namespace Pelican {

struct VulkanTargetPlan;

// Extracts the selected Vulkan endpoint as data-only execution evidence.
// Backend handles and queue-family objects remain in the runtime/backend.
ExecutionEndpoint selectedVulkanExecutionEndpoint(
    const VulkanTargetPlan &target_plan);

FrameExecutionPlan compileFrameExecutionPlan(
    const FrameGraphDefinition &definition,
    const FramePlan &frame_plan,
    ExecutionEndpoint endpoint);

// Isolated compatibility callers may only have the legacy FramePlan. This
// preserves publication invariants while production compiler programs emit
// the richer definition-derived plan above.
FrameExecutionPlan makeCompatibilityFrameExecutionPlan(
    const FramePlan &frame_plan,
    ExecutionEndpoint endpoint = ExecutionEndpoint{
        .id = "device:0",
        .endpoint_class =
            ExecutionEndpointClass::device,
        .backend = "vulkan",
    });

void validateFrameExecutionPlanCompatibility(
    const FrameExecutionPlan &execution_plan,
    const FramePlan &frame_plan);

} // namespace Pelican
