#include "imageformat.hpp"

#include <array>
#include <cctype>
#include <stdexcept>

namespace Pelican {

namespace {

// RC-3a1: the one table of image formats. Its rows are in the enum's order
// (imageFormatInfo indexes it), authorable formats first in the order the
// render target parser has always listed them (Studio's format choices).
// The aspect and the components' storage are Vulkan's
// (vk::componentName / vk::componentNumericFormat; a core test checks every
// row), the texel bytes RC-1's memory estimate.
constexpr std::array<ImageFormatInfo, 46> kImageFormats{{
    {ImageFormat::r8_unorm, "R8_UNORM", true, ImageAspect::color, 1,
     ImageComponentStorage::unorm, std::nullopt, 1},
    {ImageFormat::r8_snorm, "R8_SNORM", true, ImageAspect::color, 1,
     ImageComponentStorage::snorm, std::nullopt, 1},
    {ImageFormat::r8_uint, "R8_UINT", true, ImageAspect::color, 1,
     ImageComponentStorage::uint, std::nullopt, 1},
    {ImageFormat::r8_sint, "R8_SINT", true, ImageAspect::color, 1,
     ImageComponentStorage::sint, std::nullopt, 1},
    {ImageFormat::r8g8_unorm, "R8G8_UNORM", true, ImageAspect::color, 2,
     ImageComponentStorage::unorm, std::nullopt, 2},
    {ImageFormat::r8g8_snorm, "R8G8_SNORM", true, ImageAspect::color, 2,
     ImageComponentStorage::snorm, std::nullopt, 2},
    {ImageFormat::r8g8_uint, "R8G8_UINT", true, ImageAspect::color, 2,
     ImageComponentStorage::uint, std::nullopt, 2},
    {ImageFormat::r8g8_sint, "R8G8_SINT", true, ImageAspect::color, 2,
     ImageComponentStorage::sint, std::nullopt, 2},
    {ImageFormat::r8g8b8a8_unorm, "R8G8B8A8_UNORM", true, ImageAspect::color, 4,
     ImageComponentStorage::unorm, std::nullopt, 4},
    {ImageFormat::r8g8b8a8_snorm, "R8G8B8A8_SNORM", true, ImageAspect::color, 4,
     ImageComponentStorage::snorm, std::nullopt, 4},
    {ImageFormat::r8g8b8a8_uint, "R8G8B8A8_UINT", true, ImageAspect::color, 4,
     ImageComponentStorage::uint, std::nullopt, 4},
    {ImageFormat::r8g8b8a8_sint, "R8G8B8A8_SINT", true, ImageAspect::color, 4,
     ImageComponentStorage::sint, std::nullopt, 4},
    {ImageFormat::r8g8b8a8_srgb, "R8G8B8A8_SRGB", true, ImageAspect::color, 4,
     ImageComponentStorage::srgb, std::nullopt, 4},
    {ImageFormat::b8g8r8a8_unorm, "B8G8R8A8_UNORM", true, ImageAspect::color, 4,
     ImageComponentStorage::unorm, std::nullopt, 4},
    {ImageFormat::b8g8r8a8_srgb, "B8G8R8A8_SRGB", true, ImageAspect::color, 4,
     ImageComponentStorage::srgb, std::nullopt, 4},
    {ImageFormat::a2b10g10r10_unorm_pack32, "A2B10G10R10_UNORM_PACK32", true, ImageAspect::color, 4,
     ImageComponentStorage::unorm, std::nullopt, 4},
    {ImageFormat::a2r10g10b10_unorm_pack32, "A2R10G10B10_UNORM_PACK32", true, ImageAspect::color, 4,
     ImageComponentStorage::unorm, std::nullopt, 4},
    {ImageFormat::b10g11r11_ufloat_pack32, "B10G11R11_UFLOAT_PACK32", true, ImageAspect::color, 3,
     ImageComponentStorage::ufloat, std::nullopt, 4},
    {ImageFormat::r16_unorm, "R16_UNORM", true, ImageAspect::color, 1,
     ImageComponentStorage::unorm, std::nullopt, 2},
    {ImageFormat::r16_snorm, "R16_SNORM", true, ImageAspect::color, 1,
     ImageComponentStorage::snorm, std::nullopt, 2},
    {ImageFormat::r16_uint, "R16_UINT", true, ImageAspect::color, 1,
     ImageComponentStorage::uint, std::nullopt, 2},
    {ImageFormat::r16_sint, "R16_SINT", true, ImageAspect::color, 1,
     ImageComponentStorage::sint, std::nullopt, 2},
    {ImageFormat::r16_sfloat, "R16_SFLOAT", true, ImageAspect::color, 1,
     ImageComponentStorage::sfloat, std::nullopt, 2},
    {ImageFormat::r16g16_unorm, "R16G16_UNORM", true, ImageAspect::color, 2,
     ImageComponentStorage::unorm, std::nullopt, 4},
    {ImageFormat::r16g16_snorm, "R16G16_SNORM", true, ImageAspect::color, 2,
     ImageComponentStorage::snorm, std::nullopt, 4},
    {ImageFormat::r16g16_uint, "R16G16_UINT", true, ImageAspect::color, 2,
     ImageComponentStorage::uint, std::nullopt, 4},
    {ImageFormat::r16g16_sint, "R16G16_SINT", true, ImageAspect::color, 2,
     ImageComponentStorage::sint, std::nullopt, 4},
    {ImageFormat::r16g16_sfloat, "R16G16_SFLOAT", true, ImageAspect::color, 2,
     ImageComponentStorage::sfloat, std::nullopt, 4},
    {ImageFormat::r16g16b16a16_unorm, "R16G16B16A16_UNORM", true, ImageAspect::color, 4,
     ImageComponentStorage::unorm, std::nullopt, 8},
    {ImageFormat::r16g16b16a16_snorm, "R16G16B16A16_SNORM", true, ImageAspect::color, 4,
     ImageComponentStorage::snorm, std::nullopt, 8},
    {ImageFormat::r16g16b16a16_uint, "R16G16B16A16_UINT", true, ImageAspect::color, 4,
     ImageComponentStorage::uint, std::nullopt, 8},
    {ImageFormat::r16g16b16a16_sint, "R16G16B16A16_SINT", true, ImageAspect::color, 4,
     ImageComponentStorage::sint, std::nullopt, 8},
    {ImageFormat::r16g16b16a16_sfloat, "R16G16B16A16_SFLOAT", true, ImageAspect::color, 4,
     ImageComponentStorage::sfloat, std::nullopt, 8},
    {ImageFormat::r32_uint, "R32_UINT", true, ImageAspect::color, 1,
     ImageComponentStorage::uint, std::nullopt, 4},
    {ImageFormat::r32_sint, "R32_SINT", true, ImageAspect::color, 1,
     ImageComponentStorage::sint, std::nullopt, 4},
    {ImageFormat::r32_sfloat, "R32_SFLOAT", true, ImageAspect::color, 1,
     ImageComponentStorage::sfloat, std::nullopt, 4},
    {ImageFormat::r32g32_uint, "R32G32_UINT", true, ImageAspect::color, 2,
     ImageComponentStorage::uint, std::nullopt, 8},
    {ImageFormat::r32g32_sint, "R32G32_SINT", true, ImageAspect::color, 2,
     ImageComponentStorage::sint, std::nullopt, 8},
    {ImageFormat::r32g32_sfloat, "R32G32_SFLOAT", true, ImageAspect::color, 2,
     ImageComponentStorage::sfloat, std::nullopt, 8},
    {ImageFormat::r32g32b32a32_uint, "R32G32B32A32_UINT", true, ImageAspect::color, 4,
     ImageComponentStorage::uint, std::nullopt, 16},
    {ImageFormat::r32g32b32a32_sint, "R32G32B32A32_SINT", true, ImageAspect::color, 4,
     ImageComponentStorage::sint, std::nullopt, 16},
    {ImageFormat::r32g32b32a32_sfloat, "R32G32B32A32_SFLOAT", true, ImageAspect::color, 4,
     ImageComponentStorage::sfloat, std::nullopt, 16},
    {ImageFormat::d16_unorm, "D16_UNORM", true, ImageAspect::depth, 1,
     ImageComponentStorage::unorm, std::nullopt, 2},
    {ImageFormat::d24_unorm_s8_uint, "D24_UNORM_S8_UINT", true, ImageAspect::depth_stencil, 2,
     ImageComponentStorage::unorm, ImageComponentStorage::uint, 4},
    {ImageFormat::d32_sfloat, "D32_SFLOAT", true, ImageAspect::depth, 1,
     ImageComponentStorage::sfloat, std::nullopt, 4},
    {ImageFormat::d32_sfloat_s8_uint, "D32_SFLOAT_S8_UINT", false, ImageAspect::depth_stencil, 2,
     ImageComponentStorage::sfloat, ImageComponentStorage::uint, 8},
}};

constexpr bool tableFollowsEnum() {
    for (std::size_t index = 0; index < kImageFormats.size(); ++index) {
        if (static_cast<std::size_t>(kImageFormats[index].format) != index) return false;
    }
    return true;
}
static_assert(tableFollowsEnum(), "kImageFormats rows must follow ImageFormat's order");
static_assert(static_cast<std::size_t>(ImageFormat::d32_sfloat_s8_uint) + 1 ==
              kImageFormats.size());

} // namespace

std::string_view imageAspectName(ImageAspect aspect) {
    switch (aspect) {
    case ImageAspect::color: return "color";
    case ImageAspect::depth: return "depth";
    case ImageAspect::stencil: return "stencil";
    case ImageAspect::depth_stencil: return "depth_stencil";
    }
    throw std::runtime_error("unknown image aspect");
}

std::string_view imageComponentStorageName(ImageComponentStorage storage) {
    switch (storage) {
    case ImageComponentStorage::unorm: return "unorm";
    case ImageComponentStorage::snorm: return "snorm";
    case ImageComponentStorage::uint: return "uint";
    case ImageComponentStorage::sint: return "sint";
    case ImageComponentStorage::sfloat: return "sfloat";
    case ImageComponentStorage::ufloat: return "ufloat";
    case ImageComponentStorage::srgb: return "srgb";
    }
    throw std::runtime_error("unknown image component storage");
}

ImageShaderNumeric imageShaderNumeric(ImageComponentStorage storage) {
    switch (storage) {
    case ImageComponentStorage::uint: return ImageShaderNumeric::unsigned_integer;
    case ImageComponentStorage::sint: return ImageShaderNumeric::signed_integer;
    case ImageComponentStorage::unorm:
    case ImageComponentStorage::snorm:
    case ImageComponentStorage::sfloat:
    case ImageComponentStorage::ufloat:
    case ImageComponentStorage::srgb: return ImageShaderNumeric::floating;
    }
    throw std::runtime_error("unknown image component storage");
}

std::span<const ImageFormatInfo> imageFormatTable() {
    return kImageFormats;
}

std::vector<std::string> authoredImageFormatNames() {
    std::vector<std::string> names;
    for (const auto &row : kImageFormats) {
        if (row.authorable) names.emplace_back(row.name);
    }
    return names;
}

const ImageFormatInfo &imageFormatInfo(ImageFormat format) {
    const auto index = static_cast<std::size_t>(format);
    if (index >= kImageFormats.size()) throw std::out_of_range("unknown image format");
    return kImageFormats[index];
}

std::string_view imageFormatName(ImageFormat format) {
    return imageFormatInfo(format).name;
}

std::optional<ImageFormat> findImageFormat(std::string_view name) {
    for (const auto &row : kImageFormats) {
        if (row.name == name) return row.format;
    }
    return std::nullopt;
}

ImageFormat parseAuthoredImageFormat(const std::string &name) {
    const auto format = findImageFormat(name);
    if (!format || !imageFormatInfo(*format).authorable) {
        throw std::runtime_error("Unknown format: " + name);
    }
    return *format;
}

std::string imageFormatNameOrEmpty(const std::optional<ImageFormat> &format) {
    return format ? std::string{imageFormatName(*format)} : std::string{};
}

std::optional<ImageFormat> imageFormatFromV0PhysicalPlanName(std::string_view name) {
    // v0 spelled R16G16B16A16_SFLOAT as R16G16B16A16Sfloat: the first word
    // as it is, each later one with its first letter only in capitals.
    for (const auto &row : kImageFormats) {
        std::string v0;
        std::size_t word = 0;
        bool word_start = true;
        for (const char c : row.name) {
            if (c == '_') {
                ++word;
                word_start = true;
                continue;
            }
            v0 += word == 0 || word_start
                      ? c
                      : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            word_start = false;
        }
        if (v0 == name) return row.format;
    }
    return std::nullopt;
}

ImageFormat parsePhysicalPlanImageFormat(std::string_view name, std::string_view what) {
    if (const auto format = findImageFormat(name)) return *format;
    if (const auto format = imageFormatFromV0PhysicalPlanName(name)) return *format;
    throw std::runtime_error(std::string{what} + " names an unknown image format: " +
                             std::string{name});
}

ImageAspect imageFormatAspect(ImageFormat format) {
    return imageFormatInfo(format).aspect;
}

bool imageFormatHasDepth(ImageFormat format) {
    const auto aspect = imageFormatAspect(format);
    return aspect == ImageAspect::depth || aspect == ImageAspect::depth_stencil;
}

bool imageFormatHasStencil(ImageFormat format) {
    const auto aspect = imageFormatAspect(format);
    return aspect == ImageAspect::stencil || aspect == ImageAspect::depth_stencil;
}

ImageComponentStorage imageFormatComponentStorage(ImageFormat format,
                                                  std::uint8_t component) {
    const auto &row = imageFormatInfo(format);
    if (component >= row.component_count) {
        throw std::out_of_range("image format " + std::string{row.name} +
                                " has no component " + std::to_string(component));
    }
    // A depth/stencil format's stencil follows its depth.
    if (row.aspect == ImageAspect::depth_stencil && component == 1) return *row.stencil_storage;
    if (row.aspect == ImageAspect::stencil) return *row.stencil_storage;
    return row.storage;
}

ImageShaderNumeric imageFormatShaderNumeric(ImageFormat format) {
    return imageShaderNumeric(imageFormatInfo(format).storage);
}

std::uint32_t imageFormatEstimatedTexelBytes(ImageFormat format) {
    return imageFormatInfo(format).estimated_texel_bytes;
}

} // namespace Pelican
