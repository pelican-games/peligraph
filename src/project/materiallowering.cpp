#include "materiallowering.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace Pelican {
namespace {

std::size_t alignUp(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

std::pair<std::size_t, std::size_t> std140SizeAlignment(SurfaceParamType type) {
    switch (type) {
    case SurfaceParamType::floating:
    case SurfaceParamType::integer:
        return {4, 4};
    case SurfaceParamType::vec2:
        return {8, 8};
    case SurfaceParamType::vec3:
        return {12, 16};
    case SurfaceParamType::vec4:
    case SurfaceParamType::color:
        return {16, 16};
    }
    throw std::runtime_error("unknown surface parameter type while computing std140 layout");
}

std::size_t componentCount(SurfaceParamType type) {
    switch (type) {
    case SurfaceParamType::floating: return 1;
    case SurfaceParamType::vec2: return 2;
    case SurfaceParamType::vec3: return 3;
    case SurfaceParamType::vec4:
    case SurfaceParamType::color: return 4;
    case SurfaceParamType::integer: return 0;
    }
    return 0;
}

float checkedFloat(double value, std::string_view material_name, std::string_view param_name) {
    if (!std::isfinite(value) || value < -std::numeric_limits<float>::max() ||
        value > std::numeric_limits<float>::max()) {
        throw std::runtime_error("material '" + std::string{material_name} + "' value '" +
                                 std::string{param_name} + "' is not a finite float");
    }
    return static_cast<float>(value);
}

double srgbToLinear(double value) {
    const auto clamped = std::clamp(value, 0.0, 1.0);
    return clamped <= 0.04045 ? clamped / 12.92
                             : std::pow((clamped + 0.055) / 1.055, 2.4);
}

void writeValue(std::vector<std::byte> &bytes, const Std140MemberLayout &member,
                const SurfaceParamDefinition &definition, const SurfaceParamValue &authored,
                std::string_view material_name) {
    if (authored.type != definition.type) {
        throw std::runtime_error("material '" + std::string{material_name} + "' value '" +
                                 definition.name + "' does not match declared type " +
                                 std::string{surfaceParamTypeName(definition.type)});
    }
    const auto checkRange = [&](double value) {
        if ((definition.min && value < *definition.min) ||
            (definition.max && value > *definition.max)) {
            throw std::runtime_error("material '" + std::string{material_name} + "' value '" +
                                     definition.name + "' is outside its declared range");
        }
    };
    if (definition.type == SurfaceParamType::integer) {
        checkRange(static_cast<double>(authored.integer_value));
        if (authored.integer_value < std::numeric_limits<std::int32_t>::min() ||
            authored.integer_value > std::numeric_limits<std::int32_t>::max()) {
            throw std::runtime_error("material '" + std::string{material_name} + "' value '" +
                                     definition.name + "' is outside the shader int range");
        }
        const auto value = static_cast<std::int32_t>(authored.integer_value);
        std::memcpy(bytes.data() + member.offset, &value, sizeof(value));
        return;
    }

    const auto count = componentCount(definition.type);
    for (std::size_t i = 0; i < count; ++i) {
        auto value = authored.values[i];
        checkRange(value);
        if (definition.type == SurfaceParamType::color && definition.encoding == "srgb" && i < 3) {
            value = srgbToLinear(value);
        }
        const auto packed = checkedFloat(value, material_name, definition.name);
        std::memcpy(bytes.data() + member.offset + i * sizeof(float), &packed, sizeof(packed));
    }
}

MaterialDummyTexture inferDummy(std::string_view name) {
    std::string lower{name};
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (lower.find("normal") != std::string::npos) {
        return MaterialDummyTexture::flat_normal;
    }
    if (lower.find("emissive") != std::string::npos ||
        lower.find("black") != std::string::npos) {
        return MaterialDummyTexture::black;
    }
    return MaterialDummyTexture::white;
}

std::string_view blendName(SurfaceBlendMode mode) {
    switch (mode) {
    case SurfaceBlendMode::opaque: return "opaque";
    case SurfaceBlendMode::blend: return "blend";
    case SurfaceBlendMode::additive: return "additive";
    }
    return "unknown";
}

std::string_view cullName(SurfaceCullMode mode) {
    switch (mode) {
    case SurfaceCullMode::none: return "none";
    case SurfaceCullMode::front: return "front";
    case SurfaceCullMode::back: return "back";
    }
    return "unknown";
}

std::string_view depthCompareName(SurfaceDepthCompare compare) {
    switch (compare) {
    case SurfaceDepthCompare::never: return "never";
    case SurfaceDepthCompare::less: return "less";
    case SurfaceDepthCompare::equal: return "equal";
    case SurfaceDepthCompare::less_equal: return "less_equal";
    case SurfaceDepthCompare::greater: return "greater";
    case SurfaceDepthCompare::not_equal: return "not_equal";
    case SurfaceDepthCompare::greater_equal: return "greater_equal";
    case SurfaceDepthCompare::always: return "always";
    }
    return "unknown";
}

struct MaterialRouteDecision {
    MaterialRouteClass route;
    MaterialRouteReason reason;
};

bool isOpenPbrSurface(std::string_view reference) {
    constexpr std::array supported{
        std::string_view{"engine://surfaces/openpbr/opaque_single.surface"},
        std::string_view{"engine://surfaces/openpbr/opaque_double.surface"},
        std::string_view{"engine://surfaces/openpbr/mask_single.surface"},
        std::string_view{"engine://surfaces/openpbr/mask_double.surface"},
        std::string_view{"engine://surfaces/openpbr/blend_single.surface"},
        std::string_view{"engine://surfaces/openpbr/blend_double.surface"},
    };
    return std::find(supported.begin(), supported.end(), reference) != supported.end();
}

const SurfaceParamValue *effectiveValue(const MaterialDefinition &material,
                                        const SurfaceFormatDocument &surface,
                                        std::string_view name) {
    const auto override_value = std::find_if(
        material.values.begin(), material.values.end(), [&](const auto &value) {
            return value.name == name;
        });
    if (override_value != material.values.end()) return &override_value->value;
    const auto definition = std::find_if(
        surface.params.begin(), surface.params.end(), [&](const auto &param) {
            return param.name == name;
        });
    return definition == surface.params.end() ? nullptr : &definition->default_value;
}

bool scalarEquals(const MaterialDefinition &material,
                  const SurfaceFormatDocument &surface, std::string_view name,
                  double expected) {
    const auto *value = effectiveValue(material, surface, name);
    return value != nullptr && value->type == SurfaceParamType::floating &&
           std::abs(value->values[0] - expected) <= 1e-6;
}

bool colorRgbEquals(const MaterialDefinition &material,
                    const SurfaceFormatDocument &surface, std::string_view name,
                    const std::array<double, 3> &expected) {
    const auto *value = effectiveValue(material, surface, name);
    if (value == nullptr || value->type != SurfaceParamType::color) return false;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (std::abs(value->values[index] - expected[index]) > 1e-6) return false;
    }
    return true;
}

bool hasTextureOverride(const MaterialDefinition &material, std::string_view name) {
    return std::any_of(material.texture_overrides.begin(), material.texture_overrides.end(),
                       [&](const auto &texture) { return texture.name == name; });
}

DeferredEligibility openPbrEligibility(const MaterialDefinition &material,
                                       const SurfaceFormatDocument &surface) {
    const auto incompatible = [](std::string reason) {
        return DeferredEligibility{false, DeferredMaterialModel::openpbr_base_v1,
                                   std::move(reason)};
    };
    if (surface.render_state.blend != SurfaceBlendMode::opaque)
        return incompatible("blend_requires_forward");
    if (!surface.screen_inputs.empty()) return incompatible("screen_input_requires_forward");
    if (!scalarEquals(material, surface, "coat_weight", 0.0))
        return incompatible("coat_weight_nonzero");
    if (!scalarEquals(material, surface, "base_diffuse_roughness", 0.0))
        return incompatible("diffuse_roughness_nonzero");
    if (!scalarEquals(material, surface, "specular_weight", 1.0))
        return incompatible("custom_specular_weight");
    if (!colorRgbEquals(material, surface, "specular_color", {1.0, 1.0, 1.0}))
        return incompatible("custom_specular_color");
    if (!scalarEquals(material, surface, "specular_ior", 1.5))
        return incompatible("custom_specular_ior");
    for (const auto texture : {"base_diffuse_roughness_map", "specular_weight_map",
                               "specular_color_map", "specular_ior_map"}) {
        if (hasTextureOverride(material, texture))
            return incompatible("custom_" + std::string{texture});
    }
    return {true, DeferredMaterialModel::openpbr_base_v1, "compatible"};
}

MaterialRouteDecision automaticRoute(const SurfaceFormatDocument &surface,
                                     const DeferredEligibility &eligibility) {
    if (!surface.screen_inputs.empty()) {
        return {MaterialRouteClass::forward_transparent,
                MaterialRouteReason::automatic_screen_input};
    }
    if (surface.render_state.blend != SurfaceBlendMode::opaque) {
        return {MaterialRouteClass::forward_transparent,
                MaterialRouteReason::automatic_blended};
    }
    if (eligibility.compatible &&
        eligibility.model == DeferredMaterialModel::openpbr_base_v1) {
        return {MaterialRouteClass::deferred_geometry,
                MaterialRouteReason::automatic_openpbr_base};
    }
    if (surface.hooks.lighting_v1) {
        return {MaterialRouteClass::forward_opaque,
                MaterialRouteReason::automatic_custom_lighting};
    }
    if (surface.hooks.ambient_v1) {
        return {MaterialRouteClass::forward_opaque,
                MaterialRouteReason::automatic_custom_ambient};
    }
    if (surface.hooks.brdf_v1) {
        return {MaterialRouteClass::forward_opaque,
                MaterialRouteReason::automatic_custom_brdf};
    }
    return {MaterialRouteClass::deferred_geometry,
            MaterialRouteReason::automatic_deferred_compatible};
}

MaterialRouteDecision routeMaterial(const MaterialDefinition &material,
                                    const SurfaceFormatDocument &surface,
                                    const DeferredEligibility &eligibility) {
    if (material.render_path == MaterialRenderPath::automatic) {
        return automaticRoute(surface, eligibility);
    }
    if (material.render_path == MaterialRenderPath::forward) {
        const bool transparent = !surface.screen_inputs.empty() ||
                                 surface.render_state.blend != SurfaceBlendMode::opaque;
        return {transparent ? MaterialRouteClass::forward_transparent
                            : MaterialRouteClass::forward_opaque,
                MaterialRouteReason::explicit_forward};
    }
    if (!surface.screen_inputs.empty()) {
        throw std::runtime_error("material '" + material.name +
                                 "' render_path deferred is incompatible with screen_inputs");
    }
    if (surface.render_state.blend != SurfaceBlendMode::opaque) {
        throw std::runtime_error("material '" + material.name +
                                 "' render_path deferred is incompatible with blending");
    }
    if ((surface.hooks.brdf_v1 || surface.hooks.ambient_v1 ||
         surface.hooks.lighting_v1) &&
        !eligibility.compatible) {
        throw std::runtime_error("material '" + material.name +
                                 "' render_path deferred cannot preserve a custom BRDF/ambient/lighting hook");
    }
    return {MaterialRouteClass::deferred_geometry,
            MaterialRouteReason::explicit_deferred};
}

void validateVariantSurface(const MaterialDefinition &material,
                            const SurfaceFormatDocument &surface) {
    if (!material.routing) return;
    const auto expected = materialVariantRenderState(*material.routing);
    const auto &actual = surface.render_state;
    const auto mismatch = actual.blend != expected.blend || actual.cull != expected.cull ||
                          actual.depth_test != expected.depth_test ||
                          actual.depth_write != expected.depth_write ||
                          actual.depth_compare != expected.depth_compare;
    if (mismatch) {
        throw std::runtime_error("material '" + material.name + "' routing variant '" +
                                 std::string{materialVariantName(*material.routing)} +
                                 "' does not match surface pipeline state");
    }
    if (!surface.hooks.lighting_v1) {
        throw std::runtime_error("material '" + material.name + "' routing variant '" +
                                 std::string{materialVariantName(*material.routing)} +
                                 "' requires the pelican_lighting_v1 OpenPBR wrapper contract");
    }
    if (material.routing->alpha_mode == MaterialAlphaMode::mask) {
        const auto cutoff = std::find_if(surface.params.begin(), surface.params.end(),
                                         [](const SurfaceParamDefinition &param) {
                                             return param.name == "alpha_cutoff";
                                         });
        if (cutoff == surface.params.end() ||
            cutoff->type != SurfaceParamType::floating) {
            throw std::runtime_error("material '" + material.name + "' routing variant '" +
                                     std::string{materialVariantName(*material.routing)} +
                                     "' requires float surface param 'alpha_cutoff'");
        }
    }
}

} // namespace

