#include "gltfmateriallowering.hpp"

#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {

MaterialAlphaMode parseGltfAlphaMode(
    std::string_view value,
    std::string_view material_name) {
    if (value == "OPAQUE") {
        return MaterialAlphaMode::opaque;
    }
    if (value == "MASK") {
        return MaterialAlphaMode::mask;
    }
    if (value == "BLEND") {
        return MaterialAlphaMode::blend;
    }
    throw std::runtime_error(
        "glTF material '" +
        std::string{material_name} +
        "' has unsupported alphaMode '" +
        std::string{value} + "'");
}

std::string openPbrSurfaceReference(
    const MaterialVariantRouting &routing) {
    const auto sided =
        routing.double_sided ? "double" : "single";
    switch (routing.alpha_mode) {
    case MaterialAlphaMode::opaque:
        return std::string{
            "engine://surfaces/openpbr/opaque_"} +
               sided + ".surface";
    case MaterialAlphaMode::mask:
        return std::string{
            "engine://surfaces/openpbr/mask_"} +
               sided + ".surface";
    case MaterialAlphaMode::blend:
        return std::string{
            "engine://surfaces/openpbr/blend_"} +
               sided + ".surface";
    }
    throw std::runtime_error(
        "glTF material routing has an unknown alpha mode");
}

SurfaceParamValue floatValue(double value) {
    SurfaceParamValue result;
    result.type = SurfaceParamType::floating;
    result.values[0] = value;
    result.component_count = 1;
    return result;
}

} // namespace

MaterialDefinition makeGltfCoreMaterialDefinition(
    GltfCoreMaterialDescription description) {
    MaterialDefinition result;
    result.name = std::move(description.name);
    result.base = std::move(description.base);
    result.routing = MaterialVariantRouting{
        .alpha_mode = parseGltfAlphaMode(
            description.alpha_mode, result.name),
        .double_sided = description.double_sided,
    };
    result.surface =
        openPbrSurfaceReference(*result.routing);
    result.defines.emplace_back(
        gltfCoreOpenPbrSurfaceDefine);
    result.values.push_back(MaterialValue{
        .name = "alpha_cutoff",
        .value = floatValue(
            description.alpha_cutoff),
    });
    return result;
}

} // namespace Pelican
