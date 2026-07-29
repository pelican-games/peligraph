#pragma once

#include "logicalrendergraph.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

enum class MaterialPassInputSampling : std::uint8_t {
    linear_repeat,
    nearest_clamp_to_edge,
};

std::string_view materialPassInputSamplingName(
    MaterialPassInputSampling sampling);

enum class MaterialPassInputViewPolicy : std::uint8_t {
    consumer_view,
    shared_2d,
    family_array,
};

std::string_view materialPassInputViewPolicyName(
    MaterialPassInputViewPolicy policy);

enum class MaterialPassInputFallback : std::uint8_t {
    required,
    fully_lit,
};

std::string_view materialPassInputFallbackName(
    MaterialPassInputFallback fallback);

enum class MaterialPassInputRelationKind : std::uint8_t {
    directional_light_shadow_v1,
};

std::string_view materialPassInputRelationKindName(
    MaterialPassInputRelationKind kind);

struct MaterialPassInputRelation {
    MaterialPassInputRelationKind kind =
        MaterialPassInputRelationKind::directional_light_shadow_v1;
    std::uint32_t light_index = 0;
    std::string transform;

    bool operator==(const MaterialPassInputRelation &) const = default;
};

// A public surface or feature names a pass input, while the compiler carries
// its semantic source/result types and relation independently of the Vulkan
// image chosen later. screen_inputs are the user-authored subset of this
// contract; feature-owned inputs use the same set=1 ABI.
struct MaterialPassInputContract {
    std::string name;
    LogicalType source_type;
    LogicalType sampled_type;
    LogicalReadFootprint footprint;
    std::optional<std::string> conversion;
    MaterialPassInputSampling sampling =
        MaterialPassInputSampling::linear_repeat;
    MaterialPassInputViewPolicy view_policy =
        MaterialPassInputViewPolicy::consumer_view;
    MaterialPassInputFallback fallback =
        MaterialPassInputFallback::required;
    std::optional<MaterialPassInputRelation> relation;

    bool operator==(const MaterialPassInputContract &) const = default;
};

using MaterialScreenInputContract = MaterialPassInputContract;

inline constexpr std::string_view directionalShadowInputContractName =
    "directional_shadow";
inline constexpr std::string_view directionalShadowSamplerName =
    "pelican_directional_shadow_texture";

MaterialPassInputContract makeBuiltinMaterialPassInputContract(
    const LogicalTypeRegistry &types, std::string_view name);

// Public .surface screen_inputs deliberately exclude feature-owned contracts.
MaterialScreenInputContract makeBuiltinMaterialScreenInputContract(
    const LogicalTypeRegistry &types, std::string_view name);

struct ResolvedMaterialScreenInputContract {
    MaterialScreenInputContract contract;
    std::vector<std::string> conversion_path;
};

ResolvedMaterialScreenInputContract resolveMaterialScreenInputContract(
    const LogicalTypeRegistry &types,
    const LogicalTypeConversionRegistry &conversions,
    MaterialScreenInputContract contract,
    const LogicalType &actual_source_type);

enum class MaterialScreenInputReflectionKind : std::uint8_t {
    combined_image_sampler,
    input_attachment,
    unsupported,
};

struct MaterialScreenInputReflectionBinding {
    std::uint32_t set = 0;
    std::uint32_t binding = 0;
    MaterialScreenInputReflectionKind kind =
        MaterialScreenInputReflectionKind::unsupported;
    std::string name;
    std::optional<std::uint32_t>
        input_attachment_index;

    bool operator==(const MaterialScreenInputReflectionBinding &) const =
        default;
};

// The project compiler owns this Vulkan-independent shape check. The runtime
// maps backend reflection types into this small vocabulary and cannot silently
// bind a declaration to a different set/binding layout.
void validateMaterialScreenInputInterfaceReflection(
    std::size_t declared_input_count,
    std::span<const MaterialScreenInputReflectionBinding> reflection,
    std::uint32_t expected_set = 1);

// Resolves the actual set=1 order from backend reflection. The declared
// screen inputs remain first and feature-owned stable sampler names may append
// typed contracts. Unknown or reordered bindings are rejected before a
// descriptor set is allocated.
std::vector<MaterialPassInputContract>
resolveMaterialPassInputInterfaceReflection(
    const LogicalTypeRegistry &types,
    std::span<const MaterialScreenInputContract> declared_screen_inputs,
    std::span<const MaterialScreenInputReflectionBinding> reflection,
    std::uint32_t expected_set = 1);

} // namespace Pelican
