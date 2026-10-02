#include "canvasblend.hpp"

#include "../graphcomposition.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Pelican {
namespace {
using Json = nlohmann::json;

std::string nodeId(const Json &node) {
    const auto found = node.find("id");
    return found == node.end() || !found->is_string() ? std::string{} : found->get<std::string>();
}

// --- blend modes --------------------------------------------------------

double colorDodge(double b, double s) { return b <= 0.0 ? 0.0 : (s >= 1.0 ? 1.0 : std::min(1.0, b / (1.0 - s))); }
double colorBurn(double b, double s) { return b >= 1.0 ? 1.0 : (s <= 0.0 ? 0.0 : 1.0 - std::min(1.0, (1.0 - b) / s)); }
double hardLight(double b, double s) { return s <= 0.5 ? 2.0 * b * s : 1.0 - 2.0 * (1.0 - b) * (1.0 - s); }

// One row per mode, in enum order: its name, and for a separable mode the
// channel function as C++ and as the GLSL body of
// `float canvasBlendChannel<index>(float b, float s)`; a non-separable mode's
// GLSL is the body of `vec3 canvasBlend<index>(vec3 b, vec3 s)`.
struct BlendRow {
    CanvasBlend blend;
    const char *name;
    double (*channel)(double b, double s);
    const char *glsl;
};

const BlendRow blend_rows[] = {
    {CanvasBlend::normal, "normal", [](double, double s) { return s; }, "return s;"},
    {CanvasBlend::multiply, "multiply", [](double b, double s) { return b * s; }, "return b * s;"},
    {CanvasBlend::screen, "screen", [](double b, double s) { return b + s - b * s; }, "return b + s - b * s;"},
    {CanvasBlend::overlay, "overlay", [](double b, double s) { return hardLight(s, b); },
     "return b <= 0.5 ? 2.0 * b * s : 1.0 - 2.0 * (1.0 - b) * (1.0 - s);"},
    {CanvasBlend::darken, "darken", [](double b, double s) { return std::min(b, s); }, "return min(b, s);"},
    {CanvasBlend::lighten, "lighten", [](double b, double s) { return std::max(b, s); }, "return max(b, s);"},
    {CanvasBlend::color_dodge, "color_dodge", colorDodge,
     "return b <= 0.0 ? 0.0 : (s >= 1.0 ? 1.0 : min(1.0, b / (1.0 - s)));"},
    {CanvasBlend::color_burn, "color_burn", colorBurn,
     "return b >= 1.0 ? 1.0 : (s <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0 - b) / s));"},
    {CanvasBlend::hard_light, "hard_light", hardLight,
     "return s <= 0.5 ? 2.0 * b * s : 1.0 - 2.0 * (1.0 - b) * (1.0 - s);"},
    {CanvasBlend::soft_light, "soft_light",
     [](double b, double s) {
         const auto d = b <= 0.25 ? ((16.0 * b - 12.0) * b + 4.0) * b : std::sqrt(b);
         return s <= 0.5 ? b - (1.0 - 2.0 * s) * b * (1.0 - b) : b + (2.0 * s - 1.0) * (d - b);
     },
     "float d = b <= 0.25 ? ((16.0 * b - 12.0) * b + 4.0) * b : sqrt(b);\n"
     "    return s <= 0.5 ? b - (1.0 - 2.0 * s) * b * (1.0 - b) : b + (2.0 * s - 1.0) * (d - b);"},
    {CanvasBlend::difference, "difference", [](double b, double s) { return std::abs(b - s); }, "return abs(b - s);"},
    {CanvasBlend::exclusion, "exclusion", [](double b, double s) { return b + s - 2.0 * b * s; },
     "return b + s - 2.0 * b * s;"},
    {CanvasBlend::hue, "hue", nullptr, "return canvasSetLum(canvasSetSat(s, canvasSat(b)), canvasLum(b));"},
    {CanvasBlend::saturation, "saturation", nullptr,
     "return canvasSetLum(canvasSetSat(b, canvasSat(s)), canvasLum(b));"},
    {CanvasBlend::color, "color", nullptr, "return canvasSetLum(s, canvasLum(b));"},
    {CanvasBlend::luminosity, "luminosity", nullptr, "return canvasSetLum(b, canvasLum(s));"},
    {CanvasBlend::linear_dodge, "linear_dodge", [](double b, double s) { return std::min(1.0, b + s); },
     "return min(1.0, b + s);"},
    {CanvasBlend::linear_burn, "linear_burn", [](double b, double s) { return std::max(0.0, b + s - 1.0); },
     "return max(0.0, b + s - 1.0);"},
    {CanvasBlend::subtract, "subtract", [](double b, double s) { return std::max(0.0, b - s); },
     "return max(0.0, b - s);"},
    {CanvasBlend::divide, "divide",
     [](double b, double s) { return s <= 0.0 ? (b > 0.0 ? 1.0 : 0.0) : std::min(1.0, b / s); },
     "return s <= 0.0 ? (b > 0.0 ? 1.0 : 0.0) : min(1.0, b / s);"},
    {CanvasBlend::vivid_light, "vivid_light",
     [](double b, double s) { return s <= 0.5 ? colorBurn(b, 2.0 * s) : colorDodge(b, 2.0 * s - 1.0); },
     "float t = s <= 0.5 ? 2.0 * s : 2.0 * s - 1.0;\n"
     "    if (s <= 0.5) return b >= 1.0 ? 1.0 : (t <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0 - b) / t));\n"
     "    return b <= 0.0 ? 0.0 : (t >= 1.0 ? 1.0 : min(1.0, b / (1.0 - t)));"},
    {CanvasBlend::linear_light, "linear_light",
     [](double b, double s) { return std::clamp(b + 2.0 * s - 1.0, 0.0, 1.0); },
     "return clamp(b + 2.0 * s - 1.0, 0.0, 1.0);"},
    {CanvasBlend::pin_light, "pin_light",
     [](double b, double s) { return s <= 0.5 ? std::min(b, 2.0 * s) : std::max(b, 2.0 * s - 1.0); },
     "return s <= 0.5 ? min(b, 2.0 * s) : max(b, 2.0 * s - 1.0);"},
    {CanvasBlend::hard_mix, "hard_mix", [](double b, double s) { return b + s >= 1.0 ? 1.0 : 0.0; },
     "return b + s >= 1.0 ? 1.0 : 0.0;"},
    {CanvasBlend::darker_color, "darker_color", nullptr, "return canvasLum(s) < canvasLum(b) ? s : b;"},
    {CanvasBlend::lighter_color, "lighter_color", nullptr, "return canvasLum(s) > canvasLum(b) ? s : b;"},
};

const BlendRow &blendRow(CanvasBlend blend) {
    const auto &row = blend_rows[static_cast<std::size_t>(blend)];
    if (row.blend != blend) throw std::logic_error("canvas blend table out of order");
    return row;
}

// The non-separable helpers of the W3C spec (Lum, ClipColor, SetLum, Sat, SetSat).
double lum(const CanvasRgb &c) { return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2]; }

