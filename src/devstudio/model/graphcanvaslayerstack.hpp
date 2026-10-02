#pragma once

#include "graphcanvasmodel.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// SG-1 (docs/design_canvas_2d.md 2, 4). pelican.canvas in the Graph tab: the
// domain its nodes are filed under, what the palette places for them, a
// canvas.stack's sockets (its `parameters.layers`), and the Layers panel's
// rows and gestures. Every gesture is one op list for edit_graph_document --
// the panel has no edit of its own -- built here from the scope's nodes as
// the canvas reads them.
//
// Qt-free; the model calls into it where an op of the canvas's needs more
// than the catalog says (a stack's ports follow its parameters).
namespace PelicanStudio {

// What a layer port takes and a stack hands on, and a geometry's.
inline constexpr std::string_view graphCanvasLayerType = "pelican.canvas.layer@1";
inline constexpr std::string_view graphCanvasGeometryType = "pelican.canvas.geometry@1";
// The empty socket under a stack's layers: a line dropped on it adds a layer
// on top (an element of `layers` and its input, one edit).
inline constexpr std::string_view graphCanvasStackAddPort = "+ layer";
// The stack the palette's Canvas places: a 1280 x 720 canvas, nothing on it.
inline constexpr std::array<double, 2> graphCanvasDefaultSize{1280.0, 720.0};

// canvas.*, shape.* and filter.*: the canvas domain (design 4).
[[nodiscard]] bool isGraphCanvasOperation(std::string_view op);
// The palette's words for an operation it files differently from its name:
// canvas.output is the Canvas (the output, its stack and the line between).
[[nodiscard]] std::optional<std::string> graphCanvasPaletteLabel(std::string_view op);

// A stack's input ports as the canvas draws them: per layer, bottom first,
// the layer and its `<input>.opacity`; then the empty `+ layer`.
[[nodiscard]] std::vector<GraphCanvasPort> graphCanvasStackPorts(const nlohmann::json &parameters);

// One row of the Layers panel: one element of a stack's `layers`.
struct GraphCanvasStackRow {
    // Its place in `layers` (0 is the bottom).
    std::size_t index = 0;
    std::string input;
    // The element's `name`, else the input.
    std::string name;
    bool visible = true;
    // 0..1; the element's `opacity`, 1 when it says none.
    double opacity = 1.0;
    // A value line drives `<input>.opacity`: the field shows the stored one.
    bool opacity_driven = false;
    // "rect", "ellipse", "group" (a canvas.stack) or "layer" (anything else,
    // or nothing connected).
    std::string kind = "layer";
    // "fill" or "stroke" for a paint, else empty.
    std::string paint;
    // The node on the layer's input, and the shape behind a paint.
    std::string source;
    std::string shape;