DeferredEligibility evaluateDeferredEligibility(
    const MaterialDefinition &material,
    const SurfaceFormatDocument &surface) {
    if (material.surface && isOpenPbrSurface(*material.surface)) {
        return openPbrEligibility(material, surface);
    }
    if (!surface.screen_inputs.empty())
        return {false, DeferredMaterialModel::standard_pbr_v1,
                "screen_input_requires_forward"};
    if (surface.render_state.blend != SurfaceBlendMode::opaque)
        return {false, DeferredMaterialModel::standard_pbr_v1,
                "blend_requires_forward"};
    if (surface.hooks.lighting_v1)
        return {false, DeferredMaterialModel::standard_pbr_v1,
                "custom_lighting"};
    if (surface.hooks.ambient_v1)
        return {false, DeferredMaterialModel::standard_pbr_v1, "custom_ambient"};
    if (surface.hooks.brdf_v1)
        return {false, DeferredMaterialModel::standard_pbr_v1, "custom_brdf"};
    return {true, DeferredMaterialModel::standard_pbr_v1, "compatible"};
}

Std140Layout makeSurfaceStd140Layout(const SurfaceFormatDocument &surface) {
    Std140Layout layout;
    layout.members.reserve(surface.params.size());
    std::size_t offset = 0;
    for (const auto &param : surface.params) {
        const auto [size, alignment] = std140SizeAlignment(param.type);
        offset = alignUp(offset, alignment);
        layout.members.push_back(Std140MemberLayout{param.name, param.type, offset, size, alignment});
        offset += size;
    }
    layout.size = alignUp(offset, layout.alignment);
    if (layout.size > materialCustomValueCapacity) {
        const auto tail = surface.params.empty() ? std::string{"<none>"}
                                                 : surface.params.back().name;
        throw std::runtime_error("surface param '" + tail + "' makes values std140 size " +
                                 std::to_string(layout.size) +
                                 ", exceeding MaterialBuffer capacity " +
                                 std::to_string(materialCustomValueCapacity));
    }
    return layout;
}