CanvasRgb clipColor(CanvasRgb c) {
    const auto l = lum(c);
    const auto n = std::min({c[0], c[1], c[2]});
    const auto x = std::max({c[0], c[1], c[2]});
    for (auto &v : c) {
        if (n < 0.0) v = l + (v - l) * l / (l - n);
    }
    for (auto &v : c) {
        if (x > 1.0) v = l + (v - l) * (1.0 - l) / (x - l);
        v = std::clamp(v, 0.0, 1.0);
    }
    return c;
}

CanvasRgb setLum(const CanvasRgb &c, double l) {
    const auto d = l - lum(c);
    return clipColor({c[0] + d, c[1] + d, c[2] + d});
}

double sat(const CanvasRgb &c) { return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}); }

CanvasRgb setSat(const CanvasRgb &c, double s) {
    const auto n = std::min({c[0], c[1], c[2]});
    const auto x = std::max({c[0], c[1], c[2]});
    if (!(x > n)) return {0.0, 0.0, 0.0};
    return {(c[0] - n) * s / (x - n), (c[1] - n) * s / (x - n), (c[2] - n) * s / (x - n)};
}

const char *const non_separable_glsl = R"glsl(float canvasLum(vec3 c) { return dot(c, vec3(0.3, 0.59, 0.11)); }

vec3 canvasClipColor(vec3 c) {
    float l = canvasLum(c);
    float n = min(min(c.r, c.g), c.b);
    float x = max(max(c.r, c.g), c.b);
    if (n < 0.0) c = l + (c - l) * l / (l - n);
    if (x > 1.0) c = l + (c - l) * (1.0 - l) / (x - l);
    return clamp(c, 0.0, 1.0);
}

vec3 canvasSetLum(vec3 c, float l) { return canvasClipColor(c + (l - canvasLum(c))); }

float canvasSat(vec3 c) { return max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b); }

vec3 canvasSetSat(vec3 c, float s) {
    float n = min(min(c.r, c.g), c.b);
    float x = max(max(c.r, c.g), c.b);
    return x > n ? (c - n) * s / (x - n) : vec3(0.0);
}

)glsl";

// --- colour filters -----------------------------------------------------

