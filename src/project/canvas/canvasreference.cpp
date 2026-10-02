#include "canvasreference.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace Pelican {
namespace {

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

double length(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

double value(const CanvasParameter &parameter, const CanvasValues &values) {
    if (!parameter.edge) return parameter.value;
    const auto found = values.find(*parameter.edge);
    if (found == values.end())
        throw std::invalid_argument("canvas reference has no value for " + parameter.edge->node + "." +
                                    parameter.edge->port);
    return found->second;
}

double radians(double degrees) { return degrees * std::numbers::pi / 180.0; }

Vec2 rotate(Vec2 p, double angle) {
    const auto c = std::cos(angle), s = std::sin(angle);
    return {c * p.x - s * p.y, s * p.x + c * p.y};
}

double roundRect(Vec2 p, Vec2 half_size, double radius) {
    const Vec2 q{std::abs(p.x) - half_size.x + radius, std::abs(p.y) - half_size.y + radius};
    return length({std::max(q.x, 0.0), std::max(q.y, 0.0)}) + std::min(std::max(q.x, q.y), 0.0) - radius;
}

double ellipse(Vec2 p, Vec2 radii) {
    radii = {std::max(radii.x, 1e-6), std::max(radii.y, 1e-6)};
    const auto k0 = length({p.x / radii.x, p.y / radii.y});
    const auto k1 = length({p.x / (radii.x * radii.x), p.y / (radii.y * radii.y)});
    return k1 > 1e-12 ? k0 * (k0 - 1.0) / k1 : -std::min(radii.x, radii.y);
}

double strokeDistance(double d, double width, double align) {
    const auto half_width = 0.5 * std::abs(width);
    const auto shift = align > 1.5 ? half_width : (align > 0.5 ? -half_width : 0.0);
    return std::abs(d - shift) - half_width;
}

double strokeOutset(double width, double align) {
    return align > 1.5 ? std::abs(width) : (align > 0.5 ? 0.0 : 0.5 * std::abs(width));
}

double linear(double c) {
    c = std::clamp(c, 0.0, 1.0);
    return c > 0.04045 ? std::pow((c + 0.055) / 1.055, 2.4) : c / 12.92;
}

// One item with its parameters read: what the shaders compute per instance.
struct Resolved {
    CanvasShape shape;
    bool stroke;
    Vec2 half_size;
    Vec2 center;
    double radius = 0.0;
    double rotation = 0.0;
    std::array<double, 4> color{};
    double width = 0.0;
    double align = 0.0;
    double opacity = 1.0;
    Vec2 bound;

    double distance(Vec2 local) const {
        const auto q = rotate(local, -rotation);
        auto d = shape == CanvasShape::rect
                     ? roundRect(q, half_size, std::clamp(radius, 0.0, std::min(half_size.x, half_size.y)))
                     : ellipse(q, half_size);
        return stroke ? strokeDistance(d, width, align) : d;
    }
};

Resolved resolve(const CanvasItem &item, const CanvasValues &values) {
    Resolved result{item.shape, item.paint == CanvasPaint::stroke, {}, {}};
    const auto p = [&](std::size_t index) { return value(item.parameters.at(index), values); };
    result.half_size = {0.5 * std::abs(p(0)), 0.5 * std::abs(p(1))};
    result.center = {p(2), p(3)};
    const auto base = canvasShapeParameterCount(item.shape);
    if (item.shape == CanvasShape::rect) result.radius = p(4);
    result.rotation = radians(p(base - 1));
    for (std::size_t i = 0; i < 4; ++i) result.color[i] = p(base + i);
    if (result.stroke) {
        result.width = p(base + 4);
        result.align = p(base + 5);
    }
    result.opacity = item.opacity;
    for (const auto &edge : item.opacity_edges) {
        const auto found = values.find(edge);
        if (found == values.end())
            throw std::invalid_argument("canvas reference has no value for " + edge.node + "." + edge.port);
        result.opacity *= found->second;
    }
    result.opacity = std::clamp(result.opacity, 0.0, 1.0);
    const auto c = std::abs(std::cos(result.rotation)), s = std::abs(std::sin(result.rotation));
    const auto h = result.half_size;
    result.bound = item.shape == CanvasShape::rect
                       ? Vec2{c * h.x + s * h.y, s * h.x + c * h.y}
                       : Vec2{length({h.x * c, h.y * s}), length({h.x * s, h.y * c})};
    if (result.stroke) {
        const auto outset = strokeOutset(result.width, result.align);
        result.bound = {result.bound.x + outset, result.bound.y + outset};
    }
    return result;
}

} // namespace

CanvasImage renderCanvasReference(const CanvasOutput &output, std::uint32_t width, std::uint32_t height,
                                  const CanvasValues &values) {
    if (width == 0 || height == 0) throw std::invalid_argument("canvas reference image must not be empty");
    CanvasImage image{width, height, std::vector<float>(std::size_t{width} * height * 4, 0.0f)};
    // The fit, as canvasFit in the vertex shader.
    Vec2 scale{width / output.size[0], height / output.size[1]};
    if (output.fit == CanvasFit::contain) scale.x = scale.y = std::min(scale.x, scale.y);
    const Vec2 offset{(width - output.size[0] * scale.x) * 0.5, (height - output.size[1] * scale.y) * 0.5};
    const auto margin = 2.0 / std::min(scale.x, scale.y);
    for (const auto &item : output.items) {
        const auto resolved = resolve(item, values);
        // The pixels whose centres the item's quad covers.
        const auto x0 = offset.x + (resolved.center.x - resolved.bound.x - margin) * scale.x;
        const auto x1 = offset.x + (resolved.center.x + resolved.bound.x + margin) * scale.x;
        const auto y0 = offset.y + (resolved.center.y - resolved.bound.y - margin) * scale.y;
        const auto y1 = offset.y + (resolved.center.y + resolved.bound.y + margin) * scale.y;
        const auto first_x = static_cast<std::int64_t>(std::max(0.0, std::ceil(x0 - 0.5)));
        const auto last_x = static_cast<std::int64_t>(std::min<double>(width, std::ceil(x1 - 0.5)));
        const auto first_y = static_cast<std::int64_t>(std::max(0.0, std::ceil(y0 - 0.5)));
        const auto last_y = static_cast<std::int64_t>(std::min<double>(height, std::ceil(y1 - 0.5)));
        const auto local = [&](double px, double py) {
            return Vec2{(px - offset.x) / scale.x - resolved.center.x, (py - offset.y) / scale.y - resolved.center.y};
        };
        const std::array<double, 3> rgb{linear(resolved.color[0]), linear(resolved.color[1]),
                                        linear(resolved.color[2])};
        for (auto y = first_y; y < last_y; ++y) {
            for (auto x = first_x; x < last_x; ++x) {
                const auto px = x + 0.5, py = y + 0.5;
                const auto d = resolved.distance(local(px, py));
                const auto dx = resolved.distance(local(px + 1.0, py)) - d;
                const auto dy = resolved.distance(local(px, py + 1.0)) - d;
                const auto aa = std::max(length({dx, dy}), 1e-6);
                const auto coverage = std::clamp(0.5 - d / aa, 0.0, 1.0);
                const auto alpha = std::clamp(resolved.color[3], 0.0, 1.0) * coverage * resolved.opacity;
                if (alpha <= 0.0) continue;
                auto *pixel = &image.pixels[(static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4];
                const auto keep = 1.0f - static_cast<float>(alpha);
                for (std::size_t c = 0; c < 3; ++c)
                    pixel[c] = static_cast<float>(rgb[c] * alpha) + pixel[c] * keep;
                pixel[3] = static_cast<float>(alpha) + pixel[3] * keep;
            }
        }
    }
    return image;
}

} // namespace Pelican
