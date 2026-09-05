#pragma once

#include "vulkanrendercompilerpackage.hpp"

namespace Pelican::detail {

// Inserts shared scene construction and one operation for each named TLAS.
void insertSceneAccelerationStructureNodes(
    std::vector<FrameGraphDefinition> &graphs,
    const TargetPlanningPolicy &target_planning,
    const std::vector<ComputeTaskDefinition> &tasks,
    bool has_ray_project,
    std::span<const SceneAccelerationStructureDefinition> named_definitions = {});

// Internal CPU test seam for XR compute namespacing. Keeping this out of the
// package API lets the non-GPU provenance regression exercise the production
// transform without requiring a Vulkan physical device.
void namespaceComputeTasks(
    std::vector<ComputeTaskDefinition> &tasks,
    std::vector<FrameGraphDefinition> &graphs,
    const nlohmann::json &composed_config,
    CompiledRenderPipeline &compiled_pipeline);

} // namespace Pelican::detail
