#pragma once

#include "logicalrendergraph.hpp"

#include <optional>
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

} // namespace Pelican
