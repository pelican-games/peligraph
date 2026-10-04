#include "computetaskdefinitionjson.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "rendertargetcontainer.hpp"
#include "../loader/pathresolver.hpp"
#include "../log.hpp"
#include "../shader/pelican_sets.hpp"
#include "../shader/raygrouplayout.hpp"
#include "../shader/raytracingrefusal.hpp"
#include "../shader/raytracingvalidation.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../renderer/frameresources.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"
#include "../vkcore/render_target_layout_tracker.hpp"
#include "../vkcore/util.hpp"
#include "../../project/renderresourcename.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace Pelican {

namespace {

constexpr vk::DeviceSize compute_dispatch_command_size =
    sizeof(vk::DispatchIndirectCommand);
static_assert(
    sizeof(vk::DispatchIndirectCommand) ==
    sizeof(std::uint32_t) * 3);
static_assert(
    frameGraphIndexedDrawCommandBytes ==
    sizeof(std::uint32_t) * 5);
static_assert(sizeof(vk::TraceRaysIndirectCommandKHR) == compute_dispatch_command_size);
static_assert(frameGraphDrawCountBytes == 4);
static_assert(frameGraphSceneDrawBoundsV1Bytes == 32);
static_assert(frameGraphSceneDrawSegmentV1Bytes == 32);

uint32_t requireUint32(const nlohmann::json &json, std::string_view field, std::string_view context) {
    if (!json.contains(field) || !json.at(field).is_number_integer()) {
        throw std::runtime_error(std::string{context} + " requires non-negative integer field: " +
                                 std::string{field});
    }
    const auto value = json.at(field).get<uint64_t>();
    if (value > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(std::string{context} + " field is too large: " + std::string{field});
    }
    return static_cast<uint32_t>(value);
}

vk::DeviceSize requireDeviceSize(const nlohmann::json &json, std::string_view field, std::string_view context) {
    if (!json.contains(field) || !json.at(field).is_number_integer()) {
        throw std::runtime_error(std::string{context} + " requires non-negative integer field: " +
                                 std::string{field});
    }
    return static_cast<vk::DeviceSize>(json.at(field).get<uint64_t>());
}

FrameGraphHostBufferSource parseHostBufferSource(
    const nlohmann::json &entry,
    std::string_view context) {
    const auto value =
        parseStringField(entry, "host_source", context);
    if (value == "runtime_f32") return FrameGraphHostBufferSource::runtime_f32;
    if (value == "scene_lights_v2") {
        return FrameGraphHostBufferSource::scene_lights_v2;
    }
    if (value == "view_family_matrices_v1") {
        return FrameGraphHostBufferSource::view_family_matrices_v1;
    }
    if (value == "scene_draw_commands_v1") {
        return FrameGraphHostBufferSource::
            scene_draw_commands_v1;
    }
    if (value == "scene_draw_bounds_v1") {
        return FrameGraphHostBufferSource::
            scene_draw_bounds_v1;
    }
    if (value == "scene_draw_segments_v1") {
        return FrameGraphHostBufferSource::
            scene_draw_segments_v1;
    }
    throw std::runtime_error(
        std::string{context} +
        " has unknown host_source '" + value + "'");
}

FrameGraphBufferCommandLayout
parseBufferCommandLayout(
    const nlohmann::json &entry,
    std::string_view context) {
    const auto value =
        parseStringField(entry, "command_layout", context);
    if (value == "compute_dispatch") {
        return FrameGraphBufferCommandLayout::
            compute_dispatch;
    }
    if (value == "ray_dispatch") return FrameGraphBufferCommandLayout::ray_dispatch;
    if (value == "indexed_draw") {
        return FrameGraphBufferCommandLayout::
            indexed_draw;
    }
    if (value == "draw_count") {
        return FrameGraphBufferCommandLayout::
            draw_count;
    }
    throw std::runtime_error(
        std::string{context} +
        " has unknown command_layout '" + value + "'");
}

std::pair<std::uint32_t, std::uint32_t>
renderTargetExtent(
    const nlohmann::json &config,
    std::string_view resource,
    std::string_view context) {
    if (!config.contains("render_targets") ||
        !config.at("render_targets").is_array()) {
        throw std::runtime_error(
            std::string{context} +
            " requires render_targets to resolve resource '" +
            std::string{resource} + "'");
    }
    const auto found = std::find_if(
        config.at("render_targets").begin(),
        config.at("render_targets").end(),
        [resource](const nlohmann::json &target) {
            return target.is_object() &&
                   target.value("name", std::string{}) ==
                       resource;
        });
    if (found == config.at("render_targets").end()) {
        throw std::runtime_error(
            std::string{context} +
            " references unknown render target '" +
            std::string{resource} + "'");
    }
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    if (found->contains("width") &&
        found->contains("height")) {
        width = requireUint32(*found, "width", context);
        height = requireUint32(*found, "height", context);
    } else {
        const auto base = std::find_if(
            config.at("render_targets").begin(),
            config.at("render_targets").end(),
            [](const nlohmann::json &target) {
                return target.is_object() &&
                       target.contains("width") &&
                       target.contains("height");
            });
        if (base ==
            config.at("render_targets").end()) {
            throw std::runtime_error(
                std::string{context} +
                " cannot derive a base render-target extent");
        }
        const auto base_width =
            requireUint32(*base, "width", context);
        const auto base_height =
            requireUint32(*base, "height", context);
        const auto scale =
            found->value("extent_scale", 1.0);
        if (!std::isfinite(scale) || scale <= 0.0) {
            throw std::runtime_error(
                std::string{context} +
                " requires a positive finite extent_scale");
        }
        width = static_cast<std::uint64_t>(
            static_cast<double>(base_width) * scale);
        height = static_cast<std::uint64_t>(
            static_cast<double>(base_height) * scale);
    }
    if (width == 0 || height == 0) {
        throw std::runtime_error(
            std::string{context} +
            " requires a non-zero render-target extent");
    }
    if (width >
            std::numeric_limits<std::uint32_t>::max() ||
        height >
            std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error(
            std::string{context} +
            " render-target extent exceeds uint32");
    }
    return {
        static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height)};
}

vk::DeviceSize checkedMultiply(
    vk::DeviceSize lhs, vk::DeviceSize rhs,
    std::string_view context) {
    if (rhs != 0 &&
        lhs > std::numeric_limits<vk::DeviceSize>::max() /
                  rhs) {
        throw std::overflow_error(
            std::string{context} + " byte size overflow");
    }
    return lhs * rhs;
}

vk::DeviceSize checkedAdd(
    vk::DeviceSize lhs, vk::DeviceSize rhs,
    std::string_view context) {
    if (lhs >
        std::numeric_limits<vk::DeviceSize>::max() -
            rhs) {
        throw std::overflow_error(
            std::string{context} + " byte size overflow");
    }
    return lhs + rhs;
}

std::pair<FrameGraphBufferExtentSizeDefinition,
          vk::DeviceSize>
