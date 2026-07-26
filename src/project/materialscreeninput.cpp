#include "materialscreeninput.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Pelican {

std::string_view materialPassInputSamplingName(
    MaterialPassInputSampling sampling) {
    switch (sampling) {
    case MaterialPassInputSampling::linear_repeat:
        return "linear_repeat";
    case MaterialPassInputSampling::nearest_clamp_to_edge:
        return "nearest_clamp_to_edge";
    }
    throw std::runtime_error("unknown material pass input sampling");
}

std::string_view materialPassInputViewPolicyName(
    MaterialPassInputViewPolicy policy) {
    switch (policy) {
    case MaterialPassInputViewPolicy::consumer_view:
        return "consumer_view";
    case MaterialPassInputViewPolicy::shared_2d:
        return "shared_2d";
    }
    throw std::runtime_error("unknown material pass input view policy");
}

std::string_view materialPassInputFallbackName(
    MaterialPassInputFallback fallback) {
    switch (fallback) {
    case MaterialPassInputFallback::required:
        return "required";
    case MaterialPassInputFallback::fully_lit:
        return "fully_lit";
    }
    throw std::runtime_error("unknown material pass input fallback");
}

std::string_view materialPassInputRelationKindName(
    MaterialPassInputRelationKind kind) {
    switch (kind) {
    case MaterialPassInputRelationKind::directional_light_shadow_v1:
        return "pelican.light.directional_shadow@1";
    }
    throw std::runtime_error("unknown material pass input relation");
}

MaterialPassInputContract makeBuiltinMaterialPassInputContract(
    const LogicalTypeRegistry &types, std::string_view name) {
    if (name == "opaque_color") {
        const auto scene = sceneLinearHdrV1(types);
        return {std::string{name}, scene, scene,
                {LogicalReadFootprintKind::neighborhood, std::nullopt},
                std::nullopt};
    }
    if (name == "opaque_depth" || name == "scene_depth") {
        const auto depth = deviceDepthV1(types);
        return {
            .name = std::string{name},
            .source_type = depth,
            .sampled_type = depth,
            .footprint = {
                LogicalReadFootprintKind::same_pixel,
                std::nullopt},
            .sampling =
                MaterialPassInputSampling::nearest_clamp_to_edge,
        };
    }
    if (name == "linear_view_depth") {
        return {
            .name = std::string{name},
            .source_type = deviceDepthV1(types),
            .sampled_type = linearViewDepthV1(types),
            .footprint = {
                LogicalReadFootprintKind::same_pixel,
                std::nullopt},
            .conversion =
                std::string{"pelican.render.depth_linearize@1"},
            .sampling =
                MaterialPassInputSampling::nearest_clamp_to_edge,
        };
    }
    if (name == directionalShadowInputContractName) {
        const auto depth = deviceDepthV1(types);
        return {
            .name = std::string{name},
            .source_type = depth,
            .sampled_type = depth,
            .footprint = {
                LogicalReadFootprintKind::arbitrary,
                std::nullopt},
            .sampling =
                MaterialPassInputSampling::nearest_clamp_to_edge,
            .view_policy =
                MaterialPassInputViewPolicy::shared_2d,
            .fallback =
                MaterialPassInputFallback::fully_lit,
            .relation =
                MaterialPassInputRelation{
                    .kind = MaterialPassInputRelationKind::
                        directional_light_shadow_v1,
                    .light_index = 0,
                    .transform =
                        "pelican.light.shadow_view_projection@1",
                },
        };
    }
    throw std::runtime_error("unknown material pass input contract: " +
                             std::string{name});
}

MaterialScreenInputContract makeBuiltinMaterialScreenInputContract(
    const LogicalTypeRegistry &types, std::string_view name) {
    if (name == directionalShadowInputContractName) {
        throw std::runtime_error(
            "material screen input contract '" + std::string{name} +
            "' is feature-owned and cannot be authored by a .surface");
    }
    return makeBuiltinMaterialPassInputContract(types, name);
}

