#pragma once

#include "canvaslowering.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Pelican {

// CV-1: the CPU reference of a lowered canvas. It draws a CanvasOutput's items
// the way the generated shaders do -- the same distances, the same coverage
// (the gradient is the difference to the next pixel), the same sRGB decode,
// premultiplied over a transparent image -- so a test can hold the GPU's
// picture against it, and an editor can ask which item a pixel shows.
//
// An edge-driven parameter (or opacity) reads `values`, keyed by the edge's
// producer endpoint; a missing one is refused, never defaulted.
using CanvasValues = std::map<CanvasValueSource, double>;

struct CanvasImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // RGBA, row-major from the top left, linear and premultiplied.
    std::vector<float> pixels;
};

// CV-2a: the images the items sample, as the caller decoded them (the
// reference reads no file): straight RGBA bytes, row-major from the top left.
// A file texture is keyed by its asset and read as the shader reads it (the
// sRGB decode, or the bytes as they are when `linear`); a canvas.from_image
// texture by "<node>.<port>", its bytes taken as linear values. A missing one
// is refused. CV-4: an output's glyph atlases come with the output; one handed
// in under the same asset is read instead.
struct CanvasReferenceImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};
using CanvasReferenceImages = std::map<std::string, CanvasReferenceImage>;

// Groups are composited first, each into an image of the output's size, and
// read back as the shader reads them; masks, clips and transforms as the
// generated shader computes them.
CanvasImage renderCanvasReference(const CanvasOutput &output, std::uint32_t width, std::uint32_t height,
                                  const CanvasValues &values = {}, const CanvasReferenceImages &images = {});

} // namespace Pelican
