#pragma once

#include "canvaslowering.hpp"

#include <cstdint>
#include <map>
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

CanvasImage renderCanvasReference(const CanvasOutput &output, std::uint32_t width, std::uint32_t height,
                                  const CanvasValues &values = {});

} // namespace Pelican
