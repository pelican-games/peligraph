#pragma once

#include "logicalrendergraph.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Pelican {

// A public surface names a screen input, while the compiler carries the
// semantic source/result types and sampling footprint independently of the
// Vulkan image chosen later.  The fixed aliases below are the hybrid_v1 ABI;
// a future registry may add names without changing this data shape.
struct MaterialScreenInputContract {
    std::string name;
    LogicalType source_type;
    LogicalType sampled_type;
    LogicalReadFootprint footprint;
    std::optional<std::string> conversion;

    bool operator==(const MaterialScreenInputContract &) const = default;
};

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
    unsupported,
};

struct MaterialScreenInputReflectionBinding {
    std::uint32_t set = 0;
    std::uint32_t binding = 0;
    MaterialScreenInputReflectionKind kind =
        MaterialScreenInputReflectionKind::unsupported;

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

} // namespace Pelican
