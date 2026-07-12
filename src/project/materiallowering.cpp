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
    if (definition.type == SurfaceParamType::integer) {
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

std::string routeMaterial(const SurfaceFormatDocument &surface) {
    if (!surface.screen_inputs.empty() || surface.render_state.blend != SurfaceBlendMode::opaque) {
        return "forward_transparent";
    }
    if (surface.hooks.brdf_v1 || surface.hooks.lighting_v1) {
        return "forward_opaque";
    }
    return "deferred_geometry";
}

} // namespace

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
    LoweredMaterial lowered;
    lowered.name = material.name;
    lowered.surface = material.surface.value_or("<surface>");
    lowered.defines = material.defines;
    lowered.values_layout = makeSurfaceStd140Layout(surface);
    lowered.values = bindSurfaceValues(surface, material.values, material.name);
    lowered.render_state = surface.render_state;
    lowered.hooks = surface.hooks;
    lowered.screen_inputs = surface.screen_inputs;
    lowered.target_pass = routeMaterial(surface);
    lowered.textures.reserve(surface.textures.size());
    for (std::size_t i = 0; i < surface.textures.size(); ++i) {
        const auto &texture = surface.textures[i];
        lowered.textures.push_back(LoweredTextureBinding{
            texture.name,
            materialCustomTextureFirstBinding + static_cast<std::uint32_t>(i),
            texture.default_reference,
            texture.role,
            texture.role == SurfaceTextureRole::color ? LoweredTextureView::srgb
                                                       : LoweredTextureView::unorm,
            inferDummy(texture.name),
        });
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
    out << "target_pass: " << material.target_pass << '\n';
    out << "render_state: blend=" << blendName(material.render_state.blend)
        << " cull=" << cullName(material.render_state.cull)
        << " depth_test=" << (material.render_state.depth_test ? "true" : "false")
        << " depth_write=" << (material.render_state.depth_write ? "true" : "false")
        << " depth_compare=" << depthCompareName(material.render_state.depth_compare) << '\n';
    return out.str();
}

} // namespace Pelican