parseExtentDerivedBufferSize(
    const nlohmann::json &entry,
    const nlohmann::json &config,
    std::string_view context) {
    const auto &encoded =
        entry.at("size_from_extent");
    if (!encoded.is_object()) {
        throw std::runtime_error(
            std::string{context} +
            ".size_from_extent must be an object");
    }
    for (auto field = encoded.begin();
         field != encoded.end(); ++field) {
        if (field.key() != "resource" &&
            field.key() != "tile_width" &&
            field.key() != "tile_height" &&
            field.key() != "header_bytes" &&
            field.key() != "bytes_per_tile" &&
            field.key() != "copies") {
            throw std::runtime_error(
                std::string{context} +
                ".size_from_extent has unknown field '" +
                field.key() + "'");
        }
    }
    FrameGraphBufferExtentSizeDefinition definition{
        .resource = parseStringField(
            encoded, "resource",
            std::string{context} +
                ".size_from_extent"),
        .tile_width = requireUint32(
            encoded, "tile_width",
            std::string{context} +
                ".size_from_extent"),
        .tile_height = requireUint32(
            encoded, "tile_height",
            std::string{context} +
                ".size_from_extent"),
        .header_bytes = requireDeviceSize(
            encoded, "header_bytes",
            std::string{context} +
                ".size_from_extent"),
        .bytes_per_tile = requireDeviceSize(
            encoded, "bytes_per_tile",
            std::string{context} +
                ".size_from_extent"),
        .copies =
            encoded.contains("copies")
                ? requireUint32(
                      encoded, "copies",
                      std::string{context} +
                          ".size_from_extent")
                : 1u,
    };
    if (definition.tile_width == 0 ||
        definition.tile_height == 0 ||
        definition.bytes_per_tile == 0 ||
        definition.copies == 0) {
        throw std::runtime_error(
            std::string{context} +
            ".size_from_extent requires positive tile dimensions "
            "bytes_per_tile, and copies");
    }
    const auto [width, height] =
        renderTargetExtent(
            config, definition.resource, context);
    const auto tiles_x =
        (static_cast<std::uint64_t>(width) +
         definition.tile_width - 1) /
        definition.tile_width;
    const auto tiles_y =
        (static_cast<std::uint64_t>(height) +
         definition.tile_height - 1) /
        definition.tile_height;
    const auto tile_count =
        checkedMultiply(
            tiles_x, tiles_y, context);
    const auto payload =
        checkedMultiply(
            tile_count,
            definition.bytes_per_tile,
            context);
    const auto region_byte_size =
        checkedAdd(
            definition.header_bytes,
            payload, context);
    const auto byte_size =
        checkedMultiply(
            region_byte_size,
            definition.copies,
            context);
    return {std::move(definition), byte_size};
}

// RTL-1b: a ray type name the task names and the layout does not have:
// pelican.ray_group_name_unknown@1 {name, kind, task}.
std::uint32_t taskRayTypeIndex(const RayGroupLayout &layout,
                               const std::string &ray_type,
                               const std::string &task) {
    try {
        return layout.rayTypeIndex(ray_type);
    } catch (const RayTracingRefusal &refusal) {
        auto fields = refusal.fields();
        fields["task"] = task;
        throw RayTracingRefusal(refusal.code(), std::move(fields), refusal.what());
    }
}

// RTL-1b: the keys of the form without a layout that a task in a layout
// project still has (legacy_syntax_with_layout's `keys`), in name order.
nlohmann::json legacyLayoutKeys(const nlohmann::json &pipeline_json) {
    auto keys = nlohmann::json::array();
    for (const auto *key : {"closesthit", "miss"})
        if (pipeline_json.contains(key)) keys.push_back(key);
    return keys;
}

std::vector<ShaderReference> parseRayTracingShaderList(
    const nlohmann::json &pipeline_json,
    std::string_view field, ShaderStage stage,
    const std::string &name) {
    const auto found = pipeline_json.find(field);
    if (found == pipeline_json.end()) {
        throw RayTracingRefusal(
            "pelican.ray_tracing.shader_stage_required@1", nlohmann::json{{"task", name}, {"stage", std::string{field}}},
            "pelican.ray_tracing.shader_stage_required@1: compute task '" +
            name + "' requires ray_tracing." +
            std::string{field});
    }
    std::vector<ShaderReference> result;
    const auto append = [&](const nlohmann::json &value) {
        if (!value.is_string() ||
            value.get_ref<const std::string &>().empty()) {
            throw RayTracingRefusal(
                "pelican.ray_tracing.invalid_shader_reference@1", nlohmann::json{{"task", name}, {"stage", std::string{field}}},
                "pelican.ray_tracing.invalid_shader_reference@1: compute "
                "task '" +
                name + "' ray_tracing." + std::string{field} +
                " must contain non-empty strings");
        }
        result.push_back(makeShaderReference(
            value.get<std::string>(), stage));
    };
    if (found->is_string()) {
        append(*found);
    } else if (found->is_array()) {
        for (const auto &value : *found) append(value);
    } else {
        throw RayTracingRefusal(
            "pelican.ray_tracing.invalid_shader_reference@1", nlohmann::json{{"task", name}, {"stage", std::string{field}}},
            "pelican.ray_tracing.invalid_shader_reference@1: compute "
            "task '" +
            name + "' ray_tracing." + std::string{field} +
            " must be a string or string array");
    }
    if (result.empty()) {
        throw RayTracingRefusal(
            "pelican.ray_tracing.shader_stage_required@1", nlohmann::json{{"task", name}, {"stage", std::string{field}}},
            "pelican.ray_tracing.shader_stage_required@1: compute task '" +
            name + "' requires at least one ray_tracing." +
            std::string{field} + " shader");
    }
    return result;
}

std::string availableObjectKeys(const nlohmann::json &object) {
    std::string result = "[";
    if (object.is_object()) {
        for (auto field = object.begin(); field != object.end(); ++field) {
            if (result.size() > 1) result += ",";
            result += field.key();
        }
    }
    return result + "]";
}

ShaderReference makeLayoutShaderReference(
    const nlohmann::json &value, ShaderStage stage,
    const std::string &task_name) {
    if (!value.is_string() || value.get_ref<const std::string &>().empty()) {
        throw RayTracingRefusal(
            "pelican.ray_tracing.layout_shader_invalid@1", nlohmann::json{{"task", task_name}, {"stage", shaderStageName(stage)}},
            "pelican.ray_tracing.layout_shader_invalid@1: compute task '" +
            task_name + "' requires non-empty shader references");
    }
    auto reference = value.get<std::string>();
    auto lower = reference;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.ends_with(".spv")) {
        throw RayTracingRefusal(
            "pelican.ray_tracing.layout_source_only@1", nlohmann::json{{"task", task_name}, {"stage", shaderStageName(stage)}, {"shader", reference}},
            "pelican.ray_tracing.layout_source_only@1: compute task '" +
            task_name + "' ray_group_layout shaders must be GLSL source, not SPIR-V: " +
            reference);
    }
    return makeShaderReference(std::move(reference), stage);
}

void parseRayTracingRecursion(
    const nlohmann::json &pipeline_json, const std::string &name,
    RayTracingTaskShaderDefinition &result) {
    const auto recursion = pipeline_json.find("max_recursion");
    if (recursion == pipeline_json.end()) return;
    const auto positive_signed =
        recursion->is_number_integer() && !recursion->is_number_unsigned() &&
        recursion->get<std::int64_t>() > 0;
    const auto unsigned_integer = recursion->is_number_unsigned();
    const auto encoded = positive_signed
                             ? static_cast<std::uint64_t>(recursion->get<std::int64_t>())
                             : unsigned_integer ? recursion->get<std::uint64_t>() : 0;
    if ((!positive_signed && !unsigned_integer) || encoded == 0 ||
        encoded > std::numeric_limits<std::uint32_t>::max()) {
        throw RayTracingRefusal(
            "pelican.ray_tracing.invalid_recursion@1", nlohmann::json{{"task", name}},
            "pelican.ray_tracing.invalid_recursion@1: compute task '" + name +
            "' max_recursion must be a positive uint32 integer");
    }
    result.max_recursion = static_cast<std::uint32_t>(encoded);
}

void parseCallableRecords(const nlohmann::json &pipeline, const std::string &name,
                          RayTracingTaskShaderDefinition &result) {
    if (!pipeline.contains("callable_record") && !pipeline.contains("callable_records")) return;
    if (result.callables.empty() || !pipeline.contains("callable_record") ||
        !pipeline.contains("callable_records")) {
        throw RayTracingRefusal(
                "pelican.ray_tracing.callable_records_invalid@1", nlohmann::json{{"task", name}},
                "pelican.ray_tracing.callable_records_invalid@1: task '" + name +
            "' requires callable, callable_record and callable_records together");
    }
    auto layout = std::make_shared<RayCallableRecordLayout>(
        parseRayCallableRecordLayout(pipeline.at("callable_record"), name));
    result.callable_records = encodeRayCallableRecords(
        pipeline.at("callable_records"), *layout, result.callables.size(), name);
    result.callable_record_layout = std::move(layout);
}

