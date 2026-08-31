#include "gltfmateriallowering.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <picosha2.h>
#include <ranges>
#include <sstream>
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

bool isAbsoluteOrNumericReference(std::string_view value) {
    if (value.empty()) return false;
    if (value.starts_with('/') || value.starts_with('\\') ||
        value.starts_with("file://") ||
        (value.size() >= 2 && value[1] == ':')) {
        return true;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c >= '0' && c <= '9';
    });
}

void requireAuthoredReference(std::string_view value) {
    const auto path = std::filesystem::path{std::string{value}};
    const auto has_parent_escape = std::ranges::any_of(
        path, [](const auto &component) { return component == ".."; });
    if (value.empty() || isAbsoluteOrNumericReference(value) ||
        has_parent_escape || value.starts_with("asset:") ||
        value.starts_with("document:")) {
        throw std::runtime_error(
            "pelican.normalized_alpha_identity_invalid@1: authored_reference must be the WP368a logical reference, not a numeric handle, canonical identity, parent escape, or absolute path");
    }
}

void appendFloatBits(std::ostringstream &encoded, float value) {
    encoded << std::hex << std::bit_cast<std::uint32_t>(value) << ':';
}

} // namespace

NormalizedAlphaIr normalizeGltfBaseColorAlpha(
    GltfBaseColorAlphaDescription description) {
    requireAuthoredReference(description.authored_reference);
    const auto finite = [&](float value, std::string_view field) {
        if (!std::isfinite(value)) {
            throw std::runtime_error(
                "pelican.normalized_alpha_invalid@1: " +
                std::string{field} + " must be finite");
        }
    };
    finite(description.cutoff, "cutoff");
    finite(description.base_factor_a, "base_factor_a");
    for (const auto value : description.uv_offset_scale)
        finite(value, "uv_offset_scale");
    finite(description.uv_rotation, "uv_rotation");

    NormalizedAlphaIr result;
    result.material_identity_ =
        "asset:" + description.authored_reference + "#" +
        std::to_string(description.material_index);
    if (description.texture_index) {
        result.texture_identity_ =
            "asset:" + description.authored_reference +
            "#texture:gltf-texture/" +
            std::to_string(*description.texture_index);
    }
    result.alpha_mode_ = description.alpha_mode;
    result.cutoff_ = description.cutoff;
    result.base_factor_a_ = description.base_factor_a;
    result.uv_offset_scale_ = description.uv_offset_scale;
    result.uv_rotation_ = description.uv_rotation;

    std::ostringstream encoded;
    encoded << result.sourceKind() << '\0'
            << result.material_identity_ << '\0'
            << result.texture_identity_ << '\0'
            << static_cast<std::uint32_t>(result.alpha_mode_) << ':';
    appendFloatBits(encoded, result.cutoff_);
    appendFloatBits(encoded, result.base_factor_a_);
    for (const auto value : result.uv_offset_scale_)
        appendFloatBits(encoded, value);
    appendFloatBits(encoded, result.uv_rotation_);
    const auto bytes = encoded.str();
    result.digest_sha256_ = picosha2::hash256_hex_string(
        bytes.begin(), bytes.end());
    return result;
}

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