const std::vector<std::pair<CanvasFilterKind, std::string>> &filterOps() {
    static const std::vector<std::pair<CanvasFilterKind, std::string>> ops{
        {CanvasFilterKind::invert, "filter.invert"},
        {CanvasFilterKind::brightness_contrast, "filter.brightness_contrast"},
        {CanvasFilterKind::levels, "filter.levels"},
        {CanvasFilterKind::hue_saturation, "filter.hue_saturation"},
        {CanvasFilterKind::gradient_map, "filter.gradient_map"},
        {CanvasFilterKind::color_overlay, "filter.color_overlay"}};
    return ops;
}

[[noreturn]] void unsupportedParameter(const Json &node, const std::string &parameter) {
    throw GraphRegistryError("canvas.filter_parameter_unsupported@1", {{"node", nodeId(node)}, {"parameter", parameter}});
}

double number(const Json &value, const std::string &what) {
    if (!value.is_number()) throw std::invalid_argument(what + " must be a number");
    const auto result = value.get<double>();
    if (!std::isfinite(result)) throw std::invalid_argument(what + " must be finite");
    return result;
}

double numberOr(const Json &object, const char *key, double fallback, const std::string &what) {
    const auto found = object.find(key);
    return found == object.end() ? fallback : number(*found, what + " " + key);
}

void onlyKeys(const Json &object, std::initializer_list<const char *> keys, const std::string &what) {
    for (const auto &[key, value] : object.items()) {
        (void)value;
        if (std::none_of(keys.begin(), keys.end(), [&](const char *known) { return key == known; }))
            throw std::invalid_argument(what + " does not take " + key);
    }
}

std::array<double, 4> colorParameter(const Json &parameters, const std::string &what) {
    const auto found = parameters.find("color");
    if (found == parameters.end()) throw std::invalid_argument(what + " requires a color");
    if (!found->is_array() || (found->size() != 3 && found->size() != 4))
        throw std::invalid_argument(what + " color must be [r, g, b] or [r, g, b, a]");
    std::array<double, 4> rgba{0.0, 0.0, 0.0, 1.0};
    for (std::size_t i = 0; i < found->size(); ++i) rgba[i] = number(found->at(i), what + " color");
    return rgba;
}

// A natural cubic spline through four points, at t.
double naturalSpline(const std::array<double, 4> &x, const std::array<double, 4> &y, double t) {
    const double h0 = x[1] - x[0], h1 = x[2] - x[1], h2 = x[3] - x[2];
    const double r1 = 6.0 * ((y[2] - y[1]) / h1 - (y[1] - y[0]) / h0);
    const double r2 = 6.0 * ((y[3] - y[2]) / h2 - (y[2] - y[1]) / h1);
    // [2(h0+h1) h1; h1 2(h1+h2)] [M1 M2] = [r1 r2], M0 = M3 = 0.
    const double a = 2.0 * (h0 + h1), b = h1, d = 2.0 * (h1 + h2);
    const double det = a * d - b * b;
    const std::array<double, 4> m{0.0, (r1 * d - b * r2) / det, (a * r2 - b * r1) / det, 0.0};
    std::size_t i = t < x[1] ? 0 : (t < x[2] ? 1 : 2);
    const double h = x[i + 1] - x[i];
    const double u = x[i + 1] - t, v = t - x[i];
    return m[i] * u * u * u / (6.0 * h) + m[i + 1] * v * v * v / (6.0 * h) + (y[i] / h - m[i] * h / 6.0) * u +
           (y[i + 1] / h - m[i + 1] * h / 6.0) * v;
}

// Photoshop's (non-legacy) brightness / contrast as psd-tools reconstructs it
// (composite/adjustments.py apply_brightnesscontrast): a contrast spline, a
// brightness curve, the one turned 45 degrees onto the other, as 256 values.
std::vector<double> brightnessContrastCurve(double brightness, double contrast) {
    const double b = brightness / 150.0, c = contrast / 100.0;
    const double delta = 25.0 / 255.0;
    const std::array<double, 4> x{0.0, 63.0 / 255.0, 191.0 / 255.0, 1.0};
    const std::array<double, 4> y{0.0, x[1] - c * delta, x[2] + c * delta, 1.0};
    const auto magnitude = std::abs(b);
    std::vector<double> xr(256), yr(256);
    for (std::size_t i = 0; i < 256; ++i) {
        const double t = i / 255.0;
        const double h = 0.5 * (magnitude * (1.65 * std::pow(t, 0.35) - std::pow(t, 10.0)) +
                                (1.0 - magnitude) * (1.96 * std::pow(t, 0.4) + std::pow(t, 4.0)) + std::pow(t, 1.25));
        const double lift = b * t * (1.0 - t) * h;
        xr[i] = t - lift;
        yr[i] = naturalSpline(x, y, t) + lift;
    }
    // np.interp(t, xr, yr), clipped.
    std::vector<double> curve(256);
    for (std::size_t i = 0; i < 256; ++i) {
        const double t = i / 255.0;
        double value;
        if (t <= xr.front()) value = yr.front();
        else if (t >= xr.back()) value = yr.back();
        else {
            std::size_t j = 0;
            while (j + 2 < xr.size() && !(t < xr[j + 1])) ++j;
            const double span = xr[j + 1] - xr[j];
            value = span > 0.0 ? yr[j] + (yr[j + 1] - yr[j]) * (t - xr[j]) / span : yr[j];
        }
        curve[i] = std::clamp(value, 0.0, 1.0);
    }
    return curve;
}

