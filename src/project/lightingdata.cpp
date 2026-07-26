#include "lightingdata.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace Pelican {

namespace {

std::string requireString(
    const nlohmann::json &json,
    std::string_view field,
    std::string_view context) {
    if (!json.contains(field) ||
        !json.at(field).is_string()) {
        throw std::runtime_error(
            std::string{context} +
            " requires string field: " +
            std::string{field});
    }
    const auto value =
        json.at(field).get<std::string>();
    if (value.empty()) {
        throw std::runtime_error(
            std::string{context} +
            " field must not be empty: " +
            std::string{field});
    }
    return value;
}

std::uint32_t requirePositiveUint32(
    const nlohmann::json &json,
    std::string_view field,
    std::string_view context) {
    if (!json.contains(field) ||
        !json.at(field).is_number_integer()) {
        throw std::runtime_error(
            std::string{context} +
            " requires positive integer field: " +
            std::string{field});
    }
    if (json.at(field).is_number_unsigned()) {
        const auto value =
            json.at(field).get<std::uint64_t>();
        if (value == 0 ||
            value >
                std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error(
                std::string{context} +
                " field is outside uint32 positive range: " +
                std::string{field});
        }
        return static_cast<std::uint32_t>(value);
    }
    const auto value =
        json.at(field).get<std::int64_t>();
    if (value <= 0 ||
        value >
            std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            std::string{context} +
            " field is outside uint32 positive range: " +
            std::string{field});
    }
    return static_cast<std::uint32_t>(value);
}

LightingTargetPath parseTargetPath(
    const nlohmann::json &json,
    std::string_view field,
    std::string_view context) {
    const auto value =
        requireString(json, field, context);
    if (value == "compute_clustered") {
        return LightingTargetPath::
            compute_clustered;
    }
    if (value == "small_light_v1") {
        return LightingTargetPath::
            small_light_v1;
    }
    throw std::runtime_error(
        std::string{context} +
        " has unknown target path '" + value +
        "' for " + std::string{field});
}

bool declaresBuffer(
    const nlohmann::json &config,
    std::string_view name) {
    if (!config.contains("buffers") ||
        !config.at("buffers").is_array()) {
        return false;
    }
    for (const auto &buffer :
         config.at("buffers")) {
        if (buffer.is_string() &&
            buffer.get_ref<const std::string &>() ==
                name) {
            return true;
        }
        if (buffer.is_object() &&
            buffer.value("name", std::string{}) ==
                name) {
            return true;
        }
    }
    return false;
}

bool declaresComputeTask(
    const nlohmann::json &config,
    std::string_view name) {
    if (!config.contains("compute_tasks") ||
        !config.at("compute_tasks").is_array()) {
        return false;
    }
    return std::any_of(
        config.at("compute_tasks").begin(),
        config.at("compute_tasks").end(),
        [name](const nlohmann::json &task) {
            return task.is_object() &&
                   task.value(
                       "name", std::string{}) ==
                       name;
        });
}

} // namespace

std::string_view lightingOverflowPolicyName(
    LightingOverflowPolicy policy) {
    switch (policy) {
    case LightingOverflowPolicy::
        deterministic_truncate:
        return "deterministic_truncate";
    }
    throw std::runtime_error(
        "unknown lighting overflow policy");
}

std::string_view lightingTargetPathName(
    LightingTargetPath path) {
    switch (path) {
    case LightingTargetPath::compute_clustered:
        return "compute_clustered";
    case LightingTargetPath::small_light_v1:
        return "small_light_v1";
    }
    throw std::runtime_error(
        "unknown lighting target path");
}