std::vector<std::byte> bindSurfaceValues(const SurfaceFormatDocument &surface,
                                         std::span<const MaterialValue> overrides,
                                         std::string_view material_name) {
    const auto layout = makeSurfaceStd140Layout(surface);
    std::vector<std::byte> bytes(layout.size, std::byte{0});
    std::unordered_map<std::string_view, const SurfaceParamValue *> by_name;
    by_name.reserve(overrides.size());
    for (const auto &override_value : overrides) {
        if (!by_name.emplace(override_value.name, &override_value.value).second) {
            throw std::runtime_error("material '" + std::string{material_name} +
                                     "' has duplicate value '" + override_value.name + "'");
        }
    }

    for (std::size_t i = 0; i < surface.params.size(); ++i) {
        const auto &definition = surface.params[i];
        const auto found = by_name.find(definition.name);
        const auto &value = found == by_name.end() ? definition.default_value : *found->second;
        writeValue(bytes, layout.members[i], definition, value, material_name);
        if (found != by_name.end()) {
            by_name.erase(found);
        }
    }
    if (!by_name.empty()) {
        throw std::runtime_error("material '" + std::string{material_name} + "' value '" +
                                 std::string{by_name.begin()->first} +
                                 "' is not declared by its surface");
    }
    return bytes;
}

