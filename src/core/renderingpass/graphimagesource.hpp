#pragma once

#include "rendertargetdefinition.hpp"
#include "../../project/graphpartition.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <string_view>

namespace Pelican {

class PathResolver;

// EN-2: the image files a generation's render.image nodes read
// (`parameters.source`). Registration reads each file once while it resolves
// the graph (load), which gives the compiler the file's size, and hands the
// decoded pixels to the render targets that declare it (pixels). Failures are
// named:
//   graph.image_source_missing@1 {node, asset, reason}: the reference names no file
//   graph.image_source_unreadable@1 {node, asset, reason}: the file does not decode
//   graph.image_source_format_unsupported@1 {node, asset, format}: not an 8-bit
//     RGBA 2D image of one level (float and block-compressed files come later)
class GraphImageSourceLoader {
  public:
    explicit GraphImageSourceLoader(const PathResolver &paths);

    GraphImageSourceExtent load(std::string_view node, const nlohmann::json &source);
    // The pixels load() decoded for a lowered source.
    std::shared_ptr<const RenderTargetSourcePixels> pixels(const RenderTargetSource &source) const;
    // Every file read, for the generation's watched sources.
    const std::set<std::filesystem::path> &files() const noexcept { return files_; }

  private:
    const PathResolver &paths_;
    std::map<std::string, std::shared_ptr<const RenderTargetSourcePixels>> pixels_;
    std::set<std::filesystem::path> files_;
};

// EN-2: the paths that compose a document as written (the render authoring
// context, an edit's preflight, a feature edit) take each render.image
// source's size from its file, as registration does, without keeping the
// pixels. A document with no render.image source is returned unchanged.
nlohmann::json resolveGraphImageSourceExtents(const nlohmann::json &document, const PathResolver &paths);

// The RGBA8 pixels as a target of `source` holds them: straight, or with RGB
// multiplied by alpha in linear space (decoding and re-encoding sRGB when the
// color space is srgb).
std::vector<std::byte> graphImageSourcePixels(std::vector<std::byte> rgba8, std::string_view color_space,
                                              std::string_view alpha);

} // namespace Pelican