// One levels record, as psd-canvas writes it: 0..255 and a gamma.
void levelRecord(const Json &record, const std::string &what, std::vector<double> &out) {
    if (!record.is_object()) throw std::invalid_argument(what + " must be an object");
    onlyKeys(record, {"input_floor", "input_ceiling", "gamma", "output_floor", "output_ceiling"}, what);
    out.push_back(numberOr(record, "input_floor", 0.0, what) / 255.0);
    out.push_back(numberOr(record, "input_ceiling", 255.0, what) / 255.0);
    // Photoshop clamps the gamma into 0.01 .. 9.99.
    out.push_back(std::clamp(numberOr(record, "gamma", 1.0, what), 0.01, 9.99));
    out.push_back(numberOr(record, "output_floor", 0.0, what) / 255.0);
    out.push_back(numberOr(record, "output_ceiling", 255.0, what) / 255.0);
}

double level(double v, const double *r) {
    const auto scale = std::abs(r[1] - r[0]) > 1e-9 ? r[1] - r[0] : 1.0;
    auto o = std::clamp((v - r[0]) / scale, 0.0, 1.0);
    o = std::clamp(std::pow(o, 1.0 / r[2]), 0.0, 1.0);
    return std::clamp(o * (r[4] - r[3]) + r[3], 0.0, 1.0);
}

double curve(double v, const std::vector<double> &table, std::size_t first) {
    const auto t = std::clamp(v, 0.0, 1.0) * 255.0;
    const auto i = std::min(std::floor(t), 254.0);
    const auto k = first + static_cast<std::size_t>(i);
    return table[k] + (table[k + 1] - table[k]) * (t - i);
}

// psd-tools' rgb2hsl / hsl2rgb (composite/blend.py).
CanvasRgb rgbToHsl(const CanvasRgb &c) {
    const auto n = std::min({c[0], c[1], c[2]});
    const auto x = std::max({c[0], c[1], c[2]});
    const auto delta = x - n;
    const auto l = 0.5 * (x + n);
    double s = 0.0, h = 0.0;
    if (delta > 1e-9) {
        s = delta / (1.0 - std::abs(2.0 * l - 1.0));
        if (x == c[2]) h = (c[0] - c[1]) / delta + 4.0;
        else if (x == c[1]) h = (c[2] - c[0]) / delta + 2.0;
        else {
            h = (c[1] - c[2]) / delta;
            h -= 6.0 * std::floor(h / 6.0);
        }
        h /= 6.0;
    }
    return {h, s, l};
}

CanvasRgb hslToRgb(const CanvasRgb &hsl) {
    const auto chroma = (1.0 - std::abs(2.0 * hsl[2] - 1.0)) * hsl[1];
    const auto h = hsl[0] * 6.0;
    const auto x = chroma * (1.0 - std::abs(h - 2.0 * std::floor(h / 2.0) - 1.0));
    CanvasRgb rgb;
    if (h < 1.0) rgb = {chroma, x, 0.0};
    else if (h < 2.0) rgb = {x, chroma, 0.0};
    else if (h < 3.0) rgb = {0.0, chroma, x};
    else if (h < 4.0) rgb = {0.0, x, chroma};
    else if (h < 5.0) rgb = {x, 0.0, chroma};
    else rgb = {chroma, 0.0, x};
    const auto m = hsl[2] - chroma / 2.0;
    return {rgb[0] + m, rgb[1] + m, rgb[2] + m};
}

double lightness(double v, double l) {
    if (l > 0.0) return std::clamp(v * (1.0 - l) + l, 0.0, 1.0);
    if (l < 0.0) return std::clamp(v * (1.0 + l), 0.0, 1.0);
    return v;
}

double saturate(double s, double k) {
    return std::clamp(k > 0.0 ? s / (1.0 - k + 0.01) : s * (1.0 + k), 0.0, 1.0);
}

double decode(double c, bool srgb) {
    c = std::clamp(c, 0.0, 1.0);
    if (srgb) return c;
    return c > 0.04045 ? std::pow((c + 0.055) / 1.055, 2.4) : c / 12.92;
}

