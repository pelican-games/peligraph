#include "../src/project/computetaskdefinition.hpp"

bool projectComputeTaskDefaults() {
    using namespace Pelican;
    const ComputeTaskDefinition task;
    const RayTracingTaskShaderDefinition ray;
    return task.shader.stage == ShaderStage::compute && !task.ray_tracing &&
           task.loop_index == PassLoopIndex{0, 1} &&
           computeTaskScheduleName(task.schedule) == "per_frame" &&
           ray.raygen.stage == ShaderStage::raygen &&
           ray.max_recursion == defaultRayTracingMaxRecursion;
}
