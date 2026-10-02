#include "graphimagesource.hpp"

#include "../loader/imageloader.hpp"
#include "../loader/pathresolver.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {

using Json = nlohmann::json;

std::string sourceKey(std::string_view asset, std::string_view color_space, std::string_view alpha) {
    std::string key{asset};
    key += '\n';
    key += color_space;
    key += '\n';
    key += alpha;
    return key;
}

std::string_view pixelFormatName(ImagePixelFormat format) {
    switch (format) {
    case ImagePixelFormat::Rgba8Unorm: return "rgba8_unorm";
    case ImagePixelFormat::Rgba8Srgb: return "rgba8_srgb";
    case ImagePixelFormat::Rgba16Sfloat: return "rgba16_sfloat";
    case ImagePixelFormat::Rgba32Sfloat: return "rgba32_sfloat";
    case ImagePixelFormat::Bc5Unorm: return "bc5_unorm";
    case ImagePixelFormat::Bc7Unorm: return "bc7_unorm";
    case ImagePixelFormat::Bc7Srgb: return "bc7_srgb";
    }
    return "unknown";
}

float srgbToLinear(float value) {
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float linearToSrgb(float value) {
    return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

} // namespace

std::vector<std::byte> graphImageSourcePixels(std::vector<std::byte> rgba8, std::string_view color_space,
                                              std::string_view alpha) {
    if (alpha != "premultiply") return rgba8;
    const bool srgb = color_space == "srgb";
    for (std::size_t texel = 0; texel + 3 < rgba8.size(); texel += 4) {
        const auto coverage = static_cast<float>(std::to_integer<unsigned>(rgba8[texel + 3])) / 255.0f;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            auto value = static_cast<float>(std::to_integer<unsigned>(rgba8[texel + channel])) / 255.0f;
            value = srgb ? linearToSrgb(srgbToLinear(value) * coverage) : value * coverage;
            rgba8[texel + channel] = static_cast<std::byte>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        }
    }
    return rgba8;
}

GraphImageSourceLoader::GraphImageSourceLoader(const PathResolver &paths) : paths_{paths} {}

GraphImageSourceExtent GraphImageSourceLoader::load(std::string_view node, const Json &source) {
    // The graph module checked the shape (graph.image_source_invalid@1).
    const auto asset = source.at("asset").get<std::string>();
    const auto color_space = source.value("color_space", std::string{"srgb"});
    const auto alpha = source.value("alpha", std::string{"straight"});
    const auto key = sourceKey(asset, color_space, alpha);
    if (const auto found = pixels_.find(key); found != pixels_.end())
        return {found->second->width, found->second->height};
    std::filesystem::path path;
    try {
        path = paths_.resolveExistingFile(asset);
    } catch (const std::exception &error) {
        throw GraphRegistryError("graph.image_source_missing@1",
                                 {{"node", std::string{node}}, {"asset", asset}, {"reason", error.what()}});
    }
    LoadedImage image;
    try {
        image = loadImageFile(path);
    } catch (const std::exception &error) {
        throw GraphRegistryError("graph.image_source_unreadable@1",
                                 {{"node", std::string{node}}, {"asset", asset}, {"reason", error.what()}});
    }
    if (image.format != ImagePixelFormat::Rgba8Unorm && image.format != ImagePixelFormat::Rgba8Srgb)
        throw GraphRegistryError("graph.image_source_format_unsupported@1",
                                 {{"node", std::string{node}}, {"asset", asset},
                                  {"format", std::string{pixelFormatName(image.format)}}});
    if (image.dimension != LoadedImageDimension::TwoD || image.depth != 1 || image.array_layers != 1 ||
        image.mipLevels() != 1)
        throw GraphRegistryError("graph.image_source_format_unsupported@1",
                                 {{"node", std::string{node}}, {"asset", asset},
                                  {"format", std::string{loadedImageDimensionName(image.dimension)} + " with " +
                                                 std::to_string(image.mipLevels()) + " levels"}});
    const auto bytes = static_cast<std::size_t>(image.width) * image.height * 4u;
    if (image.width == 0 || image.height == 0 || image.pixels.size() < bytes)
        throw GraphRegistryError("graph.image_source_unreadable@1",
                                 {{"node", std::string{node}}, {"asset", asset},
                                  {"reason", "the file holds no pixels"}});
    image.pixels.resize(bytes);
    auto pixels = std::make_shared<RenderTargetSourcePixels>();
    pixels->width = image.width;
    pixels->height = image.height;
    pixels->rgba8 = graphImageSourcePixels(std::move(image.pixels), color_space, alpha);
    files_.insert(path);
    pixels_.emplace(key, pixels);
    return {pixels->width, pixels->height};
}

Json resolveGraphImageSourceExtents(const Json &document, const PathResolver &paths) {
    // Decoding the file is how its size is read; the pixels are dropped.
    GraphImageSourceLoader loader{paths};
    return resolveGraphImageSources(document, [&](std::string_view node, const Json &source) {
        return loader.load(node, source);
    });
}

std::shared_ptr<const RenderTargetSourcePixels>
GraphImageSourceLoader::pixels(const RenderTargetSource &source) const {
    const auto found = pixels_.find(sourceKey(source.asset, source.color_space, source.alpha));
    if (found == pixels_.end())
        throw std::runtime_error("render target source was not read while the graph was resolved: " +
                                 source.asset);
    return found->second;
}

} // namespace Pelican