const char *const filter_glsl[] = {
    // invert
    R"glsl(vec3 canvasFilter0(vec3 c, uint r) { return 1.0 - c; }
)glsl",
    // brightness_contrast
    R"glsl(float canvasCurve(float v, uint r) {
    float t = clamp(v, 0.0, 1.0) * 255.0;
    float i = min(floor(t), 254.0);
    uint k = r + uint(i);
    return mix(pelican_load_bake(k), pelican_load_bake(k + 1u), t - i);
}

vec3 canvasFilter1(vec3 c, uint r) { return vec3(canvasCurve(c.r, r), canvasCurve(c.g, r), canvasCurve(c.b, r)); }
)glsl",
    // levels
    R"glsl(float canvasLevel(float v, uint r) {
    float lo = pelican_load_bake(r), hi = pelican_load_bake(r + 1u);
    float scale = abs(hi - lo) > 1e-9 ? hi - lo : 1.0;
    float o = clamp((v - lo) / scale, 0.0, 1.0);
    o = clamp(pow(o, 1.0 / pelican_load_bake(r + 2u)), 0.0, 1.0);
    float out_lo = pelican_load_bake(r + 3u);
    return clamp(o * (pelican_load_bake(r + 4u) - out_lo) + out_lo, 0.0, 1.0);
}

vec3 canvasFilter2(vec3 c, uint r) {
    c = vec3(canvasLevel(c.r, r + 5u), canvasLevel(c.g, r + 10u), canvasLevel(c.b, r + 15u));
    return vec3(canvasLevel(c.r, r), canvasLevel(c.g, r), canvasLevel(c.b, r));
}
)glsl",
    // hue_saturation
    R"glsl(vec3 canvasRgbToHsl(vec3 c) {
    float n = min(min(c.r, c.g), c.b);
    float x = max(max(c.r, c.g), c.b);
    float delta = x - n;
    float l = 0.5 * (x + n);
    float s = 0.0, h = 0.0;
    if (delta > 1e-9) {
        s = delta / (1.0 - abs(2.0 * l - 1.0));
        if (x == c.b) h = (c.r - c.g) / delta + 4.0;
        else if (x == c.g) h = (c.b - c.r) / delta + 2.0;
        else h = mod((c.g - c.b) / delta, 6.0);
        h /= 6.0;
    }
    return vec3(h, s, l);
}

vec3 canvasHslToRgb(vec3 hsl) {
    float chroma = (1.0 - abs(2.0 * hsl.z - 1.0)) * hsl.y;
    float h = hsl.x * 6.0;
    float x = chroma * (1.0 - abs(mod(h, 2.0) - 1.0));
    vec3 rgb = h < 1.0 ? vec3(chroma, x, 0.0)
             : h < 2.0 ? vec3(x, chroma, 0.0)
             : h < 3.0 ? vec3(0.0, chroma, x)
             : h < 4.0 ? vec3(0.0, x, chroma)
             : h < 5.0 ? vec3(x, 0.0, chroma)
                       : vec3(chroma, 0.0, x);
    return rgb + (hsl.z - chroma / 2.0);
}

vec3 canvasLightness(vec3 c, float l) {
    if (l > 0.0) return clamp(c * (1.0 - l) + l, 0.0, 1.0);
    if (l < 0.0) return clamp(c * (1.0 + l), 0.0, 1.0);
    return c;
}

vec3 canvasFilter3(vec3 c, uint r) {
    float mode = pelican_load_bake(r);
    if (mode > 1.5) return c;
    if (mode > 0.5) {
        vec3 hsl = canvasRgbToHsl(c);
        hsl.x = fract(pelican_load_bake(r + 4u));
        hsl.y = pelican_load_bake(r + 5u);
        hsl.z = canvasLightness(vec3(hsl.z), pelican_load_bake(r + 6u)).x;
        return canvasHslToRgb(hsl);
    }
    vec3 hsl = canvasRgbToHsl(canvasLightness(c, pelican_load_bake(r + 3u)));
    hsl.x = fract(hsl.x + pelican_load_bake(r + 1u));
    float k = pelican_load_bake(r + 2u);
    hsl.y = clamp(k > 0.0 ? hsl.y / (1.0 - k + 0.01) : hsl.y * (1.0 + k), 0.0, 1.0);
    return canvasHslToRgb(hsl);
}
)glsl",
    // gradient_map
    R"glsl(vec3 canvasFilter4(vec3 c, uint r) {
    float t = clamp(dot(c, vec3(0.3, 0.59, 0.11)), 0.0, 1.0);
    uint count = uint(pelican_load_bake(r) + 0.5);
    uint s = r + 1u;
    vec3 color = canvasShapeColor(vec3(pelican_load_bake(s + 1u), pelican_load_bake(s + 2u), pelican_load_bake(s + 3u)));
    for (uint k = 1u; k < count; ++k) {
        uint e = r + 1u + 4u * k;
        float p0 = pelican_load_bake(e - 4u), p1 = pelican_load_bake(e);
        if (t <= p0) break;
        vec3 next = canvasShapeColor(vec3(pelican_load_bake(e + 1u), pelican_load_bake(e + 2u), pelican_load_bake(e + 3u)));
        color = t >= p1 ? next : mix(color, next, (t - p0) / max(p1 - p0, 1e-9));
    }
    return color;
}
)glsl",
    // color_overlay
    R"glsl(vec3 canvasFilter5(vec3 c, uint r) {
    vec3 color = canvasShapeColor(vec3(pelican_load_bake(r), pelican_load_bake(r + 1u), pelican_load_bake(r + 2u)));
    return mix(c, color, clamp(pelican_load_bake(r + 3u), 0.0, 1.0));
}
)glsl",
};

} // namespace

