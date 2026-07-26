#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace Pelican {

enum class LightingOverflowPolicy : std::uint8_t {
    deterministic_truncate,
};

std::string_view lightingOverflowPolicyName(
    LightingOverflowPolicy policy);

enum class LightingTargetPath : std::uint8_t {
    compute_clustered,
    small_light_v1,
};

std::string_view lightingTargetPathName(
    LightingTargetPath path);

struct CompiledLightingDataPlan {
    std::string provider_feature;
    std::string provider_reference;
    std::string inventory_contract;
    std::string selection_contract;
    std::string inventory_resource;
    std::string selection_resource;
    std::string selector_task;
    // Opaque project-owned implementation identity. The engine does not
    // branch on it.
    std::string algorithm;
    std::uint32_t tile_width = 1;
    std::uint32_t tile_height = 1;
    std::uint32_t max_lights_per_tile = 1;
    LightingOverflowPolicy overflow =
        LightingOverflowPolicy::
            deterministic_truncate;
    LightingTargetPath desktop_path =
        LightingTargetPath::compute_clustered;
    LightingTargetPath tile_gpu_path =
        LightingTargetPath::small_light_v1;
    LightingTargetPath xr_path =
        LightingTargetPath::small_light_v1;
    std::string tile_gpu_fallback_reason;
    std::string xr_fallback_reason;

    bool operator==(
        const CompiledLightingDataPlan &) const = default;
};

std::optional<CompiledLightingDataPlan>
compileLightingDataPlan(
    const nlohmann::json &config);

nlohmann::json lightingDataPlanToJson(
    const CompiledLightingDataPlan &plan);

} // namespace Pelican
