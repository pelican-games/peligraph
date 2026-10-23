#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// The producer endpoint of a value edge that drives a parameter.
struct CanvasValueSource {
    std::string node;
    std::string port;
    auto operator<=>(const CanvasValueSource &) const = default;
};

struct CanvasParameter {
    // The authored number; for a parameter an edge drives, the number the
    // parameters hold (0 when they hold none), which is the frame table's
    // first value.
    double value = 0.0;
    std::optional<CanvasValueSource> edge;
};

// CV-2b: blend modes and colour filters, the one table both the generated
// fragment shader and the CPU reference (canvasreference.hpp) compute from.
//
// A blend mode is B(Cb, Cs) of W3C Compositing and Blending Level 1 on
// straight colours in 0..1 of the stack's blend space (sRGB values or
// linear), and the item is composited source-over with it:
//   Cr = (1 - ab) Cs + ab B(Cb, Cs)
// The W3C modes are normal .. luminosity; the ones Photoshop has and W3C does
// not (linear_dodge .. lighter_color) follow Photoshop's definitions as
// psd-tools writes them (composite/blend.py).
enum class CanvasBlend : std::uint8_t {
    normal,
    multiply,
    screen,
    overlay,
    darken,
    lighten,
    color_dodge,
    color_burn,
    hard_light,
    soft_light,
    difference,
    exclusion,
    hue,
    saturation,
    color,
    luminosity,
    linear_dodge,
    linear_burn,
    subtract,
    divide,
    vivid_light,
    linear_light,
    pin_light,
    hard_mix,
    darker_color,
    lighter_color,
};

// The blend names a canvas.stack layer takes, in enum order (the list an
// editor's blend combo offers). Anything else is canvas.blend_unsupported@1.
const std::vector<std::string> &canvasBlendNames();
std::optional<CanvasBlend> canvasBlendFromName(std::string_view name);
std::string_view canvasBlendName(CanvasBlend blend);

using CanvasRgb = std::array<double, 3>;

// B(Cb, Cs).
CanvasRgb canvasBlendColor(CanvasBlend blend, const CanvasRgb &backdrop, const CanvasRgb &source);

// A colour filter (layer -> layer, per pixel, on the straight colour in the
// blend space; the alpha stays). Its numbers, as the item's tables hold them
// after the item's parameters (canvasFilterConstantCount of them):
//   invert:              none
//   brightness_contrast: 256 values, the curve at x = i / 255
//   levels:              master, red, green, blue: input_floor input_ceiling
//                        gamma output_floor output_ceiling (as authored: 0..255,
//                        the gamma clamped into 0.01..9.99 where it is read)
//   hue_saturation:      mode (0 master, 1 colorize, 2 none) hue saturation
//                        lightness (master; degrees, -100..100 as authored)
//                        then colorize's hue saturation lightness; colorize
//                        all zero does nothing
//   gradient_map:        count, reversed (0 / 1), then position r g b per stop
//                        (position as authored, clamped into 0..1 and reversed
//                        where it is read; colours sRGB), up to 16, in the
//                        order of the authored positions
//   color_overlay:       r g b a (sRGB; a mixes the colour in)
// CV-3a: the numbers are in the units the parameters are written in, so a
// value edge (the frame table) can drive them: each number names the port
// that may (`ports`, empty for none) -- levels' `input_floor` ...
// `output_ceiling` and `channels.<red|green|blue>.<the same>`,
// hue_saturation's `hue` `saturation` `lightness` and `colorize.<the same>`,
// gradient_map's `stops.<k>.position` and `stops.<k>.color.<r|g|b>` (k the
// authored stop), color_overlay's `color.<r|g|b|a>`. brightness_contrast's
// curve is baked on the CPU, so nothing drives it.
enum class CanvasFilterKind : std::uint8_t { invert, brightness_contrast, levels, hue_saturation, gradient_map,
                                             color_overlay };

inline constexpr std::size_t canvas_gradient_stop_limit = 16;

struct CanvasFilter {
    CanvasFilterKind kind = CanvasFilterKind::invert;
    std::vector<double> constants;
    // CV-3a: per number, the port that may drive it ("" for none), and the
    // edge that does (the lowering fills these in).
    std::vector<std::string> ports;
    std::vector<std::optional<CanvasValueSource>> edges;
    // The authored filter node.
    std::string node;
};

// The colour filter ops the lowering implements (filter.<kind>).
const std::vector<std::string> &canvasColorFilterOperations();
bool isCanvasColorFilter(std::string_view op);
std::string_view canvasFilterOperation(CanvasFilterKind kind);
std::size_t canvasFilterConstantCount(CanvasFilterKind kind);