RayTracingTaskShaderDefinition parseLayoutRayTracingShaders(
    const nlohmann::json &pipeline_json, const std::string &name,
    const RayTracingProjectConfig &project) {
    for (auto field = pipeline_json.begin(); field != pipeline_json.end(); ++field) {
        if (field.key() == "raygen" || field.key() == "traced_ray_types" ||
            field.key() == "misses" || field.key() == "hit_shaders" ||
            field.key() == "max_recursion" || field.key() == "callable" ||
            field.key() == "callable_record" || field.key() == "callable_records" ||
            field.key() == "acceleration_structure") {
            continue;
        }
        if (field.key() == "miss" || field.key() == "closesthit") {
            throw RayTracingRefusal(
                "pelican.ray_tracing.legacy_syntax_with_layout@1", nlohmann::json{{"task", name}, {"keys", legacyLayoutKeys(pipeline_json)}},
                "pelican.ray_tracing.legacy_syntax_with_layout@1: compute task '" +
                name + "' must use traced_ray_types/misses/hit_shaders");
        }
        if (field.key() == "any_hit" || field.key() == "anyhit" ||
            field.key() == "intersection") {
            throw RayTracingRefusal(
                "pelican.ray_tracing.unsupported_shader_stage@1", nlohmann::json{{"task", name}, {"stage", field.key()}},
                "pelican.ray_tracing.unsupported_shader_stage@1: compute task '" +
                name + "' declares unsupported ray tracing stage '" +
                field.key() + "'");
        }
        throw RayTracingRefusal(
            "pelican.ray_tracing.unknown_shader_stage@1", nlohmann::json{{"task", name}, {"field", field.key()}},
            "pelican.ray_tracing.unknown_shader_stage@1: compute task '" +
            name + "' declares unknown ray tracing field '" + field.key() + "'");
    }

    const auto &layout = project.layout;
    RayTracingTaskShaderDefinition result{
        .raygen = makeLayoutShaderReference(
            pipeline_json.contains("raygen") ? pipeline_json.at("raygen")
                                               : nlohmann::json{},
            ShaderStage::raygen, name),
        .uses_group_layout = true,
        .group_names = layout.groups,
        .ray_type_names = layout.ray_types,
    };
    if (!pipeline_json.contains("traced_ray_types") ||
        !pipeline_json.at("traced_ray_types").is_array() ||
        pipeline_json.at("traced_ray_types").empty()) {
        throw RayTracingRefusal(
            "pelican.ray_tracing.traced_ray_types_invalid@1", nlohmann::json{{"task", name}},
            "pelican.ray_tracing.traced_ray_types_invalid@1: compute task '" +
            name + "' requires a non-empty traced_ray_types array");
    }
    std::set<std::uint32_t> traced;
    for (const auto &encoded : pipeline_json.at("traced_ray_types")) {
        if (!encoded.is_string()) {
            throw RayTracingRefusal(
                "pelican.ray_tracing.traced_ray_types_invalid@1", nlohmann::json{{"task", name}},
                "pelican.ray_tracing.traced_ray_types_invalid@1: compute task '" +
                name + "' ray type names must be strings");
        }
        const auto index = taskRayTypeIndex(
            layout, encoded.get_ref<const std::string &>(), name);
        if (!traced.emplace(index).second) {
            throw RayTracingRefusal(
                "pelican.ray_tracing.traced_ray_types_invalid@1", nlohmann::json{{"task", name}, {"ray_type", encoded.get<std::string>()}},
                "pelican.ray_tracing.traced_ray_types_invalid@1: compute task '" +
                name + "' duplicates ray type '" + encoded.get<std::string>() + "'");
        }
        result.traced_ray_type_indices.push_back(index);
    }

    if (!pipeline_json.contains("misses") ||
        !pipeline_json.at("misses").is_object()) {
        throw RayTracingRefusal(
            "pelican.ray_tracing.miss_shader_missing@1", nlohmann::json{{"task", name}},
            "pelican.ray_tracing.miss_shader_missing@1: compute task '" +
            name + "' requires misses object");
    }
    const auto &misses = pipeline_json.at("misses");
    for (auto field = misses.begin(); field != misses.end(); ++field) {
        if (std::find(layout.misses.begin(), layout.misses.end(), field.key()) ==
            layout.misses.end()) {
            throw RayTracingRefusal(
                "pelican.ray_tracing.miss_shader_unknown@1", nlohmann::json{{"task", name}, {"miss", field.key()}},
                "pelican.ray_tracing.miss_shader_unknown@1: compute task '" +
                name + "' miss='" + field.key() + "'");
        }
    }
    for (const auto &miss : layout.misses) {
        if (!misses.contains(miss) || !misses.at(miss).is_string() ||
            misses.at(miss).get_ref<const std::string &>().empty()) {
            throw RayTracingRefusal(
                "pelican.ray_tracing.miss_shader_missing@1", nlohmann::json{{"task", name}, {"miss", miss}},
                "pelican.ray_tracing.miss_shader_missing@1: compute task '" +
                name + "' miss='" + miss + "' available=" +
                availableObjectKeys(misses));
        }
        result.misses.push_back(makeLayoutShaderReference(
            misses.at(miss), ShaderStage::miss, name));
    }

    if (!pipeline_json.contains("hit_shaders") ||
        !pipeline_json.at("hit_shaders").is_object()) {
        throw RayTracingRefusal(
            "pelican.ray_tracing.hit_shader_missing@1", nlohmann::json{{"task", name}},
            "pelican.ray_tracing.hit_shader_missing@1: compute task '" +
            name + "' requires hit_shaders object");
    }
    const auto &hit_shaders = pipeline_json.at("hit_shaders");
    for (auto ray = hit_shaders.begin(); ray != hit_shaders.end(); ++ray) {
        const auto ray_index = taskRayTypeIndex(layout, ray.key(), name);
        if (!traced.contains(ray_index)) {
            throw RayTracingRefusal(
                "pelican.ray_tracing.hit_shader_untraced@1", nlohmann::json{{"task", name}, {"ray_type", ray.key()}},
                "pelican.ray_tracing.hit_shader_untraced@1: compute task '" +
                name + "' supplies untraced ray type '" + ray.key() + "'");
        }
    }
    const auto ray_count = layout.ray_types.size();
    result.hit_shader_slots.resize(layout.groups.size() * ray_count);
    result.any_hit_shader_slots.resize(layout.groups.size() * ray_count);
    result.intersection_shader_slots.resize(layout.groups.size() * ray_count);
    for (const auto ray_index : result.traced_ray_type_indices) {
        const auto &ray_name = layout.ray_types.at(ray_index);
        if (!hit_shaders.contains(ray_name) ||
            !hit_shaders.at(ray_name).is_object()) {
            throw RayTracingRefusal(
                "pelican.ray_tracing.hit_shader_missing@1", nlohmann::json{{"task", name}, {"ray_type", ray_name}},
                "pelican.ray_tracing.hit_shader_missing@1: compute task '" +
                name + "' group='<all>' ray_type='" + ray_name +
                "' available=" + availableObjectKeys(hit_shaders));
        }
        const auto &groups = hit_shaders.at(ray_name);
        for (auto group = groups.begin(); group != groups.end(); ++group) {
            if (std::find(layout.groups.begin(), layout.groups.end(), group.key()) ==
                layout.groups.end()) {
                throw RayTracingRefusal(
                    "pelican.ray_tracing.hit_shader_unknown_group@1", nlohmann::json{{"task", name}, {"ray_type", ray_name}, {"group", group.key()}},
                    "pelican.ray_tracing.hit_shader_unknown_group@1: compute task '" +
                    name + "' group='" + group.key() + "' ray_type='" +
                    ray_name + "'");
            }
        }
        for (std::size_t group_index = 0;
             group_index < layout.groups.size(); ++group_index) {
            const auto &group_name = layout.groups[group_index];
            if (!groups.contains(group_name) ||
                !groups.at(group_name).is_object()) {
                throw RayTracingRefusal(
                    "pelican.ray_tracing.hit_shader_missing@1", nlohmann::json{{"task", name}, {"ray_type", ray_name}, {"group", group_name}},
                    "pelican.ray_tracing.hit_shader_missing@1: compute task '" +
                    name + "' group='" + group_name + "' ray_type='" + ray_name +
                    "' available=" + availableObjectKeys(groups));
            }
            const auto &group = groups.at(group_name);
            const auto group_type = layout.group_types.at(group_index);
            const auto invalid_group = [&] {
                throw RayTracingRefusal(
                    "pelican.ray_tracing.ray_hit_group_invalid@1", nlohmann::json{{"task", name}, {"ray_type", ray_name}, {"group", group_name}},
                    "pelican.ray_tracing.ray_hit_group_invalid@1: compute task '" +
                    name + "' group='" + group_name + "' ray_type='" + ray_name +
                    "' has an invalid stage set for its layout group type");
            };
            if (!group.contains("closesthit") ||
                !group.at("closesthit").is_string() ||
                group.at("closesthit").get_ref<const std::string &>().empty()) {
                invalid_group();
            }
            for (auto field = group.begin(); field != group.end(); ++field) {
                if (field.key() != "closesthit" && field.key() != "anyhit" &&
                    field.key() != "intersection" && field.key() != "type") {
                    invalid_group();
                }
            }
            if (group.contains("type")) {
                throw RayTracingRefusal(
                    "pelican.ray_group_type_declared_in_task@1", nlohmann::json{{"task", name}, {"ray_type", ray_name}, {"group", group_name}},
                    "pelican.ray_group_type_declared_in_task@1: layout v3 group type is declared only in ray_group_layout");
            }
            if (const auto anyhit = group.find("anyhit"); anyhit != group.end() &&
                (!anyhit->is_string() ||
                 anyhit->get_ref<const std::string &>().empty())) {
                invalid_group();
            }
            const auto intersection = group.find("intersection");
            if (group_type == RayGroupType::procedural) {
                if (intersection == group.end() || !intersection->is_string() ||
                    intersection->get_ref<const std::string &>().empty()) {
                    throw RayTracingRefusal(
                        "pelican.procedural_group_requires_intersection@1", nlohmann::json{{"task", name}, {"ray_type", ray_name}, {"group", group_name}},
                        "pelican.procedural_group_requires_intersection@1: procedural hit group requires intersection and closesthit stages");
                }
            } else if (intersection != group.end()) {
                throw RayTracingRefusal(
                    "pelican.ray_tracing.unsupported_shader_stage@1", nlohmann::json{{"task", name}, {"ray_type", ray_name}, {"group", group_name}, {"stage", "intersection"}},
                    "pelican.ray_tracing.unsupported_shader_stage@1: triangles hit group cannot declare intersection");
            }
            const auto slot_index = group_index * ray_count + ray_index;
            result.hit_shader_slots[slot_index] =
                makeLayoutShaderReference(group.at("closesthit"),
                                          ShaderStage::closesthit, name);
            if (const auto anyhit = group.find("anyhit"); anyhit != group.end()) {
                if (layout.usesMaterialAlphaProfile() &&
                    group_type == RayGroupType::triangles) {
                    throw RayTracingRefusal(
                        "pelican.ray_profile_authored_anyhit@1", nlohmann::json{{"task", name}, {"ray_type", ray_name}, {"group", group_name}},
                        "pelican.ray_profile_authored_anyhit@1: compute task '" +
                        name + "' group='" + group_name +
                        "' ray_type='" + ray_name +
                        "' cannot combine portable_profile 'material_alpha_v1' with authored anyhit '" +
                        anyhit->get<std::string>() + "'");
                }
                result.any_hit_shader_slots[slot_index] =
                    makeLayoutShaderReference(*anyhit, ShaderStage::anyhit, name);
            }
            if (intersection != group.end()) {
                result.intersection_shader_slots[slot_index] =
                    makeLayoutShaderReference(
                        *intersection, ShaderStage::intersection, name);
            }
        }
    }
    if (layout.usesMaterialAlphaProfile()) {
        for (const auto ray_index : result.traced_ray_type_indices) {
            for (std::size_t group_index = 0;
                 group_index < layout.groups.size(); ++group_index) {
                // Procedural rows have no material. The generated material
                // alpha shader must never dereference their sentinel index.
                if (layout.group_types[group_index] == RayGroupType::procedural)
                    continue;
                const auto slot_index =
                    group_index * ray_count + ray_index;
                result.any_hit_shader_slots[slot_index] =
                    ShaderReference{
                        "generated://material_alpha_v1",
                        ShaderStage::anyhit,
                        ShaderReferenceKind::engine_generated,
                        false,
                    };
            }
        }
    }
    if (pipeline_json.contains("callable")) {
        result.callables = parseRayTracingShaderList(
            pipeline_json, "callable", ShaderStage::callable, name);
        for (auto &reference : result.callables) {
            reference = makeLayoutShaderReference(reference.ref, ShaderStage::callable, name);
        }
    }
    parseCallableRecords(pipeline_json, name, result);
    parseRayTracingRecursion(pipeline_json, name, result);
    return result;
}

