#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// RC-3a1: the image formats of render targets, in the engine's own terms.
// The author's spelling (R16G16B16A16_SFLOAT) is the name; the Vulkan value
// is core's to map (vulkanimagevocabulary). One row each in the table below.
enum class ImageFormat : std::uint8_t {
    r8_unorm,
    r8_snorm,
    r8_uint,
    r8_sint,
    r8g8_unorm,
    r8g8_snorm,
    r8g8_uint,
    r8g8_sint,
    r8g8b8a8_unorm,
    r8g8b8a8_snorm,
    r8g8b8a8_uint,
    r8g8b8a8_sint,
    r8g8b8a8_srgb,
    b8g8r8a8_unorm,
    b8g8r8a8_srgb,
    a2b10g10r10_unorm_pack32,
    a2r10g10b10_unorm_pack32,
    b10g11r11_ufloat_pack32,
    r16_unorm,
    r16_snorm,
    r16_uint,
    r16_sint,
    r16_sfloat,
    r16g16_unorm,
    r16g16_snorm,
    r16g16_uint,
    r16g16_sint,
    r16g16_sfloat,
    r16g16b16a16_unorm,
    r16g16b16a16_snorm,
    r16g16b16a16_uint,
    r16g16b16a16_sint,
    r16g16b16a16_sfloat,
    r32_uint,
    r32_sint,
    r32_sfloat,
    r32g32_uint,
    r32g32_sint,
    r32g32_sfloat,
    r32g32b32a32_uint,
    r32g32b32a32_sint,
    r32g32b32a32_sfloat,
    d16_unorm,
    d24_unorm_s8_uint,
    d32_sfloat,
    // Not authorable: the engine's own (a depth/stencil it may choose).
    d32_sfloat_s8_uint,
};

enum class ImageAspect : std::uint8_t {
    color,
    depth,
    stencil,
    depth_stencil,
};

// "color" / "depth" / "stencil" / "depth_stencil".
std::string_view imageAspectName(ImageAspect aspect);

// How one component is stored.
enum class ImageComponentStorage : std::uint8_t {
    unorm,
    snorm,
    uint,
    sint,
    sfloat,
    ufloat,
    srgb,
};

std::string_view imageComponentStorageName(ImageComponentStorage storage);

// What a shader reads or writes for a component: derived from its storage
// (imageShaderNumeric), not a column of the table.
enum class ImageShaderNumeric : std::uint8_t {
    floating,
    signed_integer,
    unsigned_integer,
};

ImageShaderNumeric imageShaderNumeric(ImageComponentStorage storage);

// One row of the table.
struct ImageFormatInfo {
    ImageFormat format;
    // The author's spelling, the one name of the format everywhere: render
    // target declarations, the physical plan and its dump, Studio.
    std::string_view name;
    // Whether a render target declaration may name it (the set the render
    // target parser has always accepted). The others are the engine's own.
    bool authorable = true;
    ImageAspect aspect = ImageAspect::color;
    // The components in Vulkan's order: a color format's channels, or a
    // depth/stencil format's depth component then its stencil component.
    std::uint8_t component_count = 0;
    // Every color component's storage, or the depth component's.
    ImageComponentStorage storage = ImageComponentStorage::unorm;
    // The stencil component's storage, for a format with one.
    std::optional<ImageComponentStorage> stencil_storage;
    // RC-1: the bytes one texel occupies for the memory estimate. Not
    // Vulkan's texel block size: D32_SFLOAT_S8_UINT counts the 8 bytes an
    // implementation stores, not the 5 of its block.
    std::uint32_t estimated_texel_bytes = 0;
};

// Every format, authorable ones first in their declaration order.
std::span<const ImageFormatInfo> imageFormatTable();
// The names a render target declaration may give, in table order (Studio's
// format choices).
std::vector<std::string> authoredImageFormatNames();
const ImageFormatInfo &imageFormatInfo(ImageFormat format);

std::string_view imageFormatName(ImageFormat format);
// Any format of the table by its name (an engine-written document may hold
// an engine-only one); nullopt for another name.
std::optional<ImageFormat> findImageFormat(std::string_view name);
// What a render target declaration may name: throws "Unknown format: <name>"
// for a name the table does not list as authorable.
ImageFormat parseAuthoredImageFormat(const std::string &name);
// A physical plan's resource format as its dump and fingerprint spell it:
// the name, "" for none (a buffer).
std::string imageFormatNameOrEmpty(const std::optional<ImageFormat> &format);

// RC-3a1 (D3): v0 wrote the physical plan's formats (in ejected packages:
// complete physical plans) as Vulkan's enumerant
// without its `e` ("R16G16B16A16Sfloat"). The format a name of that
// spelling meant; nullopt for any other name. The one reader of the old
// spelling.
std::optional<ImageFormat> imageFormatFromV0PhysicalPlanName(
    std::string_view name);
// A physical-plan format as a document holds it: the name, or v0's spelling
// of it (imageFormatFromV0PhysicalPlanName). Throws, naming `what`, for any
// other text.
ImageFormat parsePhysicalPlanImageFormat(std::string_view name,
                                         std::string_view what);

ImageAspect imageFormatAspect(ImageFormat format);
bool imageFormatHasDepth(ImageFormat format);
bool imageFormatHasStencil(ImageFormat format);
// The storage of component `component` (< component_count).
ImageComponentStorage imageFormatComponentStorage(ImageFormat format,
                                                  std::uint8_t component);
// What a shader sees of a color format's channels (a depth format: its
// depth component).
ImageShaderNumeric imageFormatShaderNumeric(ImageFormat format);
std::uint32_t imageFormatEstimatedTexelBytes(ImageFormat format);

// A format in a JSON document is its name (never the enum's number). A
// reader parses the name itself (parseAuthoredImageFormat,
// parsePhysicalPlanImageFormat), saying what it reads.
template <typename BasicJsonType>
void to_json(BasicJsonType &json, ImageFormat format) {
    json = std::string{imageFormatName(format)};
}
// A physical plan's absent format (a buffer's) is "" there, not null.
template <typename BasicJsonType>
void to_json(BasicJsonType &json, const std::optional<ImageFormat> &format) {
    json = imageFormatNameOrEmpty(format);
}
template <typename BasicJsonType>
void from_json(const BasicJsonType &json, ImageFormat &format) = delete;

} // namespace Pelican
