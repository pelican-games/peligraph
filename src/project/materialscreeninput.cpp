#include "materialscreeninput.hpp"

#include <stdexcept>
#include <utility>

namespace Pelican {

MaterialScreenInputContract makeBuiltinMaterialScreenInputContract(
    const LogicalTypeRegistry &types, std::string_view name) {
    if (name == "opaque_color") {
        const auto scene = sceneLinearHdrV1(types);
        return {std::string{name}, scene, scene,
                {LogicalReadFootprintKind::neighborhood, std::nullopt},
                std::nullopt};
    }
    if (name == "opaque_depth" || name == "scene_depth") {
        const auto depth = deviceDepthV1(types);
        return {std::string{name}, depth, depth,
                {LogicalReadFootprintKind::same_pixel, std::nullopt},
                std::nullopt};
    }
    if (name == "linear_view_depth") {
        return {std::string{name}, deviceDepthV1(types), linearViewDepthV1(types),
                {LogicalReadFootprintKind::same_pixel, std::nullopt},
                std::string{"pelican.render.depth_linearize@1"}};
    }
    throw std::runtime_error("unknown material screen input contract: " +
                             std::string{name});
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

} // namespace Pelican