const std::vector<std::string> &canvasBlendNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> result;
        for (const auto &row : blend_rows) result.emplace_back(row.name);
        return result;
    }();
    return names;
}

std::optional<CanvasBlend> canvasBlendFromName(std::string_view name) {
    for (const auto &row : blend_rows)
        if (name == row.name) return row.blend;
    return std::nullopt;
}

std::string_view canvasBlendName(CanvasBlend blend) { return blendRow(blend).name; }

CanvasRgb canvasBlendColor(CanvasBlend blend, const CanvasRgb &b, const CanvasRgb &s) {
    const auto &row = blendRow(blend);
    if (row.channel != nullptr) return {row.channel(b[0], s[0]), row.channel(b[1], s[1]), row.channel(b[2], s[2])};
    switch (blend) {
    case CanvasBlend::hue: return setLum(setSat(s, sat(b)), lum(b));
    case CanvasBlend::saturation: return setLum(setSat(b, sat(s)), lum(b));
    case CanvasBlend::color: return setLum(s, lum(b));
    case CanvasBlend::luminosity: return setLum(b, lum(s));
    case CanvasBlend::darker_color: return lum(s) < lum(b) ? s : b;
    case CanvasBlend::lighter_color: return lum(s) > lum(b) ? s : b;
    default: break;
    }
    throw std::logic_error("canvas blend has no function");
}

const std::vector<std::string> &canvasColorFilterOperations() {
    static const std::vector<std::string> ops = [] {
        std::vector<std::string> result;
        for (const auto &[kind, op] : filterOps()) result.push_back(op);
        return result;
    }();
    return ops;
}

bool isCanvasColorFilter(std::string_view op) {
    const auto &ops = canvasColorFilterOperations();
    return std::find(ops.begin(), ops.end(), op) != ops.end();
}

std::string_view canvasFilterOperation(CanvasFilterKind kind) {
    return filterOps().at(static_cast<std::size_t>(kind)).second;
}

std::size_t canvasFilterConstantCount(CanvasFilterKind kind) {
    switch (kind) {
    case CanvasFilterKind::invert: return 0;
    case CanvasFilterKind::brightness_contrast: return 256;
    case CanvasFilterKind::levels: return 20;
    case CanvasFilterKind::hue_saturation: return 7;
    case CanvasFilterKind::gradient_map: return 1 + 4 * canvas_gradient_stop_limit;
    case CanvasFilterKind::color_overlay: return 4;
    }
    throw std::logic_error("unknown canvas filter kind");
}