RayTracingTaskShaderDefinition parseRayTracingShaders(
    const nlohmann::json &task_json,
    const std::string &name,
    const RayTracingProjectConfig *ray_tracing_project) {
    const auto &pipeline_json = task_json.at("ray_tracing");
    if (!pipeline_json.is_object()) {
        throw RayTracingRefusal(
            "pelican.ray_tracing.invalid_pipeline_declaration@1", nlohmann::json{{"task", name}},
            "pelican.ray_tracing.invalid_pipeline_declaration@1: compute "
            "task '" +
            name + "' ray_tracing must be an object");
    }
    if (ray_tracing_project != nullptr) {
        return parseLayoutRayTracingShaders(
            pipeline_json, name, *ray_tracing_project);
    }
    for (auto field = pipeline_json.begin();
         field != pipeline_json.end(); ++field) {
        if (field.key() == "raygen" || field.key() == "miss" ||
            field.key() == "closesthit" ||
            field.key() == "max_recursion" || field.key() == "callable" ||
            field.key() == "callable_record" || field.key() == "callable_records" ||
            field.key() == "acceleration_structure") {
            continue;
        }
        if (field.key() == "any_hit" || field.key() == "anyhit" ||
            field.key() == "intersection") {
            throw RayTracingRefusal(
                "pelican.ray_tracing.unsupported_shader_stage@1", nlohmann::json{{"task", name}, {"stage", field.key()}},
                "pelican.ray_tracing.unsupported_shader_stage@1: compute "
                "task '" +
                name + "' declares unsupported ray tracing stage '" +
                field.key() + "'");
        }
        throw RayTracingRefusal(
            "pelican.ray_tracing.unknown_shader_stage@1", nlohmann::json{{"task", name}, {"field", field.key()}},
            "pelican.ray_tracing.unknown_shader_stage@1: compute task '" +
            name + "' declares unknown ray tracing field '" +
            field.key() + "'");
    }
    RayTracingTaskShaderDefinition result{
        .raygen = makeShaderReference(
            parseStringField(
                pipeline_json, "raygen",
                "compute task ray_tracing: " + name),
            ShaderStage::raygen),
        .misses = parseRayTracingShaderList(
            pipeline_json, "miss", ShaderStage::miss, name),
        .closest_hits = parseRayTracingShaderList(
            pipeline_json, "closesthit",
            ShaderStage::closesthit, name),
    };
    if (pipeline_json.contains("callable")) {
        result.callables = parseRayTracingShaderList(
            pipeline_json, "callable", ShaderStage::callable, name);
    }
    parseCallableRecords(pipeline_json, name, result);
    if (const auto recursion = pipeline_json.find("max_recursion");
        recursion != pipeline_json.end()) {
        const auto positive_signed =
            recursion->is_number_integer() &&
            !recursion->is_number_unsigned() &&
            recursion->get<std::int64_t>() > 0;
        const auto unsigned_integer =
            recursion->is_number_unsigned();
        const auto encoded = positive_signed
                                 ? static_cast<std::uint64_t>(
                                       recursion->get<std::int64_t>())
                                 : unsigned_integer
                                       ? recursion->get<std::uint64_t>()
                                       : 0;
        if ((!positive_signed && !unsigned_integer) || encoded == 0 ||
            encoded > std::numeric_limits<std::uint32_t>::max()) {
            throw RayTracingRefusal(
                "pelican.ray_tracing.invalid_recursion@1", nlohmann::json{{"task", name}},
                "pelican.ray_tracing.invalid_recursion@1: compute task '" +
                name + "' max_recursion must be a positive uint32 integer");
        }
        result.max_recursion = static_cast<std::uint32_t>(encoded);
    }
    return result;
}