void validateSurfaceCapabilities(const SurfaceFormatDocument &surface,
                                 const MaterialLoweringCapabilities &capabilities,
                                 std::string_view material_name) {
    const auto context = "material '" + std::string{material_name} + "'";
    if (surface.textures.size() > capabilities.max_custom_textures) {
        throw std::runtime_error(context + " declares " + std::to_string(surface.textures.size()) +
                                 " custom textures but device limit is " +
                                 std::to_string(capabilities.max_custom_textures));
    }
    if (surface.render_state.blend != SurfaceBlendMode::opaque &&
        !capabilities.color_attachment_blend) {
        throw std::runtime_error(context + " render_state blend '" +
                                 std::string{blendName(surface.render_state.blend)} +
                                 "' requires color attachment blend capability");
    }
    if ((surface.render_state.depth_test || surface.render_state.depth_write) &&
        !capabilities.depth_attachment) {
        throw std::runtime_error(context +
                                 " render_state depth requires depth attachment capability");
    }
    if (!surface.render_state.depth_test && surface.render_state.depth_write) {
        throw std::runtime_error(context +
                                 " render_state requests depth_write while depth_test is disabled");
    }
}

LoweredMaterial lowerMaterial(const MaterialDefinition &material,
                              const SurfaceFormatDocument &surface,
                              const MaterialLoweringCapabilities &capabilities) {
    validateSurfaceCapabilities(surface, capabilities, material.name);
    validateVariantSurface(material, surface);
    LoweredMaterial lowered;
    lowered.name = material.name;
    lowered.tags = canonicalizeMaterialDrawTags(
        material.tags,
        "material '" + material.name + "'");
    lowered.surface = material.surface.value_or("<surface>");
    lowered.defines = material.defines;
    lowered.values_layout = makeSurfaceStd140Layout(surface);
    lowered.values = bindSurfaceValues(surface, material.values, material.name);
    lowered.render_state = surface.render_state;
    lowered.hooks = surface.hooks;
    lowered.routing = material.routing;
    lowered.screen_inputs = surface.screen_inputs;
    lowered.resource_ports = surface.resource_ports;
    if (!surface.screen_inputs.empty()) {
        const auto logical_types = makeBuiltinLogicalTypeRegistry();
        const auto logical_conversions =
            makeBuiltinLogicalTypeConversionRegistry(logical_types);
        lowered.screen_input_contracts.reserve(surface.screen_inputs.size());
        for (const auto &input : surface.screen_inputs) {
            auto contract =
                makeBuiltinMaterialScreenInputContract(logical_types, input);
            const auto source_type = contract.source_type;
            lowered.screen_input_contracts.push_back(
                resolveMaterialScreenInputContract(
                    logical_types, logical_conversions, std::move(contract),
                    source_type)
                    .contract);
        }
    }
    lowered.deferred_eligibility = evaluateDeferredEligibility(material, surface);
    const auto route = routeMaterial(material, surface, lowered.deferred_eligibility);
    lowered.route = route.route;
    lowered.route_reason = route.reason;
    lowered.exact_pass = material.exact_pass;
    lowered.target_pass = materialRouteClassName(route.route);
    if (lowered.route == MaterialRouteClass::deferred_geometry &&
        lowered.deferred_eligibility.model == DeferredMaterialModel::openpbr_base_v1) {
        if (std::find(lowered.defines.begin(), lowered.defines.end(),
                      "PELICAN_GBUFFER_MODEL_OPENPBR_BASE_V1") == lowered.defines.end()) {
            lowered.defines.emplace_back("PELICAN_GBUFFER_MODEL_OPENPBR_BASE_V1");
        }
    }
    std::unordered_map<std::string_view, std::string_view> texture_overrides;
    texture_overrides.reserve(material.texture_overrides.size());
    for (const auto &override_value : material.texture_overrides) {
        if (!texture_overrides.emplace(override_value.name, override_value.reference).second) {
            throw std::runtime_error("material '" + material.name +
                                     "' has duplicate texture override '" +
                                     override_value.name + "'");
        }
    }

    lowered.textures.reserve(surface.textures.size());
    for (std::size_t i = 0; i < surface.textures.size(); ++i) {
        const auto &texture = surface.textures[i];
        const auto override_value = texture_overrides.find(texture.name);
        const auto reference = override_value == texture_overrides.end()
                                   ? std::string_view{texture.default_reference}
                                   : override_value->second;
        lowered.textures.push_back(LoweredTextureBinding{
            texture.name,
            materialCustomTextureFirstBinding + static_cast<std::uint32_t>(i),
            std::string{reference},
            texture.role,
            texture.role == SurfaceTextureRole::color ? LoweredTextureView::srgb
                                                       : LoweredTextureView::unorm,
            inferDummy(texture.name),
        });
        if (override_value != texture_overrides.end()) {
            texture_overrides.erase(override_value);
        }
    }
    if (!texture_overrides.empty()) {
        throw std::runtime_error("material '" + material.name + "' texture override '" +
                                 std::string{texture_overrides.begin()->first} +
                                 "' is not declared by its surface");
    }
    return lowered;
}

