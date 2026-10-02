#include "canvasplace.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace Pelican {
namespace {

double resolved(const CanvasParameter &parameter, const CanvasValues &values) {
    if (!parameter.edge) return parameter.value;
    const auto found = values.find(*parameter.edge);
    if (found == values.end())
        throw std::invalid_argument("canvas.place corners: no value for " + parameter.edge->node + "." +
                                    parameter.edge->port);
    return found->second;
}

CanvasMatrix multiply(const CanvasMatrix &a, const CanvasMatrix &b) {
    CanvasMatrix result{};
    for (std::size_t column = 0; column < 4; ++column)
        for (std::size_t row = 0; row < 4; ++row) {
            double sum = 0.0;
            for (std::size_t k = 0; k < 4; ++k) sum += a[k * 4 + row] * b[column * 4 + k];
            result[column * 4 + row] = sum;
        }
    return result;
}

std::array<double, 4> transform(const CanvasMatrix &m, const std::array<double, 4> &v) {
    std::array<double, 4> result{};
    for (std::size_t row = 0; row < 4; ++row)
        result[row] = m[row] * v[0] + m[4 + row] * v[1] + m[8 + row] * v[2] + m[12 + row] * v[3];
    return result;
}

// The rotation about one axis (0 x, 1 y, 2 z), in degrees.
CanvasMatrix axisRotation(std::size_t axis, double degrees) {
    const auto radians = degrees * 3.14159265358979323846 / 180.0;
    const auto c = std::cos(radians), s = std::sin(radians);
    CanvasMatrix m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const std::size_t i = (axis + 1) % 3, j = (axis + 2) % 3;
    m[i * 4 + i] = c;
    m[i * 4 + j] = s;
    m[j * 4 + i] = -s;
    m[j * 4 + j] = c;
    return m;
}

// An item's centre and half extent in its own stack, as the vertex shader
// computes them (itemBoundsCases).
void itemBounds(const CanvasOutput &output, const CanvasItem &item, const CanvasValues &values,
                std::array<double, 2> &center, std::array<double, 2> &bound) {
    const auto p = [&](std::size_t index) { return resolved(item.parameters.at(index), values); };
    switch (item.source) {
    case CanvasItemSource::image:
        center = {p(0) + 0.5 * p(2), p(1) + 0.5 * p(3)};
        bound = {0.5 * std::abs(p(2)), 0.5 * std::abs(p(3))};
        return;
    case CanvasItemSource::glyph: {
        const auto s = p(2);
        const std::array<double, 2> cell{s * p(5), s * p(6)};
        center = {p(0) + s * p(3) + 0.5 * cell[0], p(1) + s * p(4) + 0.5 * cell[1]};
        bound = {0.5 * std::abs(cell[0]), 0.5 * std::abs(cell[1])};
        return;
    }
    case CanvasItemSource::color:
    case CanvasItemSource::group:
        center = {0.5 * output.size[0], 0.5 * output.size[1]};
        bound = center;
        return;
    case CanvasItemSource::shape: break;
    }
    const std::array<double, 2> half{0.5 * std::abs(p(0)), 0.5 * std::abs(p(1))};
    center = {p(2), p(3)};
    const auto rotation = p(item.shape == CanvasShape::rect ? 5 : 4) * 3.14159265358979323846 / 180.0;
    const auto c = std::abs(std::cos(rotation)), s = std::abs(std::sin(rotation));
    if (item.shape == CanvasShape::rect)
        bound = {c * half[0] + s * half[1], s * half[0] + c * half[1]};
    else
        bound = {std::hypot(half[0] * c, half[1] * s), std::hypot(half[0] * s, half[1] * c)};
    if (item.paint == CanvasPaint::stroke) {
        const auto paint = canvasShapeParameterCount(item.shape);
        const auto width = std::abs(p(paint + 4));
        const auto align = p(paint + 5);
        const auto outset = align > 1.5 ? width : (align > 0.5 ? 0.0 : 0.5 * width);
        bound = {bound[0] + outset, bound[1] + outset};
    }
}

} // namespace

CanvasMatrix canvasPlacementMatrix(const CanvasOutput &output, const CanvasValues &values) {
    if (!output.placement) throw std::invalid_argument("not a canvas.place: " + output.id);
    const auto &placement = *output.placement;
    const auto units = placement.pixels_per_unit;
    // Stack units to the stack's own frame: the anchor at the origin, +y up.
    const CanvasMatrix local{1.0 / units, 0, 0, 0,
                             0, -1.0 / units, 0, 0,
                             0, 0, 1.0 / units, 0,
                             -placement.anchor[0] * output.size[0] / units,
                             placement.anchor[1] * output.size[1] / units, 0, 1};
    const auto scale = resolved(placement.scale, values);
    const CanvasMatrix scaling{scale, 0, 0, 0, 0, scale, 0, 0, 0, 0, scale, 0, 0, 0, 0, 1};
    auto rotation = axisRotation(0, resolved(placement.rotation[0], values));
    rotation = multiply(axisRotation(1, resolved(placement.rotation[1], values)), rotation);
    rotation = multiply(axisRotation(2, resolved(placement.rotation[2], values)), rotation);
    CanvasMatrix translation{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (std::size_t axis = 0; axis < 3; ++axis) translation[12 + axis] = resolved(placement.position[axis], values);
    return multiply(translation, multiply(rotation, multiply(scaling, local)));
}

CanvasPlacedCorners canvasPlacedItemCorners(const CanvasOutput &output, std::size_t index, const CanvasMatrix &view,
                                            const CanvasMatrix &projection, std::array<double, 2> viewport,
                                            const CanvasValues &values) {
    const auto &item = output.items.at(index);
    const auto placement = canvasPlacementMatrix(output, values);
    const auto view_projection = multiply(projection, view);
    std::array<double, 2> center{}, bound{};
    itemBounds(output, item, values, center, bound);
    CanvasPlacedCorners corners;
    corners.depth = item.depth + (item.depth_edge ? resolved(*item.depth_edge, values) : 0.0);
    corners.in_front = true;
    const double signs[4][2] = {{-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0}};
    const auto &a = item.affine;
    for (std::size_t k = 0; k < 4; ++k) {
        const double x = center[0] + signs[k][0] * bound[0];
        const double y = center[1] + signs[k][1] * bound[1];
        corners.stack[k] = {a[0] * x + a[2] * y + a[4], a[1] * x + a[3] * y + a[5]};
        const auto world = transform(placement, {corners.stack[k][0], corners.stack[k][1], corners.depth, 1.0});
        corners.world[k] = {world[0], world[1], world[2]};
        corners.clip[k] = transform(view_projection, world);
        const auto w = corners.clip[k][3];
        if (!(w > 0.0)) {
            corners.in_front = false;
            continue;
        }
        corners.pixel[k] = {(corners.clip[k][0] / w * 0.5 + 0.5) * viewport[0],
                            (corners.clip[k][1] / w * 0.5 + 0.5) * viewport[1]};
    }
    return corners;
}

} // namespace Pelican