ResolvedMaterialScreenInputContract resolveMaterialScreenInputContract(
    const LogicalTypeRegistry &types,
    const LogicalTypeConversionRegistry &conversions,
    MaterialScreenInputContract contract,
    const LogicalType &actual_source_type) {
    types.requireCanonical(actual_source_type);
    if (actual_source_type != contract.source_type) {
        throw std::runtime_error(
            "material screen input '" + contract.name +
            "' source type does not match its contract");
    }

    const auto match = conversions.match(
        types, actual_source_type,
        exactLogicalTypePattern(types, contract.sampled_type));
    if (match.status == LogicalTypeMatchStatus::rejected) {
        throw std::runtime_error(
            "material screen input '" + contract.name +
            "' cannot produce its sampled type: " + match.reason_code +
            " (" + match.detail + ")");
    }
    if (contract.conversion) {
        if (match.conversion_path.size() != 1 ||
            match.conversion_path.front() != *contract.conversion) {
            throw std::runtime_error(
                "material screen input '" + contract.name +
                "' resolved an unexpected conversion path");
        }
    } else if (!match.conversion_path.empty()) {
        throw std::runtime_error(
            "material screen input '" + contract.name +
            "' unexpectedly requires a conversion");
    }
    return {std::move(contract), match.conversion_path};
}

void validateMaterialScreenInputInterfaceReflection(
    std::size_t declared_input_count,
    std::span<const MaterialScreenInputReflectionBinding> reflection,
    std::uint32_t expected_set) {
    std::vector<MaterialScreenInputReflectionBinding> bindings;
    for (const auto &binding : reflection) {
        if (binding.set == expected_set) bindings.push_back(binding);
    }
    std::sort(bindings.begin(), bindings.end(),
              [](const auto &left, const auto &right) {
                  return left.binding < right.binding;
              });
    if (bindings.size() != declared_input_count) {
        throw std::runtime_error(
            "material screen inputs do not match shader reflection binding count");
    }
    for (std::size_t index = 0; index < bindings.size(); ++index) {
        if (bindings[index].binding != index ||
            bindings[index].kind !=
                MaterialScreenInputReflectionKind::combined_image_sampler) {
            throw std::runtime_error(
                "material screen inputs require consecutive combined image samplers in set 1");
        }
    }
}

std::vector<MaterialPassInputContract>
resolveMaterialPassInputInterfaceReflection(
    const LogicalTypeRegistry &types,
    std::span<const MaterialScreenInputContract> declared_screen_inputs,
    std::span<const MaterialScreenInputReflectionBinding> reflection,
    std::uint32_t expected_set) {
    std::vector<MaterialScreenInputReflectionBinding> bindings;
    for (const auto &binding : reflection) {
        if (binding.set == expected_set) {
            bindings.push_back(binding);
        }
    }
    std::sort(bindings.begin(), bindings.end(),
              [](const auto &left, const auto &right) {
                  return left.binding < right.binding;
              });

    if (bindings.size() < declared_screen_inputs.size()) {
        throw std::runtime_error(
            "material screen inputs do not match shader reflection binding count");
    }

    std::vector<MaterialPassInputContract> resolved;
    resolved.reserve(bindings.size());
    for (std::size_t index = 0; index < bindings.size(); ++index) {
        const auto &binding = bindings[index];
        if (binding.binding != index ||
            binding.kind !=
                MaterialScreenInputReflectionKind::combined_image_sampler) {
            throw std::runtime_error(
                "material pass inputs require consecutive combined image samplers in set 1");
        }
        if (index < declared_screen_inputs.size()) {
            const auto &declared = declared_screen_inputs[index];
            const auto expected_name =
                "pelican_screen_" + declared.name + "_texture";
            if (!binding.name.empty() &&
                binding.name != expected_name) {
                throw std::runtime_error(
                    "material screen input '" + declared.name +
                    "' reflected as unexpected sampler '" +
                    binding.name + "'");
            }
            resolved.push_back(declared);
            continue;
        }
        if (binding.name == directionalShadowSamplerName) {
            if (std::any_of(
                    resolved.begin(), resolved.end(),
                    [](const auto &contract) {
                        return contract.name ==
                               directionalShadowInputContractName;
                    })) {
                throw std::runtime_error(
                    "material shader reflects directional shadow more than once");
            }
            resolved.push_back(
                makeBuiltinMaterialPassInputContract(
                    types, directionalShadowInputContractName));
            continue;
        }
        throw std::runtime_error(
            "material shader reflects unknown feature pass input '" +
            binding.name + "'");
    }
    return resolved;
}

} // namespace Pelican