LoweredNamedMaterialVariant lowerMaterialVariant(
    const MaterialDefinition &base,
    const MaterialNamedVariantDefinition &variant,
    const SurfaceFormatDocument &surface,
    const MaterialLoweringCapabilities &capabilities) {
    MaterialDefinition definition;
    definition.name = base.name + "#" + variant.name;
    definition.base = base.base;
    definition.defines = variant.defines;
    definition.surface = variant.surface;
    definition.values = variant.values;
    definition.texture_overrides = variant.texture_overrides;
    definition.render_path = variant.render_path;
    return LoweredNamedMaterialVariant{
        variant.name,
        lowerMaterial(definition, surface, capabilities),
    };
}

std::vector<LoweredNamedMaterialVariant> lowerMaterialVariants(
    const MaterialDefinition &base,
    const MaterialSurfaceCatalog &surfaces,
    const MaterialLoweringCapabilities &capabilities) {
    std::vector<LoweredNamedMaterialVariant> lowered;
    lowered.reserve(base.variants.size());
    for (const auto &variant : base.variants) {
        const auto surface = surfaces.find(variant.surface);
        if (surface == surfaces.end()) {
            throw std::runtime_error(
                "material '" + base.name + "' variant '" + variant.name +
                "' surface '" + variant.surface + "' was not provided");
        }
        lowered.push_back(
            lowerMaterialVariant(base, variant, surface->second, capabilities));
    }
    return lowered;
}