std::optional<CompiledLightingDataPlan>
compileLightingDataPlan(
    const nlohmann::json &config) {
    if (!config.contains("lighting_data")) {
        return std::nullopt;
    }
    const auto &encoded =
        config.at("lighting_data");
    if (!encoded.is_object()) {
        throw std::runtime_error(
            "lighting_data must be an object");
    }
    constexpr std::array known_fields{
        std::string_view{"provider_feature"},
        std::string_view{"provider_reference"},
        std::string_view{"inventory_contract"},
        std::string_view{"selection_contract"},
        std::string_view{"inventory_resource"},
        std::string_view{"selection_resource"},
        std::string_view{"selector_task"},
        std::string_view{"algorithm"},
        std::string_view{"tile_width"},
        std::string_view{"tile_height"},
        std::string_view{"max_lights_per_tile"},
        std::string_view{"overflow"},
        std::string_view{"desktop_path"},
        std::string_view{"tile_gpu_path"},
        std::string_view{"xr_path"},
        std::string_view{"tile_gpu_fallback_reason"},
        std::string_view{"xr_fallback_reason"},
    };
    for (auto field = encoded.begin();
         field != encoded.end(); ++field) {
        if (std::find(
                known_fields.begin(),
                known_fields.end(),
                field.key()) ==
            known_fields.end()) {
            throw std::runtime_error(
                "lighting_data has unknown field: " +
                field.key());
        }
    }

    CompiledLightingDataPlan result{
        .provider_feature =
            requireString(
                encoded, "provider_feature",
                "lighting_data"),
        .provider_reference =
            requireString(
                encoded, "provider_reference",
                "lighting_data"),
        .inventory_contract =
            requireString(
                encoded, "inventory_contract",
                "lighting_data"),
        .selection_contract =
            requireString(
                encoded, "selection_contract",
                "lighting_data"),
        .inventory_resource =
            requireString(
                encoded, "inventory_resource",
                "lighting_data"),
        .selection_resource =
            requireString(
                encoded, "selection_resource",
                "lighting_data"),
        .selector_task =
            requireString(
                encoded, "selector_task",
                "lighting_data"),
        .algorithm =
            requireString(
                encoded, "algorithm",
                "lighting_data"),
        .tile_width =
            requirePositiveUint32(
                encoded, "tile_width",
                "lighting_data"),
        .tile_height =
            requirePositiveUint32(
                encoded, "tile_height",
                "lighting_data"),
        .max_lights_per_tile =
            requirePositiveUint32(
                encoded,
                "max_lights_per_tile",
                "lighting_data"),
    };
    const auto overflow =
        requireString(
            encoded, "overflow",
            "lighting_data");
    if (overflow != "deterministic_truncate") {
        throw std::runtime_error(
            "lighting_data has unknown overflow policy: " +
            overflow);
    }
    result.desktop_path =
        parseTargetPath(
            encoded, "desktop_path",
            "lighting_data");
    result.tile_gpu_path =
        parseTargetPath(
            encoded, "tile_gpu_path",
            "lighting_data");
    result.xr_path =
        parseTargetPath(
            encoded, "xr_path",
            "lighting_data");
    result.tile_gpu_fallback_reason =
        requireString(
            encoded,
            "tile_gpu_fallback_reason",
            "lighting_data");
    result.xr_fallback_reason =
        requireString(
            encoded,
            "xr_fallback_reason",
            "lighting_data");

    for (const auto &resource :
         {result.inventory_resource,
          result.selection_resource}) {
        if (!declaresBuffer(config, resource)) {
            throw std::runtime_error(
                "lighting_data references undeclared buffer: " +
                resource);
        }
    }
    if (!declaresComputeTask(
            config, result.selector_task)) {
        throw std::runtime_error(
            "lighting_data references undeclared selector task: " +
            result.selector_task);
    }
    return result;
}

nlohmann::json lightingDataPlanToJson(
    const CompiledLightingDataPlan &plan) {
    return {
        {"provider_feature",
         plan.provider_feature},
        {"provider_reference",
         plan.provider_reference},
        {"inventory_contract",
         plan.inventory_contract},
        {"selection_contract",
         plan.selection_contract},
        {"inventory_resource",
         plan.inventory_resource},
        {"selection_resource",
         plan.selection_resource},
        {"selector_task", plan.selector_task},
        {"algorithm", plan.algorithm},
        {"tile_width", plan.tile_width},
        {"tile_height", plan.tile_height},
        {"max_lights_per_tile",
         plan.max_lights_per_tile},
        {"overflow",
         lightingOverflowPolicyName(
             plan.overflow)},
        {"desktop_path",
         lightingTargetPathName(
             plan.desktop_path)},
        {"tile_gpu_path",
         lightingTargetPathName(
             plan.tile_gpu_path)},
        {"xr_path",
         lightingTargetPathName(
             plan.xr_path)},
        {"tile_gpu_fallback_reason",
         plan.tile_gpu_fallback_reason},
        {"xr_fallback_reason",
         plan.xr_fallback_reason},
    };
}

} // namespace Pelican
