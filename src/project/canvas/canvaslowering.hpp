#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// CV-1: the pelican.canvas lowering. A pure function of the graph document:
// it reads no file and no clock, and the same document always lowers to the
// same bytes (document and shaders).
//
// Two stages share one intermediate form. buildCanvasIr walks every
// canvas.output down its layer stack and flattens it into the list of things
// to draw, bottom to top (a CanvasItem each). lowerCanvasNodes turns that list
// into nodes of the core vocabulary plus one generated vertex / fragment
// shader pair per output; the CPU reference (canvasreference.hpp) draws the
// same list.
//
// Named failures (GraphRegistryError):
//   canvas.in_loop_body@1          {node}                a canvas or shape op inside a graph.loop body
//   canvas.reserved_id@1           {id}                  an authored id the lowering would generate
//   canvas.blend_unsupported@1     {node, layer, blend}  any blend but "normal"
//   canvas.layer_input_missing@1   {node, layer, input}  a layer whose `input` no edge feeds
//   canvas.stack_cycle@1           {node, cycle}         stacks that contain each other
//   canvas.output_unconnected@1    {node}                a canvas.output without its layer edge
// `layer` names the layer by its `name`, or by its index when it has none.
// Wrong types and unknown ports are the graph's own checks.

enum class CanvasShape : std::uint8_t { rect, ellipse };
enum class CanvasPaint : std::uint8_t { fill, stroke };
enum class CanvasFit : std::uint8_t { contain, stretch };

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

// An item's kind is its shape times its paint:
//   0 rect_fill, 1 rect_stroke, 2 ellipse_fill, 3 ellipse_stroke.
std::uint32_t canvasItemKind(CanvasShape shape, CanvasPaint paint);
std::string_view canvasItemKindName(std::uint32_t kind);

// An item's parameters, in table order: the shape's, then the paint's.
//   rect:    size.x size.y center.x center.y corner_radius rotation
//   ellipse: size.x size.y center.x center.y rotation
//   fill:    color.r color.g color.b color.a
//   stroke:  color.r color.g color.b color.a width align
// Rotation is in degrees, the colour sRGB with straight alpha, `align`
// 0 center / 1 inner / 2 outer.
std::size_t canvasShapeParameterCount(CanvasShape shape);
std::size_t canvasPaintParameterCount(CanvasPaint paint);

struct CanvasItem {
    CanvasShape shape = CanvasShape::rect;
    CanvasPaint paint = CanvasPaint::fill;
    std::vector<CanvasParameter> parameters;
    // The layers' opacities this item sits under, multiplied: the constant
    // ones here, the edge-driven ones as factors.
    double opacity = 1.0;
    std::vector<CanvasValueSource> opacity_edges;
    // The authored nodes that made it: the paint, the shape, then each stack
    // from the innermost out.
    std::vector<std::string> nodes;
};

struct CanvasOutput {
    std::string id;
    // "" for the root; otherwise the definition the output lives in.
    std::string scope;
    // The canvas: the size of the stack the output reads, in canvas units
    // (pixels of that stack). Origin top left, +x right, +y down.
    std::array<double, 2> size{};
    CanvasFit fit = CanvasFit::contain;
    // The image: a fixed size, or extent_scale of the render extent.
    std::optional<std::array<std::uint32_t, 2>> image_size;
    double extent_scale = 1.0;
    std::string format;
    // Bottom to top.
    std::vector<CanvasItem> items;
};

struct CanvasIr {
    std::vector<CanvasOutput> outputs;
    // [{code, node}]: canvas.group_opacity_flattened when a nested stack's
    // opacity was multiplied into its items instead of applied to the group
    // drawn as one.
    nlohmann::json diagnostics = nlohmann::json::array();
};

// True when the document names a canvas or shape op anywhere.
bool documentHasCanvasNodes(const nlohmann::json &document);

CanvasIr buildCanvasIr(const nlohmann::json &document);

struct CanvasGeneratedSource {
    // `canvas_<output id>_<first 8 hex digits of the sources' digest>`; the
    // pass reads project://shaders/generated/<stem>, which the caller writes
    // as shaders/generated/<stem>.<stage>.
    std::string stem;
    // "vert" or "frag".
    std::string stage;
    std::string text;
};

struct CanvasLowering {
    // The lowered document. Without any canvas op it is the input, untouched.
    nlohmann::json document;
    bool changed = false;
    std::vector<CanvasGeneratedSource> generated_sources;
    // [{output, instance, kind, nodes}]: which authored nodes drew each
    // instance of each output's item table.
    nlohmann::json items = nlohmann::json::array();
    nlohmann::json diagnostics = nlohmann::json::array();
    CanvasIr ir;
};

// The names a canvas.output `<id>` lowers to. The raster pass keeps `<id>`
// itself, so its pass name is the author's node and its written image port is
// `image`.
std::string canvasImageNodeId(const std::string &output);
std::string canvasItemsNodeId(const std::string &output);
std::string canvasBakeNodeId(const std::string &output);
std::string canvasFrameNodeId(const std::string &output);
std::string canvasFrameSinkNodeId(const std::string &output, std::size_t index);

CanvasLowering lowerCanvasNodes(const nlohmann::json &document);

} // namespace Pelican
