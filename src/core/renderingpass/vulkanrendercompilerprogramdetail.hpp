#pragma once

#include "vulkanrendercompilerpackage.hpp"

#include <vector>

namespace Pelican::detail {

// What stays in core of a variant's compute-task namespacing (the IR's is
// project's namespaceSharedComputeTasks, in finalizeRenderGraph): the
// execution declarations of the config's compute tasks and the pipeline's
// pass provenance take the variant's suffix, and the provenance is
// synchronized with the composed config before and after. A CPU test seam:
// the provenance regression runs it without a Vulkan physical device.
void namespaceComputeTaskDefinitions(
    std::vector<ComputeTaskDefinition> &tasks,
    const nlohmann::json &composed_config,
    CompiledRenderPipeline &compiled_pipeline);

} // namespace Pelican::detail
