#pragma once

#include "framegraphbufferdefinition.hpp"
#include "rayrecordabi.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Pelican {

// RC-3c2b1: a compute task's execution values as the render graph IR holds
// them (ComputeNodeInfo), in project vocabulary. Core makes its
// ComputeTaskDefinition from these (computeTaskDefinitionsOf); the shader
// references are resolved there, against the project's ray group layout.

// The defaults of a compute task's fields, once: the IR's parse, the
// execution's types and the graph parameter schema (Studio) read these.
inline constexpr std::uint32_t defaultComputeDispatchGroupCount = 1;
inline constexpr std::uint32_t defaultRayTracingMaxRecursion = 1;
inline constexpr std::string_view defaultRayTracingAccelerationStructure = "scene";

struct ComputeImageExtentDispatchDefinition {
    // Names a typed image entry in resource_ports. The selected port's
    // subresource mip and the reflected shader workgroup size determine the
    // direct dispatch group count.
    std::string port;

    bool operator==(const ComputeImageExtentDispatchDefinition &) const = default;
};

// `dispatch`: at most one of groups / groups_from (a compute task), rays /
// rays_from (a ray tracing task) or indirect. Absent: one group.
struct ComputeDispatchDefinition {
    std::uint32_t groups_x = defaultComputeDispatchGroupCount;
    std::uint32_t groups_y = defaultComputeDispatchGroupCount;
    std::uint32_t groups_z = defaultComputeDispatchGroupCount;
    std::optional<ComputeImageExtentDispatchDefinition> groups_from;
    // Ray-tracing tasks launch one ray per selected image pixel. This uses
    // the same typed image-port resolution as groups_from, without a compute
    // local-size conversion.
    std::optional<ComputeImageExtentDispatchDefinition> rays_from;
    // Explicit ray launch dimensions, independent of any image resource.
    std::optional<std::array<std::uint32_t, 3>> rays;
    std::optional<ComputeIndirectDispatchDefinition> indirect;

    bool operator==(const ComputeDispatchDefinition &) const = default;
};

// A shader reference as the task wrote it: absent, a string (possibly
// empty), or another value.
struct AuthoredShaderField {
    enum class Form : std::uint8_t {
        absent,
        string,
        other,
    };
    Form form = Form::absent;
    std::string value;

    bool operator==(const AuthoredShaderField &) const = default;
};

// A list of names or shader references as written: absent, one string, an
// array (each entry as written), or another value.
struct AuthoredShaderList {
    enum class Form : std::uint8_t {
        absent,
        string,
        array,
        other,
    };
    Form form = Form::absent;
    std::vector<AuthoredShaderField> entries;

    bool operator==(const AuthoredShaderList &) const = default;
};

// hit_shaders.<ray type>.<group> as written.
struct AuthoredHitGroup {
    std::string group;
    bool is_object = true;
    AuthoredShaderField closesthit;
    AuthoredShaderField anyhit;
    AuthoredShaderField intersection;
    // `type` (refused: the layout owns the group type), and a field outside
    // closesthit / anyhit / intersection / type.
    bool declares_type = false;
    bool has_unknown_field = false;

    bool operator==(const AuthoredHitGroup &) const = default;
};

// hit_shaders.<ray type> as written.
struct AuthoredHitShaders {
    std::string ray_type;
    bool is_object = true;
    std::vector<AuthoredHitGroup> groups;

    bool operator==(const AuthoredHitShaders &) const = default;
};

// A compute task's `ray_tracing`. Which shader form is valid depends on the
// project's ray group layout, which parseRenderGraph does not know: without
// one, raygen / miss / closesthit; with one, raygen / traced_ray_types /
// misses / hit_shaders. So the keys and both forms' shader fields are kept as
// written and core checks them against the project (computeTaskDefinitionsOf,
// the refusals it always gave). What does not depend on the layout is
// checked by the parse: callable, the callable record (its schema parsed,
// its inline records packed), max_recursion, acceleration_structure.
struct RayTracingTaskDeclaration {
    // The object's keys, in the written (sorted) order.
    std::vector<std::string> keys;
    AuthoredShaderField raygen;
    // Without a ray group layout.
    AuthoredShaderList miss;
    AuthoredShaderList closesthit;
    // With a ray group layout.
    AuthoredShaderList traced_ray_types;
    enum class Form : std::uint8_t {
        absent,
        object,
        other,
    };
    Form misses_form = Form::absent;
    std::vector<std::pair<std::string, AuthoredShaderField>> misses;
    Form hit_shaders_form = Form::absent;
    std::vector<AuthoredHitShaders> hit_shaders;
    // Either form.
    std::vector<std::string> callables;
    std::optional<RayCallableRecordLayout> callable_record;
    // One packed record per callable: the inline callable_records, or the
    // config's ray_task_callable_records for this task once
    // resolveRenderGraphReferences applied the table.
    std::vector<std::vector<std::byte>> callable_records;
    std::uint32_t max_recursion = defaultRayTracingMaxRecursion;
    // `acceleration_structure`, or the config's
    // ray_task_acceleration_structures once applied.
    std::string acceleration_structure{defaultRayTracingAccelerationStructure};

    bool operator==(const RayTracingTaskDeclaration &) const = default;
};

} // namespace Pelican