void captureDeclaredComputeShaderReferences(
    ComputeTaskDefinition &definition,
    const nlohmann::json &task_json) {
    const auto append = [&](DeclaredShaderStage stage,
                            const nlohmann::json &value,
                            std::optional<std::size_t> index =
                                std::nullopt) {
        definition.declared_shader_refs.push_back(
            DeclaredShaderReference{
                .stage = stage,
                .index = index,
                .ref = value.get<std::string>(),
            });
    };
    if (!definition.ray_tracing) {
        append(DeclaredShaderStage::compute,
               task_json.at("shader"));
        return;
    }

    const auto &pipeline = task_json.at("ray_tracing");
    append(DeclaredShaderStage::raygen,
           pipeline.at("raygen"));
    for (std::size_t index = 0; index < definition.ray_tracing->callables.size(); ++index) {
        definition.declared_shader_refs.push_back(
            {.stage = DeclaredShaderStage::callable, .index = index,
             .ref = definition.ray_tracing->callables[index].ref});
    }
    if (definition.ray_tracing->uses_group_layout) {
        for (std::size_t index = 0;
             index < definition.ray_tracing->misses.size(); ++index) {
            definition.declared_shader_refs.push_back(
                {.stage = DeclaredShaderStage::miss,
                 .index = index,
                 .ref = definition.ray_tracing->misses[index].ref});
        }
        for (std::size_t index = 0;
             index < definition.ray_tracing->hit_shader_slots.size(); ++index) {
            const auto &slot =
                definition.ray_tracing->hit_shader_slots[index];
            if (!slot) continue;
            definition.declared_shader_refs.push_back(
                {.stage = DeclaredShaderStage::closesthit,
                 .index = index,
                 .ref = slot->ref});
        }
        for (std::size_t index = 0;
             index < definition.ray_tracing->any_hit_shader_slots.size(); ++index) {
            const auto &slot =
                definition.ray_tracing->any_hit_shader_slots[index];
            if (!slot ||
                slot->kind ==
                    ShaderReferenceKind::engine_generated) {
                continue;
            }
            definition.declared_shader_refs.push_back(
                {.stage = DeclaredShaderStage::anyhit,
                 .index = index,
                 .ref = slot->ref});
        }
        for (std::size_t index = 0;
             index < definition.ray_tracing->intersection_shader_slots.size();
             ++index) {
            const auto &slot =
                definition.ray_tracing->intersection_shader_slots[index];
            if (!slot) continue;
            definition.declared_shader_refs.push_back(
                {.stage = DeclaredShaderStage::intersection,
                 .index = index,
                 .ref = slot->ref});
        }
        return;
    }
    const auto append_list = [&](std::string_view field,
                                 DeclaredShaderStage stage) {
        const auto &encoded = pipeline.at(field);
        if (encoded.is_string()) {
            append(stage, encoded, 0);
            return;
        }
        for (std::size_t index = 0; index < encoded.size(); ++index) {
            append(stage, encoded.at(index), index);
        }
    };
    append_list("miss", DeclaredShaderStage::miss);
    append_list("closesthit", DeclaredShaderStage::closesthit);
}

