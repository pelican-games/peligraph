#pragma once

#include "shaderreflection.hpp"
#include "../../project/shaderresourceport.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

enum class VulkanResourceViewLayout : std::uint8_t;

inline constexpr std::string_view
    shaderResourcePortIncludeName =
        "pelican_resource_ports.glsl";

enum class ShaderResourceDescriptorKind : std::uint8_t {
    combined_image_sampler,
    storage_image,
    storage_buffer,
};

enum class ShaderResourceConsumerView : std::uint8_t {
    compute_once,
    graphics_sequential,
    graphics_multiview,
};

ReflectedImageViewDimension resolveShaderResourceImageViewDimension(
    const ShaderResourcePortDefinition &port,
    VulkanResourceViewLayout physical_view,
    ShaderResourceConsumerView consumer);

struct ShaderResourceInterfaceBinding {
    ShaderResourcePortDefinition port;
    std::uint32_t binding = 0;
    ShaderResourceDescriptorKind descriptor =
        ShaderResourceDescriptorKind::combined_image_sampler;
    ReflectedImageViewDimension image_view_dimension =
        ReflectedImageViewDimension::two_d;
    vk::Format storage_format = vk::Format::eUndefined;
    ShaderResourceBufferElement buffer_element =
        ShaderResourceBufferElement::unsigned_integer;
    // Empty keeps the established fullscreen/compute behavior. Material
    // ports set this explicitly so a vertex-only contract cannot silently
    // migrate to fragment (or vice versa) during reload.
    vk::ShaderStageFlags expected_stages;
    bool readable = true;
    bool writable = false;

    bool operator==(
        const ShaderResourceInterfaceBinding &) const = default;
};

std::string generateShaderResourcePortInclude(
    std::span<const ShaderResourceInterfaceBinding> bindings);

std::pair<std::string, std::string>
makeShaderResourcePortVirtualInclude(
    std::span<const ShaderResourceInterfaceBinding> bindings);

// Project shaders using generated ports may also include the stable frame
// contract. Supplying the reserved engine include closure in memory keeps
// project files independent from the engine source-tree layout.
std::vector<std::pair<std::string, std::string>>
makeShaderResourcePortVirtualIncludes(
    std::span<const ShaderResourceInterfaceBinding> bindings);

void validateShaderResourceInterfaceReflection(
    std::span<const ShaderResourceInterfaceBinding> bindings,
    const ShaderReflection &reflection,
    std::uint32_t expected_set = 1);

} // namespace Pelican
