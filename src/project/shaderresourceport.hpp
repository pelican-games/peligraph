#pragma once

#include "imagesubresource.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

enum class ShaderResourcePortAccess : std::uint8_t {
    automatic,
    sampled,
    storage,
};

std::string_view shaderResourcePortAccessName(
    ShaderResourcePortAccess access);

enum class ShaderResourcePortView : std::uint8_t {
    shared_2d,
    per_view,
    family_array,
    cube,
};

std::string_view shaderResourcePortViewName(
    ShaderResourcePortView view);

enum class ShaderResourcePortFilter : std::uint8_t {
    linear,
    nearest,
};

enum class ShaderResourcePortAddressMode : std::uint8_t {
    repeat,
    mirrored_repeat,
    clamp_to_edge,
};

// Storage-buffer element types deliberately describe the shader-visible
// std430 array element rather than a Vulkan format. The producer and consumer
// can therefore share one generated accessor contract without exposing a raw
// descriptor declaration or binding number.
enum class ShaderResourceBufferElement : std::uint8_t {
    floating,
    vec2,
    vec3,
    vec4,
    integer,
    ivec2,
    ivec3,
    ivec4,
    unsigned_integer,
    uvec2,
    uvec3,
    uvec4,
    mat4,
};

std::string_view shaderResourceBufferElementName(
    ShaderResourceBufferElement element);

std::optional<ShaderResourceBufferElement>
shaderResourceBufferElementFromName(
    std::string_view name);

enum class ShaderResourcePortKind : std::uint8_t {
    automatic,
    image,
    buffer,
};

std::string_view shaderResourcePortKindName(
    ShaderResourcePortKind kind);

struct ShaderResourcePortSampling {
    ShaderResourcePortFilter filter =
        ShaderResourcePortFilter::linear;
    ShaderResourcePortAddressMode address_mode =
        ShaderResourcePortAddressMode::repeat;

    bool operator==(
        const ShaderResourcePortSampling &) const = default;
};

// resource_ports annotate resources already declared by reads/writes/input.
// They never create graph edges. A string entry is the compact
// {"resource": "..."} form; the object form selects access/view/sampling.
struct ShaderResourcePortDefinition {
    std::string name;
    std::string resource;
    // automatic preserves the established image/buffer inference. Typed
    // fullscreen/compute buffers opt in explicitly so an element ABI can be
    // generated without exposing a descriptor binding.
    ShaderResourcePortKind kind =
        ShaderResourcePortKind::automatic;
    std::optional<ShaderResourceBufferElement>
        buffer_element;
    ShaderResourcePortAccess access =
        ShaderResourcePortAccess::automatic;
    ShaderResourcePortView view =
        ShaderResourcePortView::shared_2d;
    ShaderResourcePortSampling sampling;
    // Omitted preserves the established mip-0/default-view behavior.
    std::optional<ImageSubresourceRange> subresource;

    bool operator==(
        const ShaderResourcePortDefinition &) const = default;
};

std::vector<ShaderResourcePortDefinition>
parseShaderResourcePortDefinitions(
    const nlohmann::json &owner,
    std::span<const std::string> reads,
    std::span<const std::string> writes,
    std::string_view context);

ShaderResourcePortAccess effectiveShaderResourcePortAccess(
    const ShaderResourcePortDefinition &port,
    bool resource_is_image, bool written);

std::string shaderResourcePortVariableName(
    std::string_view port_name);

} // namespace Pelican