ComputeDispatchDefinition parseDispatch(
    const nlohmann::json &task_json, const std::string &name,
    bool ray_tracing) {
    ComputeDispatchDefinition dispatch;
    if (!task_json.contains("dispatch")) {
        if (ray_tracing) {
            throw RayTracingRefusal(
                "pelican.ray_tracing.trace_extent_required@1", nlohmann::json{{"task", name}},
                "pelican.ray_tracing.trace_extent_required@1: compute "
                "task '" +
                name + "' requires dispatch.rays_from, dispatch.rays, or dispatch.indirect");
        }
        return dispatch;
    }

    const auto &json = task_json.at("dispatch");
    if (!json.is_object()) {
        throw std::runtime_error("compute task dispatch must be an object: " + name);
    }

    if (json.contains("rays")) {
        if (!ray_tracing || json.size() != 1) {
            throw RayTracingRefusal(
                "pelican.ray_tracing.invalid_trace_dispatch@1", nlohmann::json{{"task", name}},
                "pelican.ray_tracing.invalid_trace_dispatch@1: dispatch.rays requires a ray_tracing task and cannot be combined with other dispatch fields: " + name);
        }
        const auto &rays = json.at("rays");
        if (!rays.is_array() || rays.size() != 3) {
            throw RayTracingRefusal(
                "pelican.ray_tracing.invalid_trace_dispatch@1", nlohmann::json{{"task", name}},
                "pelican.ray_tracing.invalid_trace_dispatch@1: dispatch.rays must contain three positive uint32 dimensions: " + name);
        }
        std::array<std::uint32_t, 3> extent;
        for (std::size_t i = 0; i < extent.size(); ++i) {
            const auto &value = rays.at(i);
            if (!value.is_number_integer() || value <= 0 ||
                value > std::numeric_limits<std::uint32_t>::max()) {
                throw RayTracingRefusal(
                    "pelican.ray_tracing.invalid_trace_dispatch@1", nlohmann::json{{"task", name}},
                    "pelican.ray_tracing.invalid_trace_dispatch@1: dispatch.rays must contain three positive uint32 dimensions: " + name);
            }
            extent[i] = value.get<std::uint32_t>();
        }
        dispatch.rays = extent;
        return dispatch;
    }
    if (json.contains("rays_from")) {
        if (!ray_tracing) {
            throw RayTracingRefusal(
                "pelican.ray_tracing.rays_from_requires_pipeline@1", nlohmann::json{{"task", name}},
                "pelican.ray_tracing.rays_from_requires_pipeline@1: "
                "dispatch.rays_from requires a ray_tracing task: " +
                name);
        }
        if (json.size() != 1) {
            throw RayTracingRefusal(
                "pelican.ray_tracing.invalid_trace_dispatch@1", nlohmann::json{{"task", name}},
                "pelican.ray_tracing.invalid_trace_dispatch@1: "
                "dispatch.rays_from cannot be combined with compute "
                "dispatch fields: " +
                name);
        }
        const auto &rays_from = json.at("rays_from");
        if (!rays_from.is_object() || rays_from.size() != 1 ||
            !rays_from.contains("port")) {
            throw RayTracingRefusal(
                "pelican.ray_tracing.invalid_trace_dispatch@1", nlohmann::json{{"task", name}},
                "pelican.ray_tracing.invalid_trace_dispatch@1: "
                "dispatch.rays_from must contain exactly one image port: " +
                name);
        }
        auto port = parseStringField(
            rays_from, "port",
            "compute task dispatch.rays_from: " + name);
        if (port.empty()) {
            throw RayTracingRefusal(
                "pelican.ray_tracing.invalid_trace_dispatch@1", nlohmann::json{{"task", name}},
                "pelican.ray_tracing.invalid_trace_dispatch@1: "
                "dispatch.rays_from.port must not be empty: " + name);
        }
        dispatch.rays_from =
            ComputeImageExtentDispatchDefinition{
                .port = std::move(port)};
        return dispatch;
    }
    if (ray_tracing && !json.contains("indirect")) {
        throw RayTracingRefusal(
            "pelican.ray_tracing.trace_extent_required@1", nlohmann::json{{"task", name}},
            "pelican.ray_tracing.trace_extent_required@1: compute task '" +
            name + "' requires dispatch.rays_from, dispatch.rays, or dispatch.indirect");
    }

    if (json.contains("indirect")) {
        if (ray_tracing && json.size() != 1)
            throw RayTracingRefusal(
                "pelican.ray_tracing.invalid_trace_dispatch@1", nlohmann::json{{"task", name}},
                "pelican.ray_tracing.invalid_trace_dispatch@1: indirect trace cannot be combined with other dispatch fields: " + name);
        if (json.contains("groups") ||
            json.contains("groups_from") ||
            json.contains("local_size")) {
            throw std::runtime_error(
                "compute task dispatch.indirect cannot be combined with "
                "groups, groups_from, or local_size: " +
                name);
        }
        const auto &indirect = json.at("indirect");
        if (!indirect.is_object()) {
            throw std::runtime_error(
                "compute task dispatch.indirect must be an object: " +
                name);
        }
        ComputeIndirectDispatchDefinition definition{
            .buffer = parseStringField(
                indirect, "buffer",
                "compute task dispatch.indirect: " +
                    name),
            .offset =
                indirect.contains("offset")
                    ? requireDeviceSize(
                          indirect, "offset",
                          "compute task dispatch.indirect: " +
                              name)
                    : vk::DeviceSize{0},
        };
        if (definition.buffer.empty()) {
            throw std::runtime_error(
                "compute task dispatch.indirect buffer must not be empty: " +
                name);
        }
        if (definition.offset %
                frameGraphIndirectCommandAlignment !=
            0) {
            throw std::runtime_error(
                "compute task dispatch.indirect offset must be "
                "4-byte aligned: " +
                name);
        }
        dispatch.indirect = std::move(definition);
        return dispatch;
    }

    if (json.contains("groups") &&
        json.contains("groups_from")) {
        throw std::runtime_error(
            "compute task dispatch.groups cannot be combined with "
            "groups_from: " +
            name);
    }
    if (json.contains("local_size")) {
        throw std::runtime_error(
            "compute task dispatch.local_size is not authored; "
            "declare the workgroup size in the compute shader: " +
            name);
    }
    if (json.contains("groups")) {
        const auto &groups = json.at("groups");
        if (groups.is_array()) {
            if (groups.size() != 3) {
                throw std::runtime_error("compute task dispatch.groups must have three entries: " + name);
            }
            for (const auto &entry : groups) {
                if (!entry.is_number_integer()) {
                    throw std::runtime_error("compute task dispatch.groups entries must be integers: " + name);
                }
            }
            dispatch.groups_x = groups.at(0).get<uint32_t>();
            dispatch.groups_y = groups.at(1).get<uint32_t>();
            dispatch.groups_z = groups.at(2).get<uint32_t>();
        } else if (groups.is_object()) {
            dispatch.groups_x = groups.value("x", 1u);
            dispatch.groups_y = groups.value("y", 1u);
            dispatch.groups_z = groups.value("z", 1u);
        } else {
            throw std::runtime_error("compute task dispatch.groups must be an array or object: " + name);
        }
    }
    if (json.contains("groups_from")) {
        const auto &groups_from =
            json.at("groups_from");
        if (!groups_from.is_object()) {
            throw std::runtime_error(
                "compute task dispatch.groups_from must be an "
                "object containing an image resource port: " +
                name);
        }
        for (auto field = groups_from.begin();
             field != groups_from.end(); ++field) {
            if (field.key() != "port") {
                throw std::runtime_error(
                    "compute task dispatch.groups_from has unknown "
                    "field '" +
                    field.key() + "': " + name);
            }
        }
        auto port = parseStringField(
            groups_from, "port",
            "compute task dispatch.groups_from: " +
                name);
        if (port.empty()) {
            throw std::runtime_error(
                "compute task dispatch.groups_from.port must not be "
                "empty: " +
                name);
        }
        dispatch.groups_from =
            ComputeImageExtentDispatchDefinition{
                .port = std::move(port),
            };
    }
    if (dispatch.groups_x == 0 || dispatch.groups_y == 0 || dispatch.groups_z == 0) {
        throw std::runtime_error("compute task dispatch group counts must be positive: " + name);
    }
    return dispatch;
}
} // namespace

std::string_view frameGraphHostBufferSourceName(
    FrameGraphHostBufferSource source) {
    switch (source) {
    case FrameGraphHostBufferSource::runtime_f32:
        return "runtime_f32";
    case FrameGraphHostBufferSource::scene_lights_v2:
        return "scene_lights_v2";
    case FrameGraphHostBufferSource::view_family_matrices_v1:
        return "view_family_matrices_v1";
    case FrameGraphHostBufferSource::
        scene_draw_commands_v1:
        return "scene_draw_commands_v1";
    case FrameGraphHostBufferSource::
        scene_draw_bounds_v1:
        return "scene_draw_bounds_v1";
    case FrameGraphHostBufferSource::
        scene_draw_segments_v1:
        return "scene_draw_segments_v1";
    }
    throw std::runtime_error(
        "unknown frame-graph host buffer source");
}

std::string_view frameGraphBufferCommandLayoutName(
    FrameGraphBufferCommandLayout layout) {
    switch (layout) {
    case FrameGraphBufferCommandLayout::
        compute_dispatch:
        return "compute_dispatch";
    case FrameGraphBufferCommandLayout::ray_dispatch:
        return "ray_dispatch";
    case FrameGraphBufferCommandLayout::
        indexed_draw:
        return "indexed_draw";
    case FrameGraphBufferCommandLayout::
        draw_count:
        return "draw_count";
    }
    throw std::runtime_error(
        "unknown frame-graph buffer command layout");
}

std::string_view computeTaskScheduleName(
    ComputeTaskSchedule schedule) {
    switch (schedule) {
    case ComputeTaskSchedule::per_frame:
        return "per_frame";
    case ComputeTaskSchedule::per_view:
        return "per_view";
    case ComputeTaskSchedule::once:
        return "once";
    }
    throw std::runtime_error(
        "unknown compute task schedule");
}

