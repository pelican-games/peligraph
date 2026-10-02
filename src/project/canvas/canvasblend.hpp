#pragma once

#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

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
// blend space; the alpha stays). Its numbers, as the bake table holds them
// after the item's parameters (canvasFilterConstantCount of them):
//   invert:              none
//   brightness_contrast: 256 values, the curve at x = i / 255
//   levels:              master, red, green, blue: input_floor input_ceiling
//                        gamma output_floor output_ceiling (0..1, gamma 1 is none)
//   hue_saturation:      mode (0 master, 1 colorize, 2 none) hue saturation
//                        lightness (master; hue in turns, the others in -1..1)
//                        then colorize's hue saturation lightness
//   gradient_map:        count, then position r g b per stop (sRGB), up to 16
//   color_overlay:       r g b a (sRGB; a mixes the colour in)
enum class CanvasFilterKind : std::uint8_t { invert, brightness_contrast, levels, hue_saturation, gradient_map,
                                             color_overlay };

inline constexpr std::size_t canvas_gradient_stop_limit = 16;

struct CanvasFilter {
    CanvasFilterKind kind = CanvasFilterKind::invert;
    std::vector<double> constants;
    // The authored filter node.
    std::string node;
};

// The filter ops the lowering implements (filter.<kind>).
const std::vector<std::string> &canvasColorFilterOperations();
bool isCanvasColorFilter(std::string_view op);
std::string_view canvasFilterOperation(CanvasFilterKind kind);
std::size_t canvasFilterConstantCount(CanvasFilterKind kind);

// The filter a filter.* node asks for. Named failures:
//   canvas.filter_edge_unsupported@1      {node, port}       a value edge into a filter parameter
//   canvas.filter_parameter_unsupported@1 {node, parameter}  a parameter psd-canvas writes that the
//                                                            filter does not implement (hue_saturation
//                                                            ranges, gradient_map midpoint / opacity_stops /
//                                                            dithered, brightness_contrast legacy)
CanvasFilter parseCanvasFilter(const nlohmann::json &node);

// The filter on a straight colour; `srgb` is the blend space (the sRGB
// colours a filter names are decoded into a linear one).
CanvasRgb applyCanvasFilter(const CanvasFilter &filter, const CanvasRgb &color, bool srgb);

// GLSL: `vec3 canvasBlend<index>(vec3 b, vec3 s)` for each mode, index its
// enum value, with the helpers they need.
std::string canvasBlendGlsl(const std::set<CanvasBlend> &blends);
// GLSL: `vec3 canvasFilter<index>(vec3 c, uint r)` for each kind, reading its
// numbers from bake entry r on. Needs canvasShapeColor and pelican_load_bake.
std::string canvasFilterGlsl(const std::set<CanvasFilterKind> &kinds);

} // namespace Pelican