CanvasFilter parseCanvasFilter(const nlohmann::json &node) {
    const auto id = nodeId(node);
    const auto op = node.value("op", std::string{});
    CanvasFilter filter;
    filter.node = id;
    const auto found = std::find_if(filterOps().begin(), filterOps().end(),
                                    [&](const auto &entry) { return entry.second == op; });
    if (found == filterOps().end()) throw std::invalid_argument("not a canvas colour filter: " + op);
    filter.kind = found->first;
    // CV-2b takes the numbers as authored; a value edge is refused by name.
    if (const auto inputs = node.find("inputs"); inputs != node.end() && inputs->is_object())
        for (const auto &[port, edge] : inputs->items()) {
            (void)edge;
            if (port != "layer")
                throw GraphRegistryError("canvas.filter_edge_unsupported@1", {{"node", id}, {"port", port}});
        }
    static const Json empty = Json::object();
    const auto parameters_found = node.find("parameters");
    const auto &parameters =
        parameters_found != node.end() && parameters_found->is_object() ? *parameters_found : empty;
    const auto what = op + " " + id;
    auto &out = filter.constants;
    switch (filter.kind) {
    case CanvasFilterKind::invert:
        onlyKeys(parameters, {}, what);
        break;
    case CanvasFilterKind::brightness_contrast: {
        onlyKeys(parameters, {"brightness", "contrast", "legacy"}, what);
        if (const auto legacy = parameters.find("legacy"); legacy != parameters.end()) {
            if (!legacy->is_boolean()) throw std::invalid_argument(what + " legacy must be a boolean");
            if (legacy->get<bool>()) unsupportedParameter(node, "legacy");
        }
        out = brightnessContrastCurve(numberOr(parameters, "brightness", 0.0, what),
                                      numberOr(parameters, "contrast", 0.0, what));
        break;
    }
    case CanvasFilterKind::levels: {
        onlyKeys(parameters, {"input_floor", "input_ceiling", "gamma", "output_floor", "output_ceiling", "channels"},
                 what);
        auto master = parameters;
        master.erase("channels");
        levelRecord(master, what, out);
        const auto channels = parameters.find("channels");
        if (channels != parameters.end()) {
            if (!channels->is_object()) throw std::invalid_argument(what + " channels must be an object");
            onlyKeys(*channels, {"red", "green", "blue"}, what + " channels");
        }
        for (const auto *name : {"red", "green", "blue"}) {
            if (channels != parameters.end() && channels->contains(name))
                levelRecord(channels->at(name), what + " channels " + name, out);
            else
                out.insert(out.end(), {0.0, 1.0, 1.0, 0.0, 1.0});
        }
        break;
    }
    case CanvasFilterKind::hue_saturation: {
        onlyKeys(parameters, {"hue", "saturation", "lightness", "colorize", "ranges"}, what);
        if (parameters.contains("ranges")) unsupportedParameter(node, "ranges");
        const auto hue = numberOr(parameters, "hue", 0.0, what) / 360.0;
        const auto saturation = numberOr(parameters, "saturation", 0.0, what) / 100.0;
        const auto light = numberOr(parameters, "lightness", 0.0, what) / 100.0;
        double mode = hue == 0.0 && saturation == 0.0 && light == 0.0 ? 2.0 : 0.0;
        std::array<double, 3> colorize{0.0, 0.0, 0.0};
        if (const auto found_colorize = parameters.find("colorize"); found_colorize != parameters.end()) {
            if (!found_colorize->is_object()) throw std::invalid_argument(what + " colorize must be an object");
            onlyKeys(*found_colorize, {"hue", "saturation", "lightness"}, what + " colorize");
            colorize = {numberOr(*found_colorize, "hue", 0.0, what) / 360.0,
                        numberOr(*found_colorize, "saturation", 0.0, what) / 100.0,
                        numberOr(*found_colorize, "lightness", 0.0, what) / 100.0};
            // Colorize replaces the master; all zero, it does nothing (psd-tools).
            mode = colorize == std::array<double, 3>{0.0, 0.0, 0.0} ? 2.0 : 1.0;
        }
        out = {mode, hue, saturation, light, colorize[0], colorize[1], colorize[2]};
        break;
    }
    case CanvasFilterKind::gradient_map: {
        onlyKeys(parameters, {"stops", "opacity_stops", "reversed", "dithered"}, what);
        const auto stops = parameters.find("stops");
        if (stops == parameters.end() || !stops->is_array() || stops->empty())
            throw std::invalid_argument(what + " requires stops");
        if (stops->size() > canvas_gradient_stop_limit) unsupportedParameter(node, "stops");
        if (const auto dithered = parameters.find("dithered"); dithered != parameters.end() && *dithered != false)
            unsupportedParameter(node, "dithered");
        if (const auto opacity = parameters.find("opacity_stops"); opacity != parameters.end()) {
            if (!opacity->is_array()) throw std::invalid_argument(what + " opacity_stops must be an array");
            for (const auto &stop : *opacity)
                if (!stop.is_object() || numberOr(stop, "opacity", 1.0, what) != 1.0 ||
                    numberOr(stop, "midpoint", 0.5, what) != 0.5)
                    unsupportedParameter(node, "opacity_stops");
        }
        bool reversed = false;
        if (const auto found_reversed = parameters.find("reversed"); found_reversed != parameters.end()) {
            if (!found_reversed->is_boolean()) throw std::invalid_argument(what + " reversed must be a boolean");
            reversed = found_reversed->get<bool>();
        }
        std::vector<std::array<double, 4>> parsed;
        for (const auto &stop : *stops) {
            if (!stop.is_object()) throw std::invalid_argument(what + " stop must be an object");
            onlyKeys(stop, {"position", "color", "midpoint"}, what + " stop");
            if (numberOr(stop, "midpoint", 0.5, what) != 0.5) unsupportedParameter(node, "midpoint");
            const auto color = colorParameter(stop, what + " stop");
            auto position = std::clamp(numberOr(stop, "position", 0.0, what), 0.0, 1.0);
            if (reversed) position = 1.0 - position;
            parsed.push_back({position, color[0], color[1], color[2]});
        }
        std::stable_sort(parsed.begin(), parsed.end(), [](const auto &a, const auto &b) { return a[0] < b[0]; });
        out.push_back(static_cast<double>(parsed.size()));
        for (const auto &stop : parsed) out.insert(out.end(), stop.begin(), stop.end());
        out.resize(canvasFilterConstantCount(filter.kind), 0.0);
        break;
    }
    case CanvasFilterKind::color_overlay: {
        onlyKeys(parameters, {"color"}, what);
        const auto color = colorParameter(parameters, what);
        out.assign(color.begin(), color.end());
        break;
    }
    }
    return filter;
}