std::vector<FrameGraphBufferDefinition> parseFrameGraphBufferDefinitionsFromJson(const nlohmann::json &config_json) {
    std::vector<FrameGraphBufferDefinition> definitions;
    if (!config_json.contains("buffers")) {
        return definitions;
    }
    const auto &buffers = config_json.at("buffers");
    if (!buffers.is_array()) {
        throw std::runtime_error("buffers must be an array");
    }

    definitions.reserve(buffers.size());
    for (const auto &entry : buffers) {
        if (entry.is_string()) {
            auto name = entry.get<std::string>();
            validateAuthoredRenderResourceName(name, "buffer");
            definitions.push_back(
                FrameGraphBufferDefinition{
                    std::move(name),
                    0, true});
            continue;
        }
        if (!entry.is_object()) {
            throw std::runtime_error("buffers entries must be strings or objects");
        }
        const auto name =
            parseStringField(entry, "name", "buffer");
        validateAuthoredRenderResourceName(name, "buffer");
        const auto context = "buffer '" + name + "'";
        if (entry.contains("size") &&
            entry.contains("size_from_extent")) {
            throw std::runtime_error(
                context +
                " cannot declare both size and size_from_extent");
        }
        FrameGraphBufferDefinition definition{
            .name = name,
            .size =
                entry.contains("size")
                    ? requireDeviceSize(
                          entry, "size", context)
                    : vk::DeviceSize{0},
            .persistent = true,
        };
        definition.index_buffer = entry.value("index_buffer", false);
        if (entry.contains("size_from_extent")) {
            auto [extent_size, byte_size] =
                parseExtentDerivedBufferSize(
                    entry, config_json, context);
            definition.extent_size =
                std::move(extent_size);
            definition.size = byte_size;
        }
        if (definition.index_buffer && definition.size % sizeof(std::uint32_t) != 0)
            throw std::runtime_error(context + " index buffer size must be uint32 aligned");
        if (entry.contains("host_source")) {
            definition.host_source =
                parseHostBufferSource(
                    entry, context);
        }
        if (entry.contains("command_layout")) {
            definition.command_layout =
                parseBufferCommandLayout(
                    entry, context);
        }
        if (definition.host_source == FrameGraphHostBufferSource::runtime_f32 &&
            (definition.command_layout || definition.size % sizeof(float) != 0))
            throw std::runtime_error(context + " runtime_f32 requires a float-aligned data buffer");
        if (entry.contains("initial_f32")) {
            const auto &initial = entry.at("initial_f32");
            if (!initial.is_array())
                throw std::runtime_error(context + " initial_f32 must be a float array");
            for (const auto &value : initial) {
                if (!value.is_number())
                    throw std::runtime_error(context + " initial_f32 must be a float array");
                definition.initial_f32.push_back(value.get<float>());
            }
            if (definition.host_source != FrameGraphHostBufferSource::runtime_f32 ||
                definition.initial_f32.size() * sizeof(float) != definition.size)
                throw std::runtime_error(
                    context + " initial_f32 requires host_source 'runtime_f32' and one float per declared byte quad");
        }
        if (definition.host_source == FrameGraphHostBufferSource::view_family_matrices_v1) {
            definition.host_view_family = parseStringField(entry, "view_family", context);
            if (definition.host_view_family.empty() || definition.command_layout ||
                definition.size < 16 || definition.size % 16 != 0) {
                throw std::runtime_error(context +
                    " view_family_matrices_v1 requires a view_family and a uvec4-aligned data buffer");
            }
        } else if (entry.contains("view_family")) {
            throw std::runtime_error(context + " buffer view_family requires view_family_matrices_v1");
        }
        if (entry.contains("lifetime")) {
            const auto lifetime =
                parseStringField(
                    entry, "lifetime", context);
            if (lifetime == "persistent") {
                definition.persistent = true;
            } else if (lifetime == "transient") {
                definition.persistent = false;
            } else {
                throw std::runtime_error("buffer lifetime must be persistent or transient: " + definition.name);
            }
        }
        if (definition.host_source &&
            definition.size == 0) {
            throw std::runtime_error(
                context +
                " host_source requires a non-zero size");
        }
        if (definition.command_layout &&
            (*definition.command_layout == FrameGraphBufferCommandLayout::compute_dispatch ||
             *definition.command_layout == FrameGraphBufferCommandLayout::ray_dispatch) &&
            definition.size <
                compute_dispatch_command_size) {
            throw std::runtime_error(
                context +
                " command_layout '" +
                std::string{frameGraphBufferCommandLayoutName(*definition.command_layout)} +
                "' requires at least " +
                std::to_string(
                    compute_dispatch_command_size) +
                " bytes");
        }
        if (definition.command_layout ==
                FrameGraphBufferCommandLayout::
                    indexed_draw &&
            definition.size <
                frameGraphIndexedDrawCommandBytes) {
            throw std::runtime_error(
                context +
                " command_layout 'indexed_draw' requires at least " +
                std::to_string(
                    frameGraphIndexedDrawCommandBytes) +
                " bytes");
        }
        if (definition.command_layout ==
                FrameGraphBufferCommandLayout::
                    draw_count &&
            definition.size <
                frameGraphDrawCountBytes) {
            throw std::runtime_error(
                context +
                " command_layout 'draw_count' requires at least " +
                std::to_string(
                    frameGraphDrawCountBytes) +
                " bytes");
        }
        if (definition.host_source ==
                FrameGraphHostBufferSource::
                    scene_draw_commands_v1 &&
            definition.command_layout !=
                FrameGraphBufferCommandLayout::
                    indexed_draw) {
            throw std::runtime_error(
                context +
                " host_source 'scene_draw_commands_v1' requires "
                "command_layout 'indexed_draw'");
        }
        if (definition.host_source ==
                FrameGraphHostBufferSource::
                    scene_draw_commands_v1 &&
            definition.size %
                    frameGraphIndexedDrawCommandBytes !=
                0) {
            throw std::runtime_error(
                context +
                " host_source 'scene_draw_commands_v1' size must "
                "be a multiple of the indexed draw command size");
        }
        if (definition.host_source ==
                FrameGraphHostBufferSource::
                    scene_draw_bounds_v1 &&
            definition.command_layout) {
            throw std::runtime_error(
                context +
                " host_source 'scene_draw_bounds_v1' must not declare "
                "a command_layout");
        }
        if (definition.host_source ==
                FrameGraphHostBufferSource::
                    scene_draw_bounds_v1 &&
            definition.size %
                    frameGraphSceneDrawBoundsV1Bytes !=
                0) {
            throw std::runtime_error(
                context +
                " host_source 'scene_draw_bounds_v1' size must be a "
                "multiple of 32 bytes");
        }
        if (definition.host_source ==
                FrameGraphHostBufferSource::
                    scene_draw_segments_v1 &&
            definition.command_layout) {
            throw std::runtime_error(
                context +
                " host_source 'scene_draw_segments_v1' must not declare "
                "a command_layout");
        }
        if (definition.host_source ==
                FrameGraphHostBufferSource::
                    scene_draw_segments_v1 &&
            definition.size %
                    frameGraphSceneDrawSegmentV1Bytes !=
                0) {
            throw std::runtime_error(
                context +
                " host_source 'scene_draw_segments_v1' size must be a "
                "multiple of 32 bytes");
        }
        definitions.push_back(std::move(definition));
    }
    return definitions;
}

std::unordered_set<std::string> frameGraphBufferNameSet(const std::vector<FrameGraphBufferDefinition> &definitions) {
    std::unordered_set<std::string> names;
    for (const auto &definition : definitions) {
        if (!names.insert(definition.name).second) {
            throw std::runtime_error("Duplicate buffer name: " + definition.name);
        }
    }
    return names;
}

