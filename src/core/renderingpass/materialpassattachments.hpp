#pragma once

#include "../../project/materialoutput.hpp"
#include "../../project/renderpipeline.hpp"

#include <array>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

// Compatibility defaults used only when a material pass omits the
// pelican.material_outputs schema. Explicit schemas carry an arbitrary ordered
// attachment list and are limited only by the selected Vulkan device.
inline constexpr std::array<vk::Format, 5> materialPassColorAttachmentFormatsSdr = {
    vk::Format::eB8G8R8A8Srgb,
    vk::Format::eR16G16B16A16Sfloat,
    vk::Format::eR8G8B8A8Unorm,
    vk::Format::eR16G16B16A16Sfloat,
    vk::Format::eB8G8R8A8Srgb,
};

inline constexpr std::array<vk::Format, 5> materialPassColorAttachmentFormatsHdr = {
    vk::Format::eR16G16B16A16Sfloat,
    vk::Format::eR16G16B16A16Sfloat,
    vk::Format::eR8G8B8A8Unorm,
    vk::Format::eR16G16B16A16Sfloat,
    vk::Format::eR16G16B16A16Sfloat,
};

inline constexpr const std::array<vk::Format, 5> &materialPassColorAttachmentFormats(bool hdr) {
    return hdr ? materialPassColorAttachmentFormatsHdr : materialPassColorAttachmentFormatsSdr;
}

// Routed forward materials are composed with deferred lighting before the
// single presentation tone curve.  Keep this ABI scene-linear and capable of
// preserving values above 1.0 regardless of whether the display HDR feature
// is enabled.
inline constexpr vk::Format forwardMaterialPassColorAttachmentFormat =
    vk::Format::eR16G16B16A16Sfloat;

inline constexpr vk::Format materialPassDepthAttachmentFormat = vk::Format::eD32Sfloat;

// EL-4: the color formats a material pass without material_outputs takes,
// one per socket in location order. A forward contract takes one scene
// color; a G-buffer one the SDR or the HDR table, chosen by its first
// socket's format (materialPassUsesHdrColorFormats).
// validateMaterialPassAttachments checks against these, and
// get_graph_catalog lists them.
inline constexpr bool materialPassUsesHdrColorFormats(vk::Format first_color) {
    return first_color == vk::Format::eR16G16B16A16Sfloat;
}

inline std::vector<vk::Format> materialPassColorAttachmentFormatsFor(MaterialShaderContract shader_contract,
                                                                     bool hdr) {
    if (shader_contract == MaterialShaderContract::forward_scene_color_v1)
        return {forwardMaterialPassColorAttachmentFormat};
    const auto &formats = materialPassColorAttachmentFormats(hdr);
    return {formats.begin(), formats.end()};
}

// EL-4: whether a pass of the contract may declare material_outputs (the
// legacy G-buffer keeps its fixed five attachments).
inline constexpr bool materialPassContractAcceptsMaterialOutputs(MaterialPassContract contract) {
    return contract != MaterialPassContract::legacy_gbuffer_v1;
}

// EL-4: the numeric class a color target's format has where a
// material_outputs output is written to it; the output's type must be of
// the same class.
inline MaterialOutputNumericClass materialPassFormatNumericClass(vk::Format format) {
    const auto name = vk::to_string(format);
    if (name.ends_with("Sint")) return MaterialOutputNumericClass::signed_integer;
    if (name.ends_with("Uint")) return MaterialOutputNumericClass::unsigned_integer;
    return MaterialOutputNumericClass::floating;
}

} // namespace Pelican
