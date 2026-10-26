#pragma once

#include "computetaskdeclaration.hpp"
#include "nodeschedule.hpp"
#include "passloopindex.hpp"
#include "shaderreference.hpp"
#include "shaderresourceport.hpp"
#include "viewfamilyrelation.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// RC-3c2b1: ComputeDispatchDefinition and ComputeImageExtentDispatchDefinition
// are the project's (computetaskdeclaration.hpp): the IR holds the dispatch
// and core takes it as it is.

struct RayTracingTaskShaderDefinition {
    std::string acceleration_structure{defaultRayTracingAccelerationStructure};
    // The project's default (computetaskdeclaration.hpp).
    std::uint32_t max_recursion = defaultRayTracingMaxRecursion;
    ShaderReference raygen{
        "", ShaderStage::raygen,
        ShaderReferenceKind::explicit_file, false};
    std::vector<ShaderReference> misses;
    std::vector<ShaderReference> closest_hits;
    std::vector<ShaderReference> callables;
    std::shared_ptr<const RayCallableRecordLayout> callable_record_layout;
    std::vector<std::vector<std::byte>> callable_records;
    bool uses_group_layout = false;
    std::vector<std::string> group_names;
    std::vector<std::string> ray_type_names;
    std::vector<std::uint32_t> traced_ray_type_indices;
    // H[g,r], group-major. Null slots are materialized as valid generated
    // no-op closest-hit groups for ray types this task never traces.
    std::vector<std::optional<ShaderReference>> hit_shader_slots;
    // Optional any-hit stage paired with the same physical H[g,r]
    // occurrence. The vector is empty for legacy RT1a definitions.
    std::vector<std::optional<ShaderReference>> any_hit_shader_slots;
    std::vector<std::optional<ShaderReference>> intersection_shader_slots;
};

// per_frame: executes once for the logical frame and may feed every view.
// per_view: executes once for each logical view; physical lowering may keep
// these invocations sequential while other scopes use Vulkan multiview.
// once (PS-1): dispatched as per_frame, but only on a frame where its inputs
// or outputs changed (a new runtime generation, a recreated persistent
// output, different host bytes read, an upstream once node that ran,
// rerun_once); the frame plan node carries FrameGraphNodeDefinition::once.
// RC-3c2b1: the IR's NodeSchedule (one set, one default).
using ComputeTaskSchedule = NodeSchedule;

std::string_view computeTaskScheduleName(
    ComputeTaskSchedule schedule);

struct ComputeTaskDefinition {
    std::string name;
    ShaderReference shader = ShaderReference{"", ShaderStage::compute, ShaderReferenceKind::explicit_file, false};
    // Present for a trace-rays task. It remains a top-level compute node in
    // the frame plan because both execution kinds must run outside dynamic
    // rendering.
    std::optional<RayTracingTaskShaderDefinition> ray_tracing;
    std::optional<std::string> ray_query_acceleration_structure;
    bool shader_declaration_parsed = false;
    std::vector<DeclaredShaderReference> declared_shader_refs;
    std::vector<std::string> reads;
    std::vector<std::string> writes;
    // Optional typed annotations over reads/writes. The dependency lists
    // remain authoritative and are not synthesized from these declarations.
    std::vector<ShaderResourcePortDefinition> resource_ports;
    ComputeDispatchDefinition dispatch;
    ComputeTaskSchedule schedule = defaultNodeSchedule;
    std::string view_family{
        mainRenderViewFamilyId};
    RenderViewSelect view_select;
    // LC-1: as PassDefinition::loop_index.
    PassLoopIndex loop_index;
};

} // namespace Pelican
