#pragma once

// RC-3c2a: the formats a material pass's attachments take, in the engine's
// own vocabulary (core's materialpassattachments.hpp maps them to Vulkan).
// The resource checks of a material pass (generationresources.hpp) and the
// material pipelines read the one table.

#include "imageformat.hpp"
#include "materialoutput.hpp"
#include "renderpipeline.hpp"

#include <array>
#include <vector>

namespace Pelican {

// Compatibility defaults used only when a material pass omits the
// pelican.material_outputs schema. Explicit schemas carry an arbitrary ordered
// attachment list and are limited only by the selected device.
inline constexpr std::array<ImageFormat, 5> materialPassColorImageFormatsSdr = {
    ImageFormat::b8g8r8a8_srgb,
    ImageFormat::r16g16b16a16_sfloat,
    ImageFormat::r8g8b8a8_unorm,
    ImageFormat::r16g16b16a16_sfloat,
    ImageFormat::b8g8r8a8_srgb,
};

inline constexpr std::array<ImageFormat, 5> materialPassColorImageFormatsHdr = {
    ImageFormat::r16g16b16a16_sfloat,
    ImageFormat::r16g16b16a16_sfloat,
    ImageFormat::r8g8b8a8_unorm,
    ImageFormat::r16g16b16a16_sfloat,
    ImageFormat::r16g16b16a16_sfloat,
};

// Routed forward materials are composed with deferred lighting before the
// single presentation tone curve.  Keep this ABI scene-linear and capable of
// preserving values above 1.0 regardless of whether the display HDR feature
// is enabled.
inline constexpr ImageFormat forwardMaterialPassColorImageFormat = ImageFormat::r16g16b16a16_sfloat;

inline constexpr ImageFormat materialPassDepthImageFormat = ImageFormat::d32_sfloat;

// EL-4: the color formats a material pass without material_outputs takes,
// one per socket in location order. A forward contract takes one scene
// color; a G-buffer one the SDR or the HDR table, chosen by its first
// socket's format (materialPassUsesHdrColorImageFormats).
inline constexpr bool materialPassUsesHdrColorImageFormats(ImageFormat first_color) {
    return first_color == ImageFormat::r16g16b16a16_sfloat;
}

inline std::vector<ImageFormat> materialPassColorImageFormatsFor(MaterialShaderContract shader_contract,
                                                                 bool hdr) {
    if (shader_contract == MaterialShaderContract::forward_scene_color_v1)
        return {forwardMaterialPassColorImageFormat};
    const auto &formats = hdr ? materialPassColorImageFormatsHdr : materialPassColorImageFormatsSdr;
    return {formats.begin(), formats.end()};
}

// EL-4: whether a pass of the contract may declare material_outputs (the
// legacy G-buffer keeps its fixed five attachments).
inline constexpr bool materialPassContractAcceptsMaterialOutputs(MaterialPassContract contract) {
    return contract != MaterialPassContract::legacy_gbuffer_v1;
}

// EL-4: the numeric class a color target's format has where a
// material_outputs output is written to it; the output's type must be of
// the same class. RC-3a1: the format table's numeric type.
inline MaterialOutputNumericClass materialPassFormatNumericClass(ImageFormat format) {
    switch (imageFormatShaderNumeric(format)) {
    case ImageShaderNumeric::signed_integer: return MaterialOutputNumericClass::signed_integer;
    case ImageShaderNumeric::unsigned_integer: return MaterialOutputNumericClass::unsigned_integer;
    case ImageShaderNumeric::floating: break;
    }
    return MaterialOutputNumericClass::floating;
}

} // namespace Pelican
