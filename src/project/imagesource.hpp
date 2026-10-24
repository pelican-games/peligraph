#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace Pelican {

// EN-2: a render.image read from a project image file, as a render target
// declares it (`source`). The image is read only: its pixels are uploaded
// whenever the target's image is created and it is never cleared, aliased
// or written by a pass. RC-3c1a: the one declaration and the one place its
// defaults live; core's RenderTargetSource is this type.
struct ImageSourceDeclaration {
    std::string asset;
    // "srgb": the image has an sRGB view and shaders read linear values.
    // "linear": UNORM, read as stored.
    std::string color_space = "srgb";
    // "straight": the file's values; "premultiply": RGB times alpha, in
    // linear space, when the file is read.
    std::string alpha = "straight";
    // "none" or "full" (every mip generated at upload).
    std::string mips = "none";

    bool operator==(const ImageSourceDeclaration &) const = default;
};

// The lowered form of a render.image's file source on a render target
// (`target_json`), nullopt without `source`. The graph module checked the
// authored values; the lowering wrote the extent, format and usage the file
// decides, so only the source's own settings are read here. Throws
// "Render target source ...: <name>" for an empty asset, a value outside its
// choices, a target without width and height, or one that keeps history.
std::optional<ImageSourceDeclaration> parseImageSourceDeclaration(
    const nlohmann::json &target_json, const std::string &name);

} // namespace Pelican