// CV-3a: the neighbourhood filters -- filter.blur, filter.drop_shadow and
// filter.outer_glow, which read around each pixel (canvaslowering.hpp draws
// them) -- and every filter op the lowering implements, the colour ones first.
const std::vector<std::string> &canvasNeighbourhoodFilterOperations();
bool isCanvasNeighbourhoodFilter(std::string_view op);
const std::vector<std::string> &canvasFilterOperations();
bool isCanvasFilter(std::string_view op);

// The filter a filter.* node asks for. Named failures:
//   canvas.filter_edge_unsupported@1      {node, port}       a value edge into a port no number of the
//                                                            filter takes (brightness_contrast's
//                                                            brightness and contrast: its curve is baked)
//   canvas.filter_parameter_unsupported@1 {node, parameter}  a parameter psd-canvas writes that the
//                                                            filter does not implement (hue_saturation
//                                                            ranges, gradient_map midpoint / opacity_stops /
//                                                            dithered, brightness_contrast legacy)
// The edges themselves are the lowering's to read (CanvasFilter::edges stays empty).
CanvasFilter parseCanvasFilter(const nlohmann::json &node);

// The filter on a straight colour; `srgb` is the blend space (the sRGB
// colours a filter names are decoded into a linear one). The second form
// takes the numbers as the frame reads them (a driven one at its value).
CanvasRgb applyCanvasFilter(const CanvasFilter &filter, const CanvasRgb &color, bool srgb);
CanvasRgb applyCanvasFilter(CanvasFilterKind kind, const std::vector<double> &numbers, const CanvasRgb &color,
                            bool srgb);

// GLSL: `vec3 canvasBlend<index>(vec3 b, vec3 s)` for each mode, index its
// enum value, with the helpers they need.
std::string canvasBlendGlsl(const std::set<CanvasBlend> &blends);
// GLSL: `vec3 canvasFilter<index>(vec3 c, mat4 it, uint r)` for each kind,
// reading its numbers as canvasParam(it, r), canvasParam(it, r + 1u), ...
// (the item's parameter numbering, a driven number from the frame table).
// Needs canvasShapeColor and canvasParam.
std::string canvasFilterGlsl(const std::set<CanvasFilterKind> &kinds);

// --- CV-3a: the neighbourhood filters' arithmetic ---------------------------
//
// One formula on both sides: the generated shaders (canvasEffectGlsl) and the
// CPU reference compute the same, in float and in double.
//
// filter.blur's radius r is Photoshop's Gaussian Blur radius, taken as the
// standard deviation: sigma = r (stack pixels). filter.drop_shadow's and
// filter.outer_glow's radius S is a layer style's Size, the distance the
// effect reaches, and spread s its Spread (0..1): the layer's alpha is
// spread by s S, then blurred with sigma = (1 - s) S / 3, so it fades out
// over the rest of S.

// The standard normal CDF (Abramowitz & Stegun 7.1.26 for erf, |error| < 1.5e-7).
double canvasNormalCdf(double x);
// Its inverse for p in (0, 1) (Acklam's rational approximation).
double canvasProbit(double p);
// A drop shadow's or a glow's coverage from a signed distance d (stack
// units, positive outside) whose change per pixel is aa: the outline spread
// by spread * radius, then faded with sigma = (1 - spread) radius / 3 (and
// at least the pixel's antialiasing).
double canvasEffectCoverage(double d, double aa, double radius, double spread);
// The same from an image instead of a distance: b is the layer's alpha
// blurred with sigma = radius / 3; the spread moves its edge outward by
// spread * radius and sharpens what is left (exact for a straight edge).
double canvasSpreadAlpha(double b, double radius, double spread);
// The blur of an image halved `levels` times (each a 2 x 2 average) and read
// back with a bilinear filter, in the halved image's pixels, for a total
// sigma of `sigma` (pixels before halving): what the halving and the read
// back blur already is taken off.
double canvasBlurLevelSigma(double sigma, std::uint32_t levels);
// GLSL of the same: canvasNormalCdf, canvasProbit, canvasEffectCoverage and
// canvasSpreadAlpha.
std::string canvasEffectGlsl();
// A Gaussian tap's weight before the taps are normalized: exp(-t^2 / (2
// sigma^2)), or 1 at t = 0 alone for no sigma (the blur passes' GLSL computes
// the same, canvaslowering.cpp).
double canvasBlurWeight(int t, double sigma);

} // namespace Pelican
