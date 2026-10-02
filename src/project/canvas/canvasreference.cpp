#include "canvasreference.hpp"

#include "canvastext.hpp"
#include "distancefield.hpp"

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

double encodeSrgb(double c) {
    c = std::clamp(c, 0.0, 1.0);
    return c > 0.0031308 ? 1.055 * std::pow(c, 1.0 / 2.4) - 0.055 : c * 12.92;
}

struct Rgba {
    double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
    Rgba scaled(double k) const { return {r * k, g * k, b * k, a * k}; }
};

using Affine = std::array<double, 6>;

Vec2 itemPoint(const Affine &m, Vec2 p) {
    const auto det = m[0] * m[3] - m[2] * m[1];
    const Vec2 d{p.x - m[4], p.y - m[5]};
    return {(m[3] * d.x - m[2] * d.y) / det, (-m[1] * d.x + m[0] * d.y) / det};
}

Vec2 apply(const Affine &m, Vec2 p) { return {m[0] * p.x + m[2] * p.y + m[4], m[1] * p.x + m[3] * p.y + m[5]}; }

bool inside(Vec2 uv) { return uv.x >= 0.0 && uv.y >= 0.0 && uv.x < 1.0 && uv.y < 1.0; }

// The drawing of one output: groups first, then its own items.
class Reference {
  public:
    Reference(const CanvasOutput &output, std::uint32_t width, std::uint32_t height, const CanvasValues &values,
              const CanvasReferenceImages &images)
        : output_{output}, width_{width}, height_{height}, values_{values}, images_{images} {
        // The fit, as canvasFit in the shaders.
        scale_ = {width / output.size[0], height / output.size[1]};
        if (output.fit == CanvasFit::contain) scale_.x = scale_.y = std::min(scale_.x, scale_.y);
        offset_ = {(width - output.size[0] * scale_.x) * 0.5, (height - output.size[1] * scale_.y) * 0.5};
        margin_ = 2.0 / std::min(scale_.x, scale_.y);
        // CV-4: the output's own glyph atlases, unless the caller hands one in.
        for (const auto &atlas : output.atlases)
            atlases_[atlas.asset] = {atlas.pixels->width, atlas.pixels->height, atlas.pixels->rgba};
        for (const auto &group : output.groups) groups_.push_back(composite(group.items, group.srgb));
    }

    std::vector<float> draw() const { return composite(output_.items, false); }

  private:
    const CanvasOutput &output_;
    std::uint32_t width_;
    std::uint32_t height_;
    const CanvasValues &values_;
    const CanvasReferenceImages &images_;
    Vec2 scale_;
    Vec2 offset_;
    double margin_ = 0.0;
    std::vector<std::vector<float>> groups_;
    CanvasReferenceImages atlases_;

    Vec2 canvasPoint(double px, double py) const { return {(px - offset_.x) / scale_.x, (py - offset_.y) / scale_.y}; }

    const CanvasReferenceImage &imageOf(const CanvasTexture &texture) const {
        const auto key = texture.origin == CanvasTexture::Origin::file ? texture.asset
                                                                       : texture.node.value_or("") + "." + texture.port;
        auto found = images_.find(key);
        if (found == images_.end())
            if (const auto atlas = atlases_.find(key); atlas != atlases_.end()) return atlas->second;
        if (found == images_.end()) throw std::invalid_argument("canvas reference has no image for " + key);
        if (found->second.rgba.size() != std::size_t{found->second.width} * found->second.height * 4)
            throw std::invalid_argument("canvas reference image " + key + " does not hold width x height RGBA");
        return found->second;
    }

