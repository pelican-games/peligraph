#pragma once

#include "vulkanrendercompilerpackage.hpp"

#include <set>
#include <string>

namespace Pelican::detail {

// Before the builds: a raster ray query, a compute task's ray_query or a
// named scene acceleration structure makes every graph's target planning
// require pelican.vulkan.ray_query@1 (RC-3c1a: the test seam for the bridge
// against requireRenderGraphRayQueryCapability).
void requireRayQueryPlanningCapability(
    const std::vector<FrameGraphDefinition> &graphs,
    const std::vector<ComputeTaskDefinition> &tasks,
    CompiledRenderPipeline &pipeline);

// Inserts shared scene construction and one operation for each named TLAS.
void insertSceneAccelerationStructureNodes(
    std::vector<FrameGraphDefinition> &graphs,
    const TargetPlanningPolicy &target_planning,
    const std::vector<ComputeTaskDefinition> &tasks,
    bool has_ray_project,
    std::span<const SceneAccelerationStructureDefinition> named_definitions = {});

// Applies runtime-name ordering as additional dependencies without changing dataflow inference.
void applyFrameNodeOrder(
    std::vector<FrameGraphDefinition> &graphs,
    const nlohmann::json &order,
    bool require_all_graphs = true);

// Internal CPU test seam for XR compute namespacing. Keeping this out of the
// package API lets the non-GPU provenance regression exercise the production
// transform without requiring a Vulkan physical device.
void namespaceComputeTasks(
    std::vector<ComputeTaskDefinition> &tasks,
    std::vector<FrameGraphDefinition> &graphs,
    const nlohmann::json &composed_config,
    CompiledRenderPipeline &compiled_pipeline);

// The fullscreen passes of the composed config an XR variant may run once
// for all views, as targetViewExecutionRequest reads them from the JSON:
// type "fullscreen", no `implementation`, engine://fullscreen with a builtin
// fragment. RC-3c1a: the test seam for the bridge against
// renderGraphMultiviewCapableNodes (RC-3c1b reads the IR instead).
std::set<std::string, std::less<>> multiviewCapableFullscreenPasses(
    const nlohmann::json &config, bool enable_multiview_runtime);

} // namespace Pelican::detail