CanvasRgb applyCanvasFilter(const CanvasFilter &filter, const CanvasRgb &c, bool srgb) {
    const auto &k = filter.constants;
    switch (filter.kind) {
    case CanvasFilterKind::invert: return {1.0 - c[0], 1.0 - c[1], 1.0 - c[2]};
    case CanvasFilterKind::brightness_contrast: return {curve(c[0], k, 0), curve(c[1], k, 0), curve(c[2], k, 0)};
    case CanvasFilterKind::levels: {
        const CanvasRgb channel{level(c[0], &k[5]), level(c[1], &k[10]), level(c[2], &k[15])};
        return {level(channel[0], &k[0]), level(channel[1], &k[0]), level(channel[2], &k[0])};
    }
    case CanvasFilterKind::hue_saturation: {
        if (k[0] > 1.5) return c;
        if (k[0] > 0.5) {
            auto hsl = rgbToHsl(c);
            hsl[0] = k[4] - std::floor(k[4]);
            hsl[1] = k[5];
            hsl[2] = lightness(hsl[2], k[6]);
            return hslToRgb(hsl);
        }
        auto hsl = rgbToHsl({lightness(c[0], k[3]), lightness(c[1], k[3]), lightness(c[2], k[3])});
        hsl[0] += k[1];
        hsl[0] -= std::floor(hsl[0]);
        hsl[1] = saturate(hsl[1], k[2]);
        return hslToRgb(hsl);
    }
    case CanvasFilterKind::gradient_map: {
        const auto t = std::clamp(lum(c), 0.0, 1.0);
        const auto count = static_cast<std::size_t>(k[0] + 0.5);
        const auto stop = [&](std::size_t i) {
            return CanvasRgb{decode(k[2 + 4 * i], srgb), decode(k[3 + 4 * i], srgb), decode(k[4 + 4 * i], srgb)};
        };
        auto color = stop(0);
        for (std::size_t i = 1; i < count; ++i) {
            const auto p0 = k[1 + 4 * (i - 1)], p1 = k[1 + 4 * i];
            if (t <= p0) break;
            const auto next = stop(i);
            if (t >= p1) color = next;
            else {
                const auto f = (t - p0) / std::max(p1 - p0, 1e-9);
                for (std::size_t j = 0; j < 3; ++j) color[j] += (next[j] - color[j]) * f;
            }
        }
        return color;
    }
    case CanvasFilterKind::color_overlay: {
        const auto a = std::clamp(k[3], 0.0, 1.0);
        CanvasRgb result;
        for (std::size_t j = 0; j < 3; ++j) result[j] = c[j] + (decode(k[j], srgb) - c[j]) * a;
        return result;
    }
    }
    throw std::logic_error("unknown canvas filter kind");
}

std::string canvasBlendGlsl(const std::set<CanvasBlend> &blends) {
    std::string text;
    if (std::any_of(blends.begin(), blends.end(), [](CanvasBlend blend) { return blendRow(blend).channel == nullptr; }))
        text += non_separable_glsl;
    for (const auto blend : blends) {
        const auto &row = blendRow(blend);
        const auto index = std::to_string(static_cast<int>(blend));
        text += "// " + std::string{row.name} + "\n";
        if (row.channel != nullptr) {
            text += "float canvasBlendChannel" + index + "(float b, float s) {\n    " + row.glsl + "\n}\n\n";
            text += "vec3 canvasBlend" + index + "(vec3 b, vec3 s) {\n    return vec3(canvasBlendChannel" + index +
                    "(b.r, s.r), canvasBlendChannel" + index + "(b.g, s.g), canvasBlendChannel" + index +
                    "(b.b, s.b));\n}\n\n";
        } else {
            text += "vec3 canvasBlend" + index + "(vec3 b, vec3 s) {\n    " + row.glsl + "\n}\n\n";
        }
    }
    return text;
}

std::string canvasFilterGlsl(const std::set<CanvasFilterKind> &kinds) {
    std::string text;
    for (const auto kind : kinds) {
        text += "// " + std::string{canvasFilterOperation(kind)} + "\n";
        text += filter_glsl[static_cast<std::size_t>(kind)];
        text += "\n";
    }
    return text;
}

} // namespace Pelican