LoweredMaterial lowerMaterialWithSnapshots(
    const MaterialDefinition &material,
    const SurfaceFormatDocument &surface,
    std::span<const std::string> available_snapshots,
    const MaterialLoweringCapabilities &capabilities) {
    for (const auto &input : surface.screen_inputs) {
        if (std::find(available_snapshots.begin(), available_snapshots.end(), input) ==
            available_snapshots.end()) {
            throw std::runtime_error("material '" + material.name + "' surface '" +
                                     material.surface.value_or("<surface>") +
                                     "' references undefined screen snapshot '" + input + "'");
        }
    }
    return lowerMaterial(material, surface, capabilities);
}

LoweredMaterial lowerSurfaceDefaults(const SurfaceFormatDocument &surface,
                                     std::string_view surface_name,
                                     const MaterialLoweringCapabilities &capabilities) {
    MaterialDefinition material;
    material.name = "<surface-defaults>";
    material.surface = std::string{surface_name};
    return lowerMaterial(material, surface, capabilities);
}

std::string dumpLoweredMaterial(const LoweredMaterial &material) {
    std::ostringstream out;
    out << "material: " << material.name << '\n';
    out << "surface: " << material.surface << '\n';
    if (!material.tags.empty()) {
        out << "tags:\n";
        for (const auto &tag : material.tags)
            out << "  - " << tag << '\n';
    }
    out << "layer: C-material (lowered from B surface source)\n";
    out << "hooks:";
    const auto hooks = surfaceHookNames(material.hooks);
    if (hooks.empty()) out << " []\n";
    else {
        out << '\n';
        for (const auto hook : hooks) out << "  - " << hook << '\n';
    }
    out << "shader_source:\n";
    out << "  vertex: engine://shaders/material/surface_v1.vert\n";
    out << "  fragment: engine://shaders/material/surface_v1.frag\n";
    out << "variants:\n";
    out << "  main: []\n";
    out << "  depth: [PELICAN_PASS_DEPTH]\n";
    out << "  velocity: [PELICAN_PASS_VELOCITY]\n";
    out << "public_libraries:\n";
    out << "  - engine://shaders/include/pelican_surface_v1.glsl\n";
    out << "  - engine://shaders/include/pelican_lighting_v1.glsl\n";
    out << "defines:";
    if (material.defines.empty()) out << " []\n";
    else {
        out << '\n';
        for (const auto &define : material.defines) out << "  - " << define << '\n';
    }
    out << "values: std140 size=" << material.values_layout.size << '\n';
    for (const auto &member : material.values_layout.members) {
        out << "  " << member.name << " type=" << surfaceParamTypeName(member.type)
            << " offset=" << member.offset << " size=" << member.size << '\n';
    }
    out << "bindings:\n";
    out << "  set=2 binding=6 type=storage_buffer name=MaterialBuffer\n";
    for (const auto &texture : material.textures) {
        out << "  set=2 binding=" << texture.binding << " type=combined_image_sampler name="
            << texture.name << " view="
            << (texture.view == LoweredTextureView::srgb ? "SRGB" : "UNORM")
            << " default=" << texture.reference << '\n';
    }
    for (std::size_t i = 0; i < material.screen_inputs.size(); ++i) {
        out << "  set=1 binding=" << i << " type=combined_image_sampler name="
            << material.screen_inputs[i] << " accessor=pelican_screen_"
            << material.screen_inputs[i] << "\n";
    }
    out << "screen_inputs:";
    if (material.screen_inputs.empty()) out << " []\n";
    else {
        out << '\n';
        for (const auto &input : material.screen_inputs) out << "  - " << input << '\n';
    }
    if (!material.resource_ports.empty()) {
        out << "resource_ports:\n";
        for (const auto &port : material.resource_ports) {
            out << "  - name=" << port.name
                << " kind="
                << surfaceResourcePortKindName(port.kind)
                << " stage="
                << surfaceResourcePortStageName(port.stage);
            if (port.kind ==
                SurfaceResourcePortKind::buffer) {
                out << " element="
                    << shaderResourceBufferElementName(
                           port.element);
            }
            out << '\n';
        }
    }
    out << "target_pass: " << material.target_pass << '\n';
    out << "route: " << materialRouteClassName(material.route)
        << " reason=" << materialRouteReasonName(material.route_reason) << '\n';
    out << "deferred_eligibility: "
        << (material.deferred_eligibility.compatible ? "compatible" : "incompatible")
        << " model=" << deferredMaterialModelName(material.deferred_eligibility.model)
        << " reason=" << material.deferred_eligibility.reason << '\n';
    if (material.exact_pass) out << "pass: " << *material.exact_pass << '\n';
    out << "render_state: blend=" << blendName(material.render_state.blend)
        << " cull=" << cullName(material.render_state.cull)
        << " depth_test=" << (material.render_state.depth_test ? "true" : "false")
        << " depth_write=" << (material.render_state.depth_write ? "true" : "false")
        << " depth_compare=" << depthCompareName(material.render_state.depth_compare) << '\n';
    if (material.routing) {
        out << "routing: alpha_mode=" << materialAlphaModeName(material.routing->alpha_mode)
            << " double_sided=" << (material.routing->double_sided ? "true" : "false")
            << " variant=" << materialVariantName(*material.routing);
        if (material.routing->alpha_mode == MaterialAlphaMode::mask) {
            out << " discard=alpha<alpha_cutoff";
        }
        out << '\n';
    }
    return out.str();
}

} // namespace Pelican