std::vector<ComputeTaskDefinition> parseComputeTaskDefinitionsFromConfigJson(
    const nlohmann::json &config_json,
    const RayTracingProjectConfig *ray_tracing_project) {
    std::vector<ComputeTaskDefinition> definitions;
    const auto tasks = config_json.value("compute_tasks", nlohmann::json::array());
    if (!tasks.is_array()) {
        throw std::runtime_error("compute_tasks must be an array");
    }

    definitions.reserve(tasks.size());
    for (const auto &task_json : tasks) {
        if (!task_json.is_object()) {
            throw std::runtime_error("compute_tasks entries must be objects");
        }
        ComputeTaskDefinition definition;
        definition.name = parseStringField(task_json, "name", "compute task");
        for (const auto *unsupported :
             {"any_hit", "anyhit", "intersection", "callable"}) {
            if (task_json.contains(unsupported)) {
                throw RayTracingRefusal(
                    "pelican.ray_tracing.unsupported_shader_stage@1", nlohmann::json{{"task", definition.name}, {"stage", std::string{unsupported}}},
                    "pelican.ray_tracing.unsupported_shader_stage@1: "
                    "compute task '" +
                    definition.name +
                    "' declares unsupported top-level ray tracing stage '" +
                    unsupported + "'");
            }
        }
        const auto ray_tracing =
            task_json.contains("ray_tracing");
        if (task_json.contains("ray_query")) {
            const auto &query = task_json.at("ray_query");
            if (ray_tracing || !query.is_object() || query.size() != 1 ||
                !query.contains("acceleration_structure")) {
                throw std::runtime_error("Compute task '" + definition.name +
                    "' ray_query requires only acceleration_structure and cannot accompany ray_tracing");
            }
            definition.ray_query_acceleration_structure =
                parseStringField(query, "acceleration_structure", definition.name);
            if (definition.ray_query_acceleration_structure->empty())
                throw std::runtime_error("Compute task '" + definition.name +
                    "' ray_query acceleration_structure must not be empty");
        }
        if (ray_tracing && task_json.contains("shader")) {
            throw RayTracingRefusal(
                "pelican.ray_tracing.ambiguous_pipeline_declaration@1", nlohmann::json{{"task", definition.name}},
                "pelican.ray_tracing.ambiguous_pipeline_declaration@1: "
                "compute task '" +
                definition.name +
                "' cannot declare both shader and ray_tracing");
        }
        if (ray_tracing) {
            definition.ray_tracing =
                parseRayTracingShaders(
                    task_json, definition.name,
                    ray_tracing_project);
            if (task_json.at("ray_tracing").contains("acceleration_structure"))
                definition.ray_tracing->acceleration_structure = parseStringField(
                    task_json.at("ray_tracing"), "acceleration_structure", definition.name);
        } else {
            definition.shader = makeShaderReference(
                parseStringField(
                    task_json, "shader",
                    "compute task: " + definition.name),
                ShaderStage::compute);
        }
        definition.shader_declaration_parsed = true;
        captureDeclaredComputeShaderReferences(
            definition, task_json);
        definition.reads = parseOptionalStringList(task_json, "reads", "compute task: " + definition.name);
        definition.writes = parseOptionalStringList(task_json, "writes", "compute task: " + definition.name);
        definition.after = parseOptionalStringList(task_json, "after", "compute task: " + definition.name);
        definition.before = parseOptionalStringList(task_json, "before", "compute task: " + definition.name);
        definition.view_family =
            parseRenderViewFamilyId(
                task_json,
                "Compute task '" +
                    definition.name + "'");
        definition.view_select =
            parseRenderViewSelect(
                task_json,
                "Compute task '" +
                    definition.name + "'");
        definition.resource_ports =
            parseShaderResourcePortDefinitions(
                task_json, definition.reads,
                definition.writes,
                "compute task '" + definition.name + "'");
        definition.dispatch = parseDispatch(
            task_json, definition.name, ray_tracing);
        const auto extent_from =
            definition.dispatch.groups_from
                ? definition.dispatch.groups_from
                : definition.dispatch.rays_from;
        if (extent_from) {
            const auto &port_name =
                extent_from->port;
            const auto port = std::find_if(
                definition.resource_ports.begin(),
                definition.resource_ports.end(),
                [&](const ShaderResourcePortDefinition
                        &candidate) {
                    return candidate.name ==
                           port_name;
                });
            if (port ==
                definition.resource_ports.end()) {
                throw std::runtime_error(
                    "compute task image extent dispatch references "
                    "unknown resource port '" +
                    port_name + "': " +
                    definition.name);
            }
            if (port->kind ==
                ShaderResourcePortKind::buffer) {
                throw std::runtime_error(
                    "compute task image extent dispatch port must be "
                    "an image: " +
                    definition.name);
            }
        }
        if (task_json.contains("schedule")) {
            const auto schedule =
                parseStringField(
                    task_json, "schedule",
                    "compute task: " +
                        definition.name);
            if (schedule == "per_frame") {
                definition.schedule =
                    ComputeTaskSchedule::per_frame;
            } else if (schedule == "per_view") {
                definition.schedule =
                    ComputeTaskSchedule::per_view;
            } else if (schedule == "once") {
                definition.schedule =
                    ComputeTaskSchedule::once;
            } else {
                throw std::runtime_error(
                    "compute task schedule supports per_frame, "
                    "per_view or once: " +
                    definition.name);
            }
        }
        definitions.push_back(std::move(definition));
    }
    if (const auto connections = config_json.find("ray_task_acceleration_structures");
        connections != config_json.end()) {
        if (!connections->is_object())
            throw std::runtime_error("ray_task_acceleration_structures must be an object");
        for (const auto &[name, selected] : connections->items()) {
            if (name.empty() || !selected.is_string() || selected.get_ref<const std::string &>().empty())
                throw std::runtime_error("ray_task_acceleration_structures requires nonempty task and AS names");
            const auto task = std::find_if(definitions.begin(), definitions.end(),
                [&](const auto &entry) { return entry.name == name; });
            if (task == definitions.end())
                throw std::runtime_error("ray_task_acceleration_structures references unknown task '" + name + "'");
            if (task->ray_tracing)
                task->ray_tracing->acceleration_structure = selected.get<std::string>();
            else if (task->ray_query_acceleration_structure)
                task->ray_query_acceleration_structure = selected.get<std::string>();
            else
                throw std::runtime_error("ray_task_acceleration_structures task '" + name + "' does not declare ray_tracing or ray_query");
        }
    }
    if (const auto records = config_json.find("ray_task_callable_records"); records != config_json.end()) {
        if (!records->is_object())
            throw std::runtime_error("ray_task_callable_records must be an object");
        for (const auto &[name, values] : records->items()) {
            const auto task = std::find_if(definitions.begin(), definitions.end(),
                [&](const auto &entry) { return entry.name == name; });
            if (task == definitions.end() || !task->ray_tracing || !task->ray_tracing->callable_record_layout)
                throw std::runtime_error("ray_task_callable_records references task without a callable record schema: " + name);
            auto &ray = *task->ray_tracing;
            ray.callable_records = encodeRayCallableRecords(values, *ray.callable_record_layout, ray.callables.size(), name);
        }
    }
    return definitions;
}

void validateComputeTaskBufferContracts(
    std::span<const FrameGraphBufferDefinition>
        buffer_definitions,
    std::span<const ComputeTaskDefinition>
        task_definitions) {
    for (const auto &task : task_definitions) {
        if (!task.dispatch.indirect) continue;
        const auto &indirect =
            *task.dispatch.indirect;
        if (std::find(
                task.writes.begin(),
                task.writes.end(),
                indirect.buffer) !=
            task.writes.end()) {
            throw std::runtime_error(
                "compute task '" + task.name +
                "' cannot write its own indirect dispatch "
                "buffer; use a separate producer task");
        }
        const auto buffer = std::find_if(
            buffer_definitions.begin(),
            buffer_definitions.end(),
            [&](const FrameGraphBufferDefinition
                    &candidate) {
                return candidate.name ==
                       indirect.buffer;
            });
        if (buffer == buffer_definitions.end()) {
            throw std::runtime_error(
                "compute task '" + task.name +
                "' indirect dispatch references unknown buffer '" +
                indirect.buffer + "'");
        }
        const auto expected_layout = task.ray_tracing
            ? FrameGraphBufferCommandLayout::ray_dispatch
            : FrameGraphBufferCommandLayout::compute_dispatch;
        if (buffer->command_layout != expected_layout) {
            throw std::runtime_error(
                "compute task '" + task.name +
                "' indirect dispatch buffer '" +
                indirect.buffer +
                "' requires command_layout '" +
                std::string{frameGraphBufferCommandLayoutName(expected_layout)} + "'");
        }
        if (indirect.offset %
                frameGraphIndirectCommandAlignment !=
            0) {
            throw std::runtime_error(
                "compute task '" + task.name +
                "' indirect dispatch offset must be 4-byte "
                "aligned");
        }
        if (indirect.offset > buffer->size ||
            buffer->size - indirect.offset <
                compute_dispatch_command_size) {
            throw std::runtime_error(
                "compute task '" + task.name +
                "' indirect dispatch command at offset " +
                std::to_string(indirect.offset) +
                " exceeds buffer '" +
                indirect.buffer + "' size " +
                std::to_string(buffer->size));
        }
    }
}
} // namespace Pelican
