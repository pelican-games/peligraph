#pragma once

#include "shaderresourceport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

enum class SurfaceLanguage {
    glsl,
    hlsl,
    slang,
};

enum class SurfaceParamType {
    floating,
    vec2,
    vec3,
    vec4,
    integer,
    color,
};

struct SurfaceParamValue {
    SurfaceParamType type = SurfaceParamType::floating;
    std::array<double, 4> values{};
    std::int64_t integer_value = 0;
    std::uint8_t component_count = 1;
};

struct SurfaceParamDefinition {
    std::string name;
    SurfaceParamType type = SurfaceParamType::floating;
    SurfaceParamValue default_value;
    std::optional<double> min;
    std::optional<double> max;
    std::optional<std::string> hint;
    // Authored color factors are sRGB unless the declaration explicitly opts
    // into linear encoding. Non-color params always use linear numeric data.
    std::string encoding = "linear";
};

enum class SurfaceTextureRole {
    color,
    data,
};

enum class SurfaceTextureDimension : std::uint8_t {
    two_d,
    cube,
    two_d_array,
    three_d,
};

std::string_view surfaceTextureDimensionName(
    SurfaceTextureDimension dimension);

enum class SurfaceTextureFilter : std::uint8_t {
    nearest,
    linear,
};

enum class SurfaceTextureAddressMode : std::uint8_t {
    repeat,
    mirrored_repeat,
    clamp_to_edge,
};

enum class SurfaceTextureCompare : std::uint8_t {
    none,
    never,
    less,
    equal,
    less_equal,
    greater,
    not_equal,
    greater_equal,
    always,
};

enum class SurfaceTextureAnisotropyFallback : std::uint8_t {
    disable,
    reject,
};

struct SurfaceTextureSampler {
    SurfaceTextureFilter filter = SurfaceTextureFilter::linear;
    SurfaceTextureFilter mip_filter = SurfaceTextureFilter::linear;
    SurfaceTextureAddressMode address =
        SurfaceTextureAddressMode::repeat;
    SurfaceTextureCompare compare =
        SurfaceTextureCompare::none;
    float anisotropy = 1.0f;
    SurfaceTextureAnisotropyFallback
        anisotropy_fallback =
            SurfaceTextureAnisotropyFallback::disable;

    bool operator==(const SurfaceTextureSampler &) const =
        default;
};

struct SurfaceTextureDefinition {
    std::string name;
    std::string default_reference;
    std::string color_space;
    SurfaceTextureRole role = SurfaceTextureRole::data;
    SurfaceTextureDimension dimension =
        SurfaceTextureDimension::two_d;
    SurfaceTextureSampler sampler;
};

enum class SurfaceResourcePortKind : std::uint8_t {
    image,
    buffer,
};

std::string_view surfaceResourcePortKindName(
    SurfaceResourcePortKind kind);

enum class SurfaceResourcePortStage : std::uint8_t {
    vertex,
    fragment,
    vertex_fragment,
};

std::string_view surfaceResourcePortStageName(
    SurfaceResourcePortStage stage);

// The reusable surface declares only its shader-facing interface. A material
// pass maps this semantic name to a graph image/buffer and owns footprint,
// history, view, and sampling policy.
struct SurfaceResourcePortDefinition {
    std::string name;
    SurfaceResourcePortKind kind =
        SurfaceResourcePortKind::image;
    SurfaceResourcePortStage stage =
        SurfaceResourcePortStage::fragment;
    ShaderResourceBufferElement element =
        ShaderResourceBufferElement::unsigned_integer;

    bool operator==(
        const SurfaceResourcePortDefinition &) const = default;
};

enum class SurfaceBlendMode {
    opaque,
    blend,
    additive,
};

enum class SurfaceCullMode {
    none,
    front,
    back,
};

enum class SurfaceDepthCompare {
    never,
    less,
    equal,
    less_equal,
    greater,
    not_equal,
    greater_equal,
    always,
};

struct SurfaceHookSet {
    bool vertex_displace_v1 = false;
    bool surface_v1 = false;
    bool brdf_v1 = false;
    bool ambient_v1 = false;
    bool lighting_v1 = false;
};

struct SurfaceRenderState {
    SurfaceBlendMode blend = SurfaceBlendMode::opaque;
    SurfaceCullMode cull = SurfaceCullMode::back;
    bool depth_test = true;
    bool depth_write = true;
    SurfaceDepthCompare depth_compare = SurfaceDepthCompare::less;
};

struct SurfaceFormatDocument {
    SurfaceLanguage language = SurfaceLanguage::glsl;
    std::vector<SurfaceParamDefinition> params;
    std::vector<SurfaceTextureDefinition> textures;
    std::vector<std::string> screen_inputs;
    std::vector<SurfaceResourcePortDefinition> resource_ports;
    SurfaceRenderState render_state;
    std::size_t code_offset = 0;
    std::size_t code_line = 1;
    std::string code;
    SurfaceHookSet hooks;
    std::vector<std::string> warnings;
};

SurfaceFormatDocument parseSurfaceFormat(std::string_view source,
                                         std::string_view source_name = "<surface>");

std::string_view surfaceParamTypeName(SurfaceParamType type);

std::vector<std::string_view> surfaceHookNames(const SurfaceHookSet &hooks);

} // namespace Pelican
