#pragma once

#include "canvaslowering.hpp"
#include "canvasreference.hpp"

#include <array>
#include <cstddef>

namespace Pelican {

// CV-6a: where a canvas.place puts its items, on the CPU. The 3D picture is
// the GPU's alone; this is the arithmetic of its vertex shader, so a test can
// hold the pixels against it and an editor can draw a gizmo on the placed
// stack. Edge-driven numbers read `values` (keyed by the edge's producer
// endpoint, as the CPU reference does); a missing one is refused.

// A 4 x 4 matrix, column-major as glm keeps one: row r of column c is [c * 4 + r].
using CanvasMatrix = std::array<double, 16>;

// The map from a point of the placed stack -- x, y in stack units (origin top
// left, +y down) and z its depth in stack units (toward the viewer) -- to the
// world: (x - anchor.x * width, anchor.y * height - y, z) / pixels_per_unit,
// scaled, rotated by Rz Ry Rx (X first, degrees) and moved to position.
// `output` is a canvas.place's (CanvasOutput::placement is set).
CanvasMatrix canvasPlacementMatrix(const CanvasOutput &output, const CanvasValues &values = {});

// The four corners of one item's rectangle (the vertex shader's quad, without
// the antialiasing margin) in the order top left, top right, bottom right,
// bottom left of the item's own stack.
struct CanvasPlacedCorners {
    // In the placed stack (after the item's canvas.transform), and the item's depth.
    std::array<std::array<double, 2>, 4> stack{};
    double depth = 0.0;
    std::array<std::array<double, 3>, 4> world{};
    // projection * view * world.
    std::array<std::array<double, 4>, 4> clip{};
    // Pixels from the top left of the viewport (NDC y = -1 is the top row, as
    // Vulkan rasterizes); meaningful when `in_front` (every w positive).
    std::array<std::array<double, 2>, 4> pixel{};
    bool in_front = false;
};

CanvasPlacedCorners canvasPlacedItemCorners(const CanvasOutput &output, std::size_t item, const CanvasMatrix &view,
                                            const CanvasMatrix &projection, std::array<double, 2> viewport,
                                            const CanvasValues &values = {});

} // namespace Pelican
