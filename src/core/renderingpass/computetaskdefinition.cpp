#include "computetaskdefinition.hpp"

#include "../shader/raygrouplayout.hpp"
#include "../shader/raytracingrefusal.hpp"
#include "../../project/graphcomposition.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>
#include <vulkan/vulkan.hpp>

namespace Pelican {

namespace {

// RC-3c2a: project's (framegraphbufferdefinition.hpp), held to Vulkan here.
static_assert(sizeof(vk::DispatchIndirectCommand) == frameGraphDispatchCommandBytes);
static_assert(sizeof(vk::DispatchIndirectCommand) == sizeof(std::uint32_t) * 3);
static_assert(frameGraphIndexedDrawCommandBytes == sizeof(std::uint32_t) * 5);
static_assert(sizeof(vk::TraceRaysIndirectCommandKHR) == frameGraphDispatchCommandBytes);
static_assert(frameGraphDrawCountBytes == 4);
static_assert(frameGraphSceneDrawBoundsV1Bytes == 32);
static_assert(frameGraphSceneDrawSegmentV1Bytes == 32);

using Json = nlohmann::json;

[[noreturn]] void refuseRay(const std::string &code, Json fields, const std::string &message) {
    throw RayTracingRefusal(code, std::move(fields), code + ": " + message);
}

// RTL-1b: a ray type name the task names and the layout does not have:
// pelican.ray_group_name_unknown@1 {name, kind, task}.
std::uint32_t taskRayTypeIndex(const RayGroupLayout &layout, const std::string &ray_type,
                               const std::string &task) {
    try {
        return layout.rayTypeIndex(ray_type);
    } catch (const RayTracingRefusal &refusal) {
        auto fields = refusal.fields();
        fields["task"] = task;
        throw RayTracingRefusal(refusal.code(), std::move(fields), refusal.what());
    }
}

bool hasKey(const RayTracingTaskDeclaration &ray, std::string_view key) {
    return std::find(ray.keys.begin(), ray.keys.end(), key) != ray.keys.end();
}

// The keys a form takes; a stage no form takes is unsupported, any other
// key unknown, and (with a layout) a key of the form without one is the
// legacy syntax.
void requireRayTracingKeys(const RayTracingTaskDeclaration &ray, const std::string &task,
                           bool layout) {
    static constexpr std::array<std::string_view, 5> either{
        "raygen", "max_recursion", "callable", "callable_record", "callable_records"};
    for (const auto &key : ray.keys) {
        if (std::find(either.begin(), either.end(), key) != either.end() ||
            key == "acceleration_structure") {
            continue;
        }
        const bool layout_key = key == "traced_ray_types" || key == "misses" || key == "hit_shaders";
        const bool legacy_key = key == "miss" || key == "closesthit";
        if (layout ? layout_key : legacy_key) continue;
        if (layout && legacy_key) {
            auto keys = Json::array();
            for (const auto *legacy : {"closesthit", "miss"})
                if (hasKey(ray, legacy)) keys.push_back(legacy);
            refuseRay("pelican.ray_tracing.legacy_syntax_with_layout@1", {{"task", task}, {"keys", keys}},
                      "compute task '" + task + "' must use traced_ray_types/misses/hit_shaders");
        }
        if (key == "any_hit" || key == "anyhit" || key == "intersection") {
            refuseRay("pelican.ray_tracing.unsupported_shader_stage@1", {{"task", task}, {"stage", key}},
                      "compute task '" + task + "' declares unsupported ray tracing stage '" + key + "'");
        }
        refuseRay("pelican.ray_tracing.unknown_shader_stage@1", {{"task", task}, {"field", key}},
                  "compute task '" + task + "' declares unknown ray tracing field '" + key + "'");
    }
}

// A list of the form without a layout (miss / closesthit), as
// parseRayTracingShaderList read it.
std::vector<ShaderReference> legacyShaderList(const AuthoredShaderList &list, std::string_view field,
                                              ShaderStage stage, const std::string &task) {
    const std::string name{field};
    if (list.form == AuthoredShaderList::Form::absent) {
        refuseRay("pelican.ray_tracing.shader_stage_required@1", {{"task", task}, {"stage", name}},
                  "compute task '" + task + "' requires ray_tracing." + name);
    }
    if (list.form == AuthoredShaderList::Form::other) {
        refuseRay("pelican.ray_tracing.invalid_shader_reference@1", {{"task", task}, {"stage", name}},
                  "compute task '" + task + "' ray_tracing." + name + " must be a string or string array");
    }
    std::vector<ShaderReference> result;
    for (const auto &entry : list.entries) {
        if (entry.form != AuthoredShaderField::Form::string || entry.value.empty()) {
            refuseRay("pelican.ray_tracing.invalid_shader_reference@1", {{"task", task}, {"stage", name}},
                      "compute task '" + task + "' ray_tracing." + name + " must contain non-empty strings");
        }
        result.push_back(makeShaderReference(entry.value, stage));
    }
    if (result.empty()) {
        refuseRay("pelican.ray_tracing.shader_stage_required@1", {{"task", task}, {"stage", name}},
                  "compute task '" + task + "' requires at least one ray_tracing." + name + " shader");
    }
    return result;
}

// A layout task's shader: GLSL source, non-empty.
ShaderReference layoutShaderReference(const AuthoredShaderField &field, ShaderStage stage,
                                      const std::string &task) {
    if (field.form != AuthoredShaderField::Form::string || field.value.empty()) {
        refuseRay("pelican.ray_tracing.layout_shader_invalid@1", {{"task", task}, {"stage", shaderStageName(stage)}},
                  "compute task '" + task + "' requires non-empty shader references");
    }
    auto lower = field.value;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.ends_with(".spv")) {
        refuseRay("pelican.ray_tracing.layout_source_only@1",
                  {{"task", task}, {"stage", shaderStageName(stage)}, {"shader", field.value}},
                  "compute task '" + task + "' ray_group_layout shaders must be GLSL source, not SPIR-V: " +
                      field.value);
    }
    return makeShaderReference(field.value, stage);
}

std::string availableKeys(const std::vector<std::string> &keys) {
    std::string result = "[";
    for (const auto &key : keys) {
        if (result.size() > 1) result += ",";
        result += key;
    }
    return result + "]";
}

// The callables and their records (both forms) as the parse checked them.
void appendCallables(const RayTracingTaskDeclaration &ray, RayTracingTaskShaderDefinition &result, bool layout,
                     const std::string &task) {
    for (const auto &callable : ray.callables) {
        result.callables.push_back(
            layout ? layoutShaderReference({AuthoredShaderField::Form::string, callable}, ShaderStage::callable, task)
                   : makeShaderReference(callable, ShaderStage::callable));
    }
    if (ray.callable_record) {
        result.callable_record_layout = std::make_shared<const RayCallableRecordLayout>(*ray.callable_record);
        result.callable_records = ray.callable_records;
    }
    result.max_recursion = ray.max_recursion;
}

RayTracingTaskShaderDefinition legacyRayTracingShaders(const RayTracingTaskDeclaration &ray,
                                                       const std::string &task) {
    requireRayTracingKeys(ray, task, false);
    if (ray.raygen.form != AuthoredShaderField::Form::string) {
        throw GraphRegistryError("render_graph.compute_task_invalid@1",
                                 {{"node", task}, {"field", "ray_tracing.raygen"}, {"reason", "requires_string"}});
    }
    RayTracingTaskShaderDefinition result{
        .raygen = makeShaderReference(ray.raygen.value, ShaderStage::raygen),
        .misses = legacyShaderList(ray.miss, "miss", ShaderStage::miss, task),
        .closest_hits = legacyShaderList(ray.closesthit, "closesthit", ShaderStage::closesthit, task),
    };
    appendCallables(ray, result, false, task);
    return result;
}

// parseLayoutRayTracingShaders' rules, over the declaration as written.
RayTracingTaskShaderDefinition layoutRayTracingShaders(const RayTracingTaskDeclaration &ray,
                                                       const std::string &name,
                                                       const RayTracingProjectConfig &project) {
    requireRayTracingKeys(ray, name, true);
    const auto &layout = project.layout;
    RayTracingTaskShaderDefinition result{
        .raygen = layoutShaderReference(ray.raygen, ShaderStage::raygen, name),
        .uses_group_layout = true,
        .group_names = layout.groups,
        .ray_type_names = layout.ray_types,
    };
    if (ray.traced_ray_types.form != AuthoredShaderList::Form::array || ray.traced_ray_types.entries.empty()) {
        refuseRay("pelican.ray_tracing.traced_ray_types_invalid@1", {{"task", name}},
                  "compute task '" + name + "' requires a non-empty traced_ray_types array");
    }
    std::set<std::uint32_t> traced;
    for (const auto &encoded : ray.traced_ray_types.entries) {
        if (encoded.form != AuthoredShaderField::Form::string) {
            refuseRay("pelican.ray_tracing.traced_ray_types_invalid@1", {{"task", name}},
                      "compute task '" + name + "' ray type names must be strings");
        }
        const auto index = taskRayTypeIndex(layout, encoded.value, name);
        if (!traced.emplace(index).second) {
            refuseRay("pelican.ray_tracing.traced_ray_types_invalid@1", {{"task", name}, {"ray_type", encoded.value}},
                      "compute task '" + name + "' duplicates ray type '" + encoded.value + "'");
        }
        result.traced_ray_type_indices.push_back(index);
    }

    if (ray.misses_form != RayTracingTaskDeclaration::Form::object) {
        refuseRay("pelican.ray_tracing.miss_shader_missing@1", {{"task", name}},
                  "compute task '" + name + "' requires misses object");
    }
    std::vector<std::string> miss_keys;
    for (const auto &[miss, shader] : ray.misses) {
        (void)shader;
        miss_keys.push_back(miss);
        if (std::find(layout.misses.begin(), layout.misses.end(), miss) == layout.misses.end()) {
            refuseRay("pelican.ray_tracing.miss_shader_unknown@1", {{"task", name}, {"miss", miss}},
                      "compute task '" + name + "' miss='" + miss + "'");
        }
    }
    for (const auto &miss : layout.misses) {
        const auto found = std::find_if(ray.misses.begin(), ray.misses.end(),
                                        [&](const auto &entry) { return entry.first == miss; });
        if (found == ray.misses.end() || found->second.form != AuthoredShaderField::Form::string ||
            found->second.value.empty()) {
            refuseRay("pelican.ray_tracing.miss_shader_missing@1", {{"task", name}, {"miss", miss}},
                      "compute task '" + name + "' miss='" + miss + "' available=" + availableKeys(miss_keys));
        }
        result.misses.push_back(layoutShaderReference(found->second, ShaderStage::miss, name));
    }

    if (ray.hit_shaders_form != RayTracingTaskDeclaration::Form::object) {
        refuseRay("pelican.ray_tracing.hit_shader_missing@1", {{"task", name}},
                  "compute task '" + name + "' requires hit_shaders object");
    }
    std::vector<std::string> ray_keys;
    for (const auto &shaders : ray.hit_shaders) {
        ray_keys.push_back(shaders.ray_type);
        const auto ray_index = taskRayTypeIndex(layout, shaders.ray_type, name);
        if (!traced.contains(ray_index)) {
            refuseRay("pelican.ray_tracing.hit_shader_untraced@1", {{"task", name}, {"ray_type", shaders.ray_type}},
                      "compute task '" + name + "' supplies untraced ray type '" + shaders.ray_type + "'");
        }
    }
    const auto ray_count = layout.ray_types.size();
    result.hit_shader_slots.resize(layout.groups.size() * ray_count);
    result.any_hit_shader_slots.resize(layout.groups.size() * ray_count);
    result.intersection_shader_slots.resize(layout.groups.size() * ray_count);
    for (const auto ray_index : result.traced_ray_type_indices) {
        const auto &ray_name = layout.ray_types.at(ray_index);
        const auto shaders = std::find_if(ray.hit_shaders.begin(), ray.hit_shaders.end(),
                                          [&](const auto &entry) { return entry.ray_type == ray_name; });
        if (shaders == ray.hit_shaders.end() || !shaders->is_object) {
            refuseRay("pelican.ray_tracing.hit_shader_missing@1", {{"task", name}, {"ray_type", ray_name}},
                      "compute task '" + name + "' group='<all>' ray_type='" + ray_name +
                          "' available=" + availableKeys(ray_keys));
        }
        std::vector<std::string> group_keys;
        for (const auto &group : shaders->groups) {
            group_keys.push_back(group.group);
            if (std::find(layout.groups.begin(), layout.groups.end(), group.group) == layout.groups.end()) {
                refuseRay("pelican.ray_tracing.hit_shader_unknown_group@1",
                          {{"task", name}, {"ray_type", ray_name}, {"group", group.group}},
                          "compute task '" + name + "' group='" + group.group + "' ray_type='" + ray_name + "'");
            }
        }
        for (std::size_t group_index = 0; group_index < layout.groups.size(); ++group_index) {
            const auto &group_name = layout.groups[group_index];
            const auto group = std::find_if(shaders->groups.begin(), shaders->groups.end(),
                                            [&](const auto &entry) { return entry.group == group_name; });
            if (group == shaders->groups.end() || !group->is_object) {
                refuseRay("pelican.ray_tracing.hit_shader_missing@1",
                          {{"task", name}, {"ray_type", ray_name}, {"group", group_name}},
                          "compute task '" + name + "' group='" + group_name + "' ray_type='" + ray_name +
                              "' available=" + availableKeys(group_keys));
            }
            const auto group_type = layout.group_types.at(group_index);
            const auto invalid_group = [&] {
                refuseRay("pelican.ray_tracing.ray_hit_group_invalid@1",
                          {{"task", name}, {"ray_type", ray_name}, {"group", group_name}},
                          "compute task '" + name + "' group='" + group_name + "' ray_type='" + ray_name +
                              "' has an invalid stage set for its layout group type");
            };
            if (group->closesthit.form != AuthoredShaderField::Form::string || group->closesthit.value.empty()) {
                invalid_group();
            }
            if (group->has_unknown_field) invalid_group();
            if (group->declares_type) {
                throw RayTracingRefusal(
                    "pelican.ray_group_type_declared_in_task@1",
                    Json{{"task", name}, {"ray_type", ray_name}, {"group", group_name}},
                    "pelican.ray_group_type_declared_in_task@1: layout v3 group type is declared only in "
                    "ray_group_layout");
            }
            if (group->anyhit.form != AuthoredShaderField::Form::absent &&
                (group->anyhit.form != AuthoredShaderField::Form::string || group->anyhit.value.empty())) {
                invalid_group();
            }
            const bool has_intersection = group->intersection.form != AuthoredShaderField::Form::absent;
            if (group_type == RayGroupType::procedural) {
                if (group->intersection.form != AuthoredShaderField::Form::string ||
                    group->intersection.value.empty()) {
                    throw RayTracingRefusal(
                        "pelican.procedural_group_requires_intersection@1",
                        Json{{"task", name}, {"ray_type", ray_name}, {"group", group_name}},
                        "pelican.procedural_group_requires_intersection@1: procedural hit group requires "
                        "intersection and closesthit stages");
                }
            } else if (has_intersection) {
                throw RayTracingRefusal(
                    "pelican.ray_tracing.unsupported_shader_stage@1",
                    Json{{"task", name}, {"ray_type", ray_name}, {"group", group_name}, {"stage", "intersection"}},
                    "pelican.ray_tracing.unsupported_shader_stage@1: triangles hit group cannot declare "
                    "intersection");
            }
            const auto slot_index = group_index * ray_count + ray_index;
            result.hit_shader_slots[slot_index] =
                layoutShaderReference(group->closesthit, ShaderStage::closesthit, name);
            if (group->anyhit.form != AuthoredShaderField::Form::absent) {
                if (layout.usesMaterialAlphaProfile() && group_type == RayGroupType::triangles) {
                    refuseRay("pelican.ray_profile_authored_anyhit@1",
                              {{"task", name}, {"ray_type", ray_name}, {"group", group_name}},
                              "compute task '" + name + "' group='" + group_name + "' ray_type='" + ray_name +
                                  "' cannot combine portable_profile 'material_alpha_v1' with authored anyhit '" +
                                  group->anyhit.value + "'");
                }
                result.any_hit_shader_slots[slot_index] =
                    layoutShaderReference(group->anyhit, ShaderStage::anyhit, name);
            }
            if (has_intersection) {
                result.intersection_shader_slots[slot_index] =
                    layoutShaderReference(group->intersection, ShaderStage::intersection, name);
            }
        }
    }
    if (layout.usesMaterialAlphaProfile()) {
        for (const auto ray_index : result.traced_ray_type_indices) {
            for (std::size_t group_index = 0; group_index < layout.groups.size(); ++group_index) {
                // Procedural rows have no material. The generated material
                // alpha shader must never dereference their sentinel index.
                if (layout.group_types[group_index] == RayGroupType::procedural) continue;
                result.any_hit_shader_slots[group_index * ray_count + ray_index] = ShaderReference{
                    "generated://material_alpha_v1",
                    ShaderStage::anyhit,
                    ShaderReferenceKind::engine_generated,
                    false,
                };
            }
        }
    }
    appendCallables(ray, result, true, name);
    return result;
}

// The shader references as the task declared them (shaderresolution reads
// them): a layout task's after the layout resolved them (a generated anyhit
// is not declared), the form without one's as written.
std::vector<DeclaredShaderReference> declaredShaderRefsOf(const ComputeTaskDefinition &definition,
                                                          const ComputeNodeInfo &info) {
    std::vector<DeclaredShaderReference> result;
    if (!definition.ray_tracing) {
        result.push_back({.stage = DeclaredShaderStage::compute, .index = std::nullopt, .ref = *info.shader});
        return result;
    }
    const auto &ray = *definition.ray_tracing;
    result.push_back({.stage = DeclaredShaderStage::raygen, .index = std::nullopt,
                      .ref = info.ray_tracing->raygen.value});
    for (std::size_t index = 0; index < ray.callables.size(); ++index) {
        result.push_back({.stage = DeclaredShaderStage::callable, .index = index, .ref = ray.callables[index].ref});
    }
    if (ray.uses_group_layout) {
        for (std::size_t index = 0; index < ray.misses.size(); ++index) {
            result.push_back({.stage = DeclaredShaderStage::miss, .index = index, .ref = ray.misses[index].ref});
        }
        const auto slots = [&](const std::vector<std::optional<ShaderReference>> &values, DeclaredShaderStage stage) {
            for (std::size_t index = 0; index < values.size(); ++index) {
                const auto &slot = values[index];
                if (!slot || slot->kind == ShaderReferenceKind::engine_generated) continue;
                result.push_back({.stage = stage, .index = index, .ref = slot->ref});
            }
        };
        slots(ray.hit_shader_slots, DeclaredShaderStage::closesthit);
        slots(ray.any_hit_shader_slots, DeclaredShaderStage::anyhit);
        slots(ray.intersection_shader_slots, DeclaredShaderStage::intersection);
        return result;
    }
    const auto as_written = [&](const AuthoredShaderList &list, DeclaredShaderStage stage) {
        for (std::size_t index = 0; index < list.entries.size(); ++index) {
            result.push_back({.stage = stage, .index = index, .ref = list.entries[index].value});
        }
    };
    as_written(info.ray_tracing->miss, DeclaredShaderStage::miss);
    as_written(info.ray_tracing->closesthit, DeclaredShaderStage::closesthit);
    return result;
}

ComputeTaskDefinition computeTaskDefinitionOf(const SharedComputeTask &task,
                                              const RayTracingProjectConfig *project) {
    const auto &node = *task.node;
    const auto &info = std::get<ComputeNodeInfo>(node.info);
    const std::string authored{task.authored_name};
    ComputeTaskDefinition definition;
    definition.name = node.name;
    if (info.ray_tracing) {
        definition.ray_tracing = project != nullptr ? layoutRayTracingShaders(*info.ray_tracing, authored, *project)
                                                    : legacyRayTracingShaders(*info.ray_tracing, authored);
        definition.ray_tracing->acceleration_structure = info.ray_tracing->acceleration_structure;
        // What a trace launches (the parse read only its form).
        if (!info.dispatch.rays && !info.dispatch.rays_from && !info.dispatch.indirect) {
            refuseRay("pelican.ray_tracing.trace_extent_required@1", {{"task", authored}},
                      "compute task '" + authored +
                          "' requires dispatch.rays_from, dispatch.rays, or dispatch.indirect");
        }
    } else {
        if (!info.shader) {
            throw GraphRegistryError("render_graph.compute_task_invalid@1",
                                     {{"node", authored}, {"field", "shader"}, {"reason", "requires_string"}});
        }
        definition.shader = makeShaderReference(*info.shader, ShaderStage::compute);
    }
    definition.ray_query_acceleration_structure = info.ray_query_acceleration_structure;
    definition.shader_declaration_parsed = true;
    definition.declared_shader_refs = declaredShaderRefsOf(definition, info);
    for (const auto &use : node.uses) {
        const auto written = use.resource + (use.history ? "@history" : "");
        switch (use.role) {
        case ResourceUseRole::input:
            definition.reads.push_back(written);
            break;
        case ResourceUseRole::output:
            definition.writes.push_back(written);
            break;
        case ResourceUseRole::resource_port:
            definition.resource_ports.push_back(ShaderResourcePortDefinition{
                .name = use.binding.port,
                .resource = written,
                .kind = use.binding.kind,
                .buffer_element = use.binding.element,
                .access = use.binding.access,
                .view = use.binding.view,
                .sampling = use.binding.sampling,
                .subresource = use.subresource,
                .domain = use.binding.domain,
            });
            break;
        default:
            break;
        }
    }
    definition.dispatch = info.dispatch;
    definition.schedule = node.schedule;
    definition.view_family = node.view_family;
    definition.view_select = node.view_select;
    return definition;
}

} // namespace

std::vector<ComputeTaskDefinition> computeTaskDefinitionsOf(
    const RenderGraphIR &ir, const RayTracingProjectConfig *ray_tracing_project) {
    std::vector<ComputeTaskDefinition> result;
    for (const auto &task : sharedComputeTasksOf(ir)) {
        result.push_back(computeTaskDefinitionOf(task, ray_tracing_project));
    }
    return result;
}

} // namespace Pelican