    bool operator==(const GraphCanvasStackRow &) const = default;
};

// The rows of one stack, top first (the reverse of `layers`). Empty when
// `stack` is no canvas.stack of `nodes`.
[[nodiscard]] std::vector<GraphCanvasStackRow> graphCanvasLayerRows(
    const std::vector<GraphCanvasNode> &nodes, std::string_view stack);
// "Rect", "Ellipse", "Group", "Layer"; a stroke's is "<shape> stroke".
[[nodiscard]] std::string graphCanvasLayerKindText(const GraphCanvasStackRow &row);

// The stack the Layers panel shows for a node picked in the Graph: the stack
// itself, the stack a canvas.output reads, or the stack that holds a picked
// paint or shape (a nested one before its parent). Empty for anything else.
[[nodiscard]] std::string graphCanvasStackFor(const std::vector<GraphCanvasNode> &nodes,
                                              std::string_view node);
// The row a picked node stands for in `stack` (its paint, or its shape), or
// nothing.
[[nodiscard]] std::optional<std::size_t> graphCanvasRowFor(
    const std::vector<GraphCanvasNode> &nodes, std::string_view stack, std::string_view node);

// --- Gestures. Each returns the op list of one edit, or an empty array when
// there is nothing to send (`scope` is the scope's id, "" for the root).

// The palette's Canvas: a canvas.stack {size, layers: []} and a canvas.output
// reading it, in one list (an output alone is refused, canvas.output_unconnected@1).
// The output is last: it is the node the gesture is about.
[[nodiscard]] nlohmann::json graphCanvasCompositionOps(const std::vector<GraphCanvasNode> &nodes,
                                                       std::string_view scope);
// What the palette places for one canvas operation: the Canvas for
// canvas.output, an empty 1280 x 720 stack, a shape at the middle of it, a
// paint with its colour. Empty for any other operation.
[[nodiscard]] nlohmann::json graphCanvasPaletteOps(const std::vector<GraphCanvasNode> &nodes,
                                                   std::string_view scope, std::string_view op);

// + Rect / + Ellipse: the shape at the stack's middle, a quarter of its size,
// its fill, and a layer on top reading it. `shape` is "rect" or "ellipse".
[[nodiscard]] nlohmann::json graphCanvasAddShapeLayerOps(const std::vector<GraphCanvasNode> &nodes,
                                                         std::string_view scope,
                                                         std::string_view stack,
                                                         std::string_view shape);
// + Group: an empty stack of the parent's size, as a layer on top.
[[nodiscard]] nlohmann::json graphCanvasAddGroupLayerOps(const std::vector<GraphCanvasNode> &nodes,
                                                         std::string_view scope,
                                                         std::string_view stack);
// + Stroke: a stroke of the row's shape, as a layer just above the row.
[[nodiscard]] nlohmann::json graphCanvasAddStrokeLayerOps(const std::vector<GraphCanvasNode> &nodes,
                                                          std::string_view scope,
                                                          std::string_view stack,
                                                          std::size_t index);
// Delete: the layer, its lines, and the canvas nodes only it used.
[[nodiscard]] nlohmann::json graphCanvasDeleteLayerOps(const std::vector<GraphCanvasNode> &nodes,
                                                       std::string_view scope,
                                                       std::string_view stack, std::size_t index);
// Up / Down: the layer swapped with the one above (+1) or below (-1).
[[nodiscard]] nlohmann::json graphCanvasMoveLayerOps(const std::vector<GraphCanvasNode> &nodes,
                                                     std::string_view scope,
                                                     std::string_view stack, std::size_t index,
                                                     int delta);
// One member of one element of `layers`: `visible`, `opacity` or `name`.
// The default (visible, opacity 1) is written by leaving the member out.
[[nodiscard]] nlohmann::json graphCanvasSetLayerOps(const std::vector<GraphCanvasNode> &nodes,
                                                    std::string_view scope,
                                                    std::string_view stack, std::size_t index,
                                                    std::string_view key,
                                                    const nlohmann::json &value);
// A line dropped on `+ layer`: a layer on top reading it.
[[nodiscard]] nlohmann::json graphCanvasConnectLayerOps(const std::vector<GraphCanvasNode> &nodes,
                                                        std::string_view scope,
                                                        std::string_view stack,
                                                        const std::string &source_node,
                                                        const std::string &source_port);
// A stack's layer line taken away: the layer goes with it (a layer with no
// line is refused, canvas.layer_input_missing@1). Empty for a port that is
// no layer's.
[[nodiscard]] nlohmann::json graphCanvasDisconnectLayerOps(const std::vector<GraphCanvasNode> &nodes,
                                                           std::string_view scope,
                                                           std::string_view stack,
                                                           std::string_view port);
// One parameter of a shape or a paint (size, center, corner_radius,
// rotation, color, width, align), the rest of its parameters as they are.
[[nodiscard]] nlohmann::json graphCanvasSetParameterOps(const std::vector<GraphCanvasNode> &nodes,
                                                        std::string_view scope,
                                                        std::string_view node,
                                                        std::string_view key,
                                                        const nlohmann::json &value);

// The colours + Rect and + Ellipse start from, and the swatches the colour
// field offers (sRGB, straight alpha), by name.
struct GraphCanvasSwatch {
    std::string name;
    std::array<double, 4> color{};
};
[[nodiscard]] const std::vector<GraphCanvasSwatch> &graphCanvasSwatches();

} // namespace PelicanStudio