    // One texel as the sampler returns it (decoded, not yet filtered).
    Rgba fetch(const CanvasTexture &texture, std::int64_t x, std::int64_t y) const {
        if (texture.origin == CanvasTexture::Origin::group) {
            x = std::clamp<std::int64_t>(x, 0, std::int64_t{width_} - 1);
            y = std::clamp<std::int64_t>(y, 0, std::int64_t{height_} - 1);
            const auto *pixel = &groups_.at(texture.group)[(static_cast<std::size_t>(y) * width_ + x) * 4];
            return {pixel[0], pixel[1], pixel[2], pixel[3]};
        }
        const auto &image = imageOf(texture);
        x = std::clamp<std::int64_t>(x, 0, std::int64_t{image.width} - 1);
        y = std::clamp<std::int64_t>(y, 0, std::int64_t{image.height} - 1);
        const auto *pixel = &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4];
        const auto channel = [&](std::size_t c) {
            const auto byte = pixel[c] / 255.0;
            return texture.origin == CanvasTexture::Origin::file && !texture.linear && c < 3 ? linear(byte) : byte;
        };
        return {channel(0), channel(1), channel(2), channel(3)};
    }

    // Clamp to edge, linear or nearest, as the ports' samplers.
    Rgba sample(const CanvasTexture &texture, Vec2 uv) const {
        double w = width_, h = height_;
        if (texture.origin != CanvasTexture::Origin::group) {
            const auto &image = imageOf(texture);
            w = image.width;
            h = image.height;
        }
        if (texture.nearest)
            return fetch(texture, static_cast<std::int64_t>(std::floor(uv.x * w)),
                         static_cast<std::int64_t>(std::floor(uv.y * h)));
        const auto x = uv.x * w - 0.5, y = uv.y * h - 0.5;
        const auto x0 = std::floor(x), y0 = std::floor(y);
        const auto fx = x - x0, fy = y - y0;
        const auto ix = static_cast<std::int64_t>(x0), iy = static_cast<std::int64_t>(y0);
        const auto t00 = fetch(texture, ix, iy), t10 = fetch(texture, ix + 1, iy);
        const auto t01 = fetch(texture, ix, iy + 1), t11 = fetch(texture, ix + 1, iy + 1);
        const auto mix = [&](double a, double b, double c, double d) {
            return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
        };
        return {mix(t00.r, t10.r, t01.r, t11.r), mix(t00.g, t10.g, t01.g, t11.g), mix(t00.b, t10.b, t01.b, t11.b),
                mix(t00.a, t10.a, t01.a, t11.a)};
    }

    static Rgba texel(Rgba t, double alpha_mode, double encode) {
        const auto a = alpha_mode > 1.5 ? 1.0 : std::clamp(t.a, 0.0, 1.0);
        std::array<double, 3> rgb{t.r, t.g, t.b};
        for (auto &c : rgb) {
            if (alpha_mode > 0.5 && alpha_mode < 1.5) c = a > 0.0 ? c / a : 0.0;
            if (encode > 1.5) c = linear(c);
            else if (encode > 0.5) c = encodeSrgb(c);
        }
        return {rgb[0] * a, rgb[1] * a, rgb[2] * a, a};
    }

    double param(const CanvasItem &item, std::size_t index) const { return value(item.parameters.at(index), values_); }

    double mask(const CanvasItem &item, Vec2 p) const {
        double m = 1.0;
        for (const auto &mask : item.masks) {
            const auto q = itemPoint(mask.affine, p);
            const Vec2 uv{(q.x - mask.rect[0]) / mask.rect[2], (q.y - mask.rect[1]) / mask.rect[3]};
            auto v = mask.outside;
            if (inside(uv)) {
                const auto t = sample(mask.texture, uv);
                v = mask.luminance ? 0.2126 * t.r + 0.7152 * t.g + 0.0722 * t.b : t.a;
            }
            m *= v;
        }
        return m;
    }

    // canvasPaint: premultiplied, in the list's blend space, without opacity
    // or clip; (px, py) is the pixel centre.
    Rgba paint(const CanvasItem &item, double px, double py, bool srgb) const {
        const auto p = canvasPoint(px, py);
        const auto q = itemPoint(item.affine, p);
        const auto shape_color = [&](double c) { return srgb ? std::clamp(c, 0.0, 1.0) : linear(c); };
        Rgba color;
        switch (item.source) {
        case CanvasItemSource::shape: {
            const auto resolved = resolve(item, values_);
            const auto distance = [&](double x, double y) {
                const auto at = itemPoint(item.affine, canvasPoint(x, y));
                return resolved.distance({at.x - resolved.center.x, at.y - resolved.center.y});
            };
            const auto d = distance(px, py);
            const auto aa = std::max(length({distance(px + 1.0, py) - d, distance(px, py + 1.0) - d}), 1e-6);
            const auto a = std::clamp(resolved.color[3], 0.0, 1.0) * std::clamp(0.5 - d / aa, 0.0, 1.0);
            color = {shape_color(resolved.color[0]) * a, shape_color(resolved.color[1]) * a,
                     shape_color(resolved.color[2]) * a, a};
            break;
        }
        case CanvasItemSource::image: {
            const Vec2 uv{(q.x - param(item, 0)) / param(item, 2), (q.y - param(item, 1)) / param(item, 3)};
            if (inside(uv)) color = texel(sample(item.texture, uv), param(item, 4), param(item, 5));
            break;
        }
        case CanvasItemSource::color: {
            if (inside({q.x / output_.size[0], q.y / output_.size[1]})) {
                const auto a = std::clamp(param(item, 3), 0.0, 1.0);
                color = {shape_color(param(item, 0)) * a, shape_color(param(item, 1)) * a,
                         shape_color(param(item, 2)) * a, a};
            }
            break;
        }
        case CanvasItemSource::group: {
            const Vec2 uv{(offset_.x + q.x * scale_.x) / width_, (offset_.y + q.y * scale_.y) / height_};
            color = texel(sample(item.texture, uv), 1.0, param(item, 0));
            break;
        }
        case CanvasItemSource::glyph: {
            // As the generated shader: the texel in the glyph's cell, the
            // atlas read with the filter, the distance reconstructed, and
            // the texels per pixel from the next pixel over.
            const auto s = param(item, 2);
            const Vec2 cell_min{param(item, 0) + s * param(item, 3), param(item, 1) + s * param(item, 4)};
            const Vec2 cell_size{s * param(item, 5), s * param(item, 6)};
            const auto n = param(item, 11);
            const auto texel_at = [&](double x, double y) {
                const auto at = itemPoint(item.affine, canvasPoint(x, y));
                return Vec2{(at.x - cell_min.x) / cell_size.x * n, (at.y - cell_min.y) / cell_size.y * n};
            };
            const auto tex = texel_at(px, py);
            const Vec2 uv{param(item, 7) + std::clamp(tex.x, 0.5, n - 0.5) / n * param(item, 9),
                          param(item, 8) + std::clamp(tex.y, 0.5, n - 0.5) / n * param(item, 10)};
            const auto t = sample(item.texture, uv);
            const auto decode = [](double v) { return static_cast<float>((v - 0.5) * 2.0 * canvas_glyph_range); };
            const float rgba[4] = {decode(t.r), decode(t.g), decode(t.b), decode(t.a)};
            double d = reconstructMtsdfNear(rgba, 0.5f, 1.0f);
            const auto base = canvas_glyph_parameter_count;
            if (item.paint == CanvasPaint::stroke) {
                const auto align = param(item, base + 5);
                const auto limit = (align > 0.5 ? 1.0 : 2.0) * (canvas_glyph_range - 1.0);
                d = strokeDistance(d, std::min(std::abs(param(item, base + 4) * n / std::abs(cell_size.x)), limit),
                                   align);
            }
            const auto right = texel_at(px + 1.0, py), below = texel_at(px, py + 1.0);
            const auto aa = std::max(length({length({right.x - tex.x, right.y - tex.y}),
                                             length({below.x - tex.x, below.y - tex.y})}) *
                                         0.70710678,
                                     1e-6);
            const auto a = std::clamp(param(item, base + 3), 0.0, 1.0) * std::clamp(0.5 - d / aa, 0.0, 1.0);
            color = {shape_color(param(item, base)) * a, shape_color(param(item, base + 1)) * a,
                     shape_color(param(item, base + 2)) * a, a};
            break;
        }
        case CanvasItemSource::backdrop:
            // CV-2b: read from the image being drawn (composite does).
            break;
        }
        return color.scaled(mask(item, p));
    }

    static CanvasRgb straight(const Rgba &c) {
        if (!(c.a > 0.0)) return {0.0, 0.0, 0.0};
        return {std::clamp(c.r / c.a, 0.0, 1.0), std::clamp(c.g / c.a, 0.0, 1.0), std::clamp(c.b / c.a, 0.0, 1.0)};
    }

    // CV-2b: canvasStyle -- the item's colour through its filters, then over
    // `back` (what the list drew below it) by its blend; atop, the backdrop
    // counts as opaque.
    static Rgba style(const CanvasItem &item, const Rgba &color, const Rgba &back, bool srgb, bool atop) {
        if (item.filters.empty() && item.blend == CanvasBlend::normal) return color;
        auto c = straight(color);
        for (const auto &filter : item.filters) c = applyCanvasFilter(filter, c, srgb);
        const auto a = color.a;
        if (item.blend == CanvasBlend::normal) return {c[0] * a, c[1] * a, c[2] * a, a};
        const auto mixed = canvasBlendColor(item.blend, straight(back), c);
        if (atop) return {mixed[0] * a, mixed[1] * a, mixed[2] * a, a};
        const auto keep = 1.0 - back.a;
        return {c[0] * a * keep + mixed[0] * a * back.a, c[1] * a * keep + mixed[1] * a * back.a,
                c[2] * a * keep + mixed[2] * a * back.a, a};
    }

    double opacity(const CanvasItem &item) const {
        auto result = item.opacity;
        for (const auto &edge : item.opacity_edges) {
            const auto found = values_.find(edge);
            if (found == values_.end())
                throw std::invalid_argument("canvas reference has no value for " + edge.node + "." + edge.port);
            result *= found->second;
        }
        return std::clamp(result, 0.0, 1.0);
    }

    // The item's quad in item space, as the vertex shader builds it.
    std::pair<Vec2, Vec2> bounds(const CanvasItem &item) const {
        switch (item.source) {
        case CanvasItemSource::shape: {
            const auto resolved = resolve(item, values_);
            return {resolved.center, resolved.bound};
        }
        case CanvasItemSource::image: {
            const Vec2 size{param(item, 2), param(item, 3)};
            return {{param(item, 0) + 0.5 * size.x, param(item, 1) + 0.5 * size.y},
                    {0.5 * std::abs(size.x), 0.5 * std::abs(size.y)}};
        }
        case CanvasItemSource::glyph: {
            const auto s = param(item, 2);
            const Vec2 size{s * param(item, 5), s * param(item, 6)};
            return {{param(item, 0) + s * param(item, 3) + 0.5 * size.x, param(item, 1) + s * param(item, 4) + 0.5 * size.y},
                    {0.5 * std::abs(size.x), 0.5 * std::abs(size.y)}};
        }
        case CanvasItemSource::color:
        case CanvasItemSource::group:
        case CanvasItemSource::backdrop: break;
        }
        const Vec2 half{0.5 * output_.size[0], 0.5 * output_.size[1]};
        return {half, half};
    }

    std::vector<float> composite(const std::vector<CanvasItem> &items, bool srgb) const {
        std::vector<float> pixels(std::size_t{width_} * height_ * 4, 0.0f);
        for (const auto &item : items) {
            const auto [center, bound] = bounds(item);
            // The pixels whose centres the item's quad covers (its bounding box
            // when it is turned; the paint is empty past the quad).
            double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
            for (const auto corner : {Vec2{-1, -1}, Vec2{1, -1}, Vec2{1, 1}, Vec2{-1, 1}}) {
                const auto canvas = apply(item.affine, {center.x + corner.x * (bound.x + margin_),
                                                        center.y + corner.y * (bound.y + margin_)});
                const auto px = offset_.x + canvas.x * scale_.x, py = offset_.y + canvas.y * scale_.y;
                x0 = std::min(x0, px), x1 = std::max(x1, px), y0 = std::min(y0, py), y1 = std::max(y1, py);
            }
            const auto first_x = static_cast<std::int64_t>(std::max(0.0, std::ceil(x0 - 0.5)));
            const auto last_x = static_cast<std::int64_t>(std::min<double>(width_, std::ceil(x1 - 0.5)));
            const auto first_y = static_cast<std::int64_t>(std::max(0.0, std::ceil(y0 - 0.5)));
            const auto last_y = static_cast<std::int64_t>(std::min<double>(height_, std::ceil(y1 - 0.5)));
            const auto item_opacity = opacity(item);
            const bool atop = item.clip_base.has_value() || item.source == CanvasItemSource::backdrop;
            for (auto y = first_y; y < last_y; ++y) {
                for (auto x = first_x; x < last_x; ++x) {
                    const auto px = x + 0.5, py = y + 0.5;
                    auto *pixel = &pixels[(static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x)) * 4];
                    // What the list drew below the item: its backdrop.
                    const Rgba back{pixel[0], pixel[1], pixel[2], pixel[3]};
                    Rgba color;
                    if (item.source == CanvasItemSource::backdrop) {
                        const auto p = canvasPoint(px, py);
                        const auto q = itemPoint(item.affine, p);
                        if (inside({q.x / output_.size[0], q.y / output_.size[1]})) {
                            const auto c = straight(back);
                            color = Rgba{c[0], c[1], c[2], 1.0}.scaled(mask(item, p));
                        }
                    } else {
                        color = paint(item, px, py, srgb);
                    }
                    color = style(item, color.scaled(item_opacity), back, srgb, atop);
                    if (color.a <= 0.0) continue;
                    const auto keep = 1.0f - static_cast<float>(color.a);
                    if (atop) {
                        // Atop: weighted by the alpha below, which it keeps.
                        const auto below = static_cast<double>(pixel[3]);
                        pixel[0] = static_cast<float>(color.r * below) + pixel[0] * keep;
                        pixel[1] = static_cast<float>(color.g * below) + pixel[1] * keep;
                        pixel[2] = static_cast<float>(color.b * below) + pixel[2] * keep;
                        continue;
                    }
                    pixel[0] = static_cast<float>(color.r) + pixel[0] * keep;
                    pixel[1] = static_cast<float>(color.g) + pixel[1] * keep;
                    pixel[2] = static_cast<float>(color.b) + pixel[2] * keep;
                    pixel[3] = static_cast<float>(color.a) + pixel[3] * keep;
                }
            }
        }
        return pixels;
    }
};

} // namespace

CanvasImage renderCanvasReference(const CanvasOutput &output, std::uint32_t width, std::uint32_t height,
                                  const CanvasValues &values, const CanvasReferenceImages &images) {
    if (width == 0 || height == 0) throw std::invalid_argument("canvas reference image must not be empty");
    const Reference reference{output, width, height, values, images};
    return CanvasImage{width, height, reference.draw()};
}

} // namespace Pelican
