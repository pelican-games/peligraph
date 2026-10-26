#pragma once

#include "../../project/computetaskdefinition.hpp"
#include "../../project/rendergraphir.hpp"

#include <vector>

namespace Pelican {

struct RayTracingProjectConfig;

// RC-3c2b1: the execution's compute tasks, made from a final render graph IR
// (the only read of a compute task is parseRenderGraph's): one definition per
// config compute task, in declaration order, named as the IR names it (the
// variant's suffix included). Reads, writes and resource ports are the
// node's uses; ordering remains in the IR / FramePlan. The dispatch,
// schedule and view are the node's. The ray tracing shaders are resolved
// here, against the project's ray group layout
// when it has one (`ray_tracing_project`), and refused as the task was
// authored: the ray tracing refusals (RayTracingRefusal, the codes and
// sentences they always had) name the task by its authored name.
std::vector<ComputeTaskDefinition> computeTaskDefinitionsOf(
    const RenderGraphIR &ir, const RayTracingProjectConfig *ray_tracing_project);

} // namespace Pelican
