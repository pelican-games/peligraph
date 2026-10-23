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
    // "rect", "ellipse", "group" (a canvas.stack), "image" (a canvas.image),
    // "color", "import" (an instance of a canvas definition: a PSD's), SG-3's
    // "text" (a paint of a shape.text) and "adjustment" (a canvas.backdrop
    // under its filters), or "layer" (anything else, or nothing connected).
    std::string kind = "layer";
    // "fill" or "stroke" for a paint, else empty.
    std::string paint;
    // The node the layer draws -- behind its canvas.transform when it has
    // one -- and the shape behind a paint.
    std::string source;
    std::string shape;
    // SG-2: the canvas.transform between the layer's input and `source`.
    std::string transform;
    // SG-3: the filter.* nodes between the layer's input and `source` (and
    // its transform), in the order they apply: the first reads the source
    // (or the transform), the stack reads the last.
    std::vector<std::string> filters;
    // SG-2 (CV-2a): clipped to the layer below; a group composited with the
    // stack it is in; the input its mask is read from, and that mask's
    // canvas.image; the blend (`normal` when it says none).
    bool clip = false;
    bool pass_through = false;
    std::string mask;
    std::string mask_source;
    std::string blend = "normal";
    // An import's definition (kind "import").
    std::string definition;

    bool operator==(const GraphCanvasStackRow &) const = default;
};

// The rows of one stack, top first (the reverse of `layers`). Empty when
// `stack` is no canvas.stack of `nodes`.
[[nodiscard]] std::vector<GraphCanvasStackRow> graphCanvasLayerRows(
    const std::vector<GraphCanvasNode> &nodes, std::string_view stack);
// "Rect", "Ellipse", "Group", "Text", "Adjustment", "Layer"; a stroke's is
// "<shape> stroke".
[[nodiscard]] std::string graphCanvasLayerKindText(const GraphCanvasStackRow &row);

// NB-2a: what a row of the stack node (docs/design_node_bodies.md 2.3) needs
// beside its GraphCanvasStackRow. The row's sockets are `row.input` (the
// layer), graphCanvasLayerOpacityPort(row.input) when `row.opacity_driven`,
// and `row.mask` when `row.mask_source` is set.
//
// The port a value line drives a layer's opacity on: `<input>.opacity`.
[[nodiscard]] std::string graphCanvasLayerOpacityPort(std::string_view input);
// Where a row's `>` goes: a group into its own stack, in the same scope and
// as read only as the stack it is in; an import into its definition, read
// only (the Layers dock's rule), its stack the root of `definition_nodes`
// (the definition's nodes; empty `stack` when they are not given or hold no
// stack). Nothing for any other row.
struct GraphCanvasRowEntry {
    std::string scope;
    std::string stack;
    bool read_only = false;

    bool operator==(const GraphCanvasRowEntry &) const = default;
};
[[nodiscard]] std::optional<GraphCanvasRowEntry> graphCanvasRowEntry(
    const GraphCanvasStackRow &row, std::string_view scope, bool read_only,
    const std::vector<GraphCanvasNode> &definition_nodes = {});
// The stack of a definition body no node of it reads: its root (an imported
// PSD's), or empty.
[[nodiscard]] std::string graphCanvasRootStack(const std::vector<GraphCanvasNode> &nodes);
// Whether `definition` is an imported canvas: a layer of some stack of
// `nodes` (the scope the Graph descended from) reads an instance of it. Its
// stacks -- the groups in it too -- are read only: no add, delete or move.
[[nodiscard]] bool graphCanvasImportedDefinition(const std::vector<GraphCanvasNode> &nodes,
                                                 std::string_view definition);

// The stack the Layers panel shows for a node picked in the Graph: the stack
// itself, the stack a canvas.output or (SG-3) a canvas.place reads, or the
// stack that holds a picked paint, shape, filter or backdrop (a nested one
// before its parent). Empty for anything else.
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

// NB-2a: every + below takes `above`, the index of the picked row: the new
// layer goes just above it (at `above + 1` in `layers`). Without one, or one
// past the top, it goes on top -- as + Adjustment's `above`.
//
// + Rect / + Ellipse: the shape at the stack's middle, a quarter of its size,
// its fill, and a layer on top reading it. `shape` is "rect" or "ellipse".
[[nodiscard]] nlohmann::json graphCanvasAddShapeLayerOps(const std::vector<GraphCanvasNode> &nodes,
                                                         std::string_view scope,
                                                         std::string_view stack,
                                                         std::string_view shape,
                                                         std::optional<std::size_t> above = std::nullopt);
// + Group: an empty stack of the parent's size, as a layer on top.
[[nodiscard]] nlohmann::json graphCanvasAddGroupLayerOps(const std::vector<GraphCanvasNode> &nodes,
                                                         std::string_view scope,
                                                         std::string_view stack,
                                                         std::optional<std::size_t> above = std::nullopt);
// + Stroke: a stroke of the row's shape, as a layer just above the row.
[[nodiscard]] nlohmann::json graphCanvasAddStrokeLayerOps(const std::vector<GraphCanvasNode> &nodes,
                                                          std::string_view scope,
                                                          std::string_view stack,
                                                          std::size_t index);
// Delete: the layer, its lines, and the canvas nodes only it used.
[[nodiscard]] nlohmann::json graphCanvasDeleteLayerOps(const std::vector<GraphCanvasNode> &nodes,
                                                       std::string_view scope,
                                                       std::string_view stack, std::size_t index);
// Up / Down: the layer swapped with the one above (+1) or below (-1) --
// graphCanvasMoveLayerToOps to `index + delta`.
[[nodiscard]] nlohmann::json graphCanvasMoveLayerOps(const std::vector<GraphCanvasNode> &nodes,
                                                     std::string_view scope,
                                                     std::string_view stack, std::size_t index,
                                                     int delta);
// NB-2a, a row dragged: `layers[from]`, every member of it as it is, moved
// so that it is `layers[to]` afterwards (the others keep their order). One
// set_node_parameters; empty when `from == to` or either is past the end.
// A drop between rows: the rows are top first, so the gap `g` under the
// `g`-th row from the top (0 above the top one) of `n` is the insert place
// `p = n - g`, and the layer at `from` lands at `to = p > from ? p - 1 : p`.
[[nodiscard]] nlohmann::json graphCanvasMoveLayerToOps(const std::vector<GraphCanvasNode> &nodes,
                                                       std::string_view scope,
                                                       std::string_view stack, std::size_t from,
                                                       std::size_t to);
// One member of one element of `layers`: `visible`, `opacity` or `name`.
// The default (visible, opacity 1) is written by leaving the member out.
[[nodiscard]] nlohmann::json graphCanvasSetLayerOps(const std::vector<GraphCanvasNode> &nodes,
                                                    std::string_view scope,
                                                    std::string_view stack, std::size_t index,
                                                    std::string_view key,
                                                    const nlohmann::json &value);
// A line dropped on `+ layer`: a layer on top reading it --
// graphCanvasInsertLayerOps at the top.
[[nodiscard]] nlohmann::json graphCanvasConnectLayerOps(const std::vector<GraphCanvasNode> &nodes,
                                                        std::string_view scope,
                                                        std::string_view stack,
                                                        const std::string &source_node,
                                                        const std::string &source_port);
// NB-2a, a line dropped between two rows: a layer reading `source_node`'s
// `source_port` put in `layers` at `at` (0 is the bottom; the size or past
// it, on top), and its line. Its input is named after the node (dots made
// underscores, numbered when taken) and the layer after the node's heading,
// as for `+ layer`. Empty for a source that is not there or is the stack.
[[nodiscard]] nlohmann::json graphCanvasInsertLayerOps(const std::vector<GraphCanvasNode> &nodes,
                                                       std::string_view scope,
                                                       std::string_view stack, std::size_t at,
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

// --- SG-2: the Layers dock.

// The stacks the dock can show in `nodes`: the stack each canvas.output
// reads, in document order (an output reading no stack is left out).
struct GraphCanvasOutputStack {
    std::string output;
    std::string stack;

    bool operator==(const GraphCanvasOutputStack &) const = default;
};
[[nodiscard]] std::vector<GraphCanvasOutputStack> graphCanvasOutputStacks(
    const std::vector<GraphCanvasNode> &nodes);
// The stack a definition hands out as its `layer` output (an imported PSD's
// root), read from the definition's nodes and outputs; empty when it is no
// canvas.stack.
[[nodiscard]] std::string graphCanvasDefinitionStack(const nlohmann::json &definition);

// + Image: a canvas.image of a picture already in the project (`asset` is
// its project:// reference, `size` its pixels) at the stack's middle, as a
// layer on top named `name`.
[[nodiscard]] nlohmann::json graphCanvasAddImageLayerOps(const std::vector<GraphCanvasNode> &nodes,
                                                         std::string_view scope,
                                                         std::string_view stack,
                                                         const std::string &asset,
                                                         const std::array<double, 2> &size,
                                                         const std::string &name,
                                                         std::optional<std::size_t> above = std::nullopt);
// Whether the layer at `index` may be a clip layer: some layer below it is
// not one (what it is clipped to; canvas.clip_base_missing@1 otherwise), and
// so has every clip layer above it once it is.
[[nodiscard]] bool graphCanvasClipAllowed(const std::vector<GraphCanvasNode> &nodes,
                                          std::string_view stack, std::size_t index);
// One member of a layer the SG-1 call does not take: `clip`, `pass_through`
// (false is the default, left out) or `blend` ("normal" is left out).
[[nodiscard]] nlohmann::json graphCanvasSetLayerFlagOps(const std::vector<GraphCanvasNode> &nodes,
                                                        std::string_view scope,
                                                        std::string_view stack, std::size_t index,
                                                        std::string_view key,
                                                        const nlohmann::json &value);
// Add mask image: a luminance canvas.image of `asset` (`size` its pixels)
// over the layer -- at an image layer's offset, else at the stack's middle --
// read as the layer's `mask`. Empty when the layer has a mask already.
[[nodiscard]] nlohmann::json graphCanvasAddMaskOps(const std::vector<GraphCanvasNode> &nodes,
                                                   std::string_view scope, std::string_view stack,
                                                   std::size_t index, const std::string &asset,
                                                   const std::array<double, 2> &size);
// Remove mask: the member, its line, and its canvas.image when nothing else
// reads it.
[[nodiscard]] nlohmann::json graphCanvasRemoveMaskOps(const std::vector<GraphCanvasNode> &nodes,
                                                      std::string_view scope, std::string_view stack,
                                                      std::size_t index);
// The stack's own fields: `blend_space` ("linear" is the default, left out)
// or `size`.
[[nodiscard]] nlohmann::json graphCanvasSetStackOps(const std::vector<GraphCanvasNode> &nodes,
                                                    std::string_view scope, std::string_view stack,
                                                    std::string_view key, const nlohmann::json &value);

// A layer's Transform. All at their defaults (no move, no turn, scale 1)
// is no canvas.transform at all.
struct GraphCanvasTransformValues {
    std::array<double, 2> translate{0.0, 0.0};
    double rotate = 0.0;
    std::array<double, 2> scale{1.0, 1.0};

    [[nodiscard]] bool isDefault() const noexcept;
    bool operator==(const GraphCanvasTransformValues &) const = default;
};
// What the row's canvas.transform says, the defaults when it has none.
[[nodiscard]] GraphCanvasTransformValues graphCanvasLayerTransform(
    const std::vector<GraphCanvasNode> &nodes, const GraphCanvasStackRow &row);
// The Transform set: a canvas.transform put between the layer and the stack
// when there was none, its parameters set when there was, taken out when
// `values` are the defaults again. One list.
[[nodiscard]] nlohmann::json graphCanvasSetTransformOps(const std::vector<GraphCanvasNode> &nodes,
                                                        std::string_view scope,
                                                        std::string_view stack, std::size_t index,
                                                        const GraphCanvasTransformValues &values);

// Import PSD output: `definition` (canvas.graph.json's one definition, its
// assets already the project's) added as `name`, a graph.instance of it, and
// that instance as the top layer of `stack`, named `PSD: <label>`. One list.
[[nodiscard]] nlohmann::json graphCanvasImportDefinitionOps(const std::vector<GraphCanvasNode> &nodes,
                                                            std::string_view scope,
                                                            std::string_view stack,
                                                            const std::string &name,
                                                            const nlohmann::json &definition,
                                                            const std::string &label);
// `definition` with every canvas.image `asset` read from the import's
// directory: a relative path, or a project://imports/<other>/ one, becomes
// `project://imports/<directory>/<path>`.
[[nodiscard]] nlohmann::json graphCanvasRebaseImportAssets(const nlohmann::json &definition,
                                                           std::string_view directory);

// The one sentence the Layers dock says for a refusal of the canvas's
// (CV-2a's named errors), found in the notice the edit got; empty for any
// other.
[[nodiscard]] std::string graphCanvasRefusalSentence(std::string_view notice);

// The colours + Rect and + Ellipse start from, and the swatches the colour
// field offers (sRGB, straight alpha), by name.
struct GraphCanvasSwatch {
    std::string name;
    std::array<double, 4> color{};
};
[[nodiscard]] const std::vector<GraphCanvasSwatch> &graphCanvasSwatches();

// --- SG-3: blend modes, text, adjustment layers and filters, 3D.

// One entry of the blend combo: an engine name (Pelican::canvasBlendNames()),
// the words it shows (Photoshop's) and its group as Photoshop's menu
// separates them: 0 normal, 1 darken, 2 lighten, 3 contrast, 4 inversion,
// 5 component. The groups in that order, each in the engine's order; a name
// the table does not know is shown as it is, in a group of its own (6) at the
// end, not left out.
struct GraphCanvasBlendChoice {
    std::string name;
    std::string label;
    int group = 0;

    bool operator==(const GraphCanvasBlendChoice &) const = default;
};
[[nodiscard]] const std::vector<GraphCanvasBlendChoice> &graphCanvasBlendChoices();

// + Text: a shape.text of `text` in `font` (a project:// reference) at
// `size`, centred on the stack's middle (`origin` there, align center), its
// white fill, and a layer on top named after the text's first line. One list.
[[nodiscard]] nlohmann::json graphCanvasAddTextLayerOps(const std::vector<GraphCanvasNode> &nodes,
                                                        std::string_view scope, std::string_view stack,
                                                        const std::string &text, const std::string &font,
                                                        double size,
                                                        std::optional<std::size_t> above = std::nullopt);
// One parameter of a shape.text, the rest as they are; the engine's default
// (align left, line_height 1.2, tracking 0, box_width 0) is written by leaving
// the parameter out.
[[nodiscard]] nlohmann::json graphCanvasSetTextParameterOps(const std::vector<GraphCanvasNode> &nodes,
                                                            std::string_view scope, std::string_view node,
                                                            std::string_view key, const nlohmann::json &value);
// The sizes the text size combos offer, in the stack's pixels (48 first is
// not implied: the list is ascending).
[[nodiscard]] const std::vector<double> &graphCanvasTextSizes();
inline constexpr double graphCanvasDefaultTextSize = 48.0;
// The stroke row that outlines the same shape as the row at `index` (a text's
// Stroke box), or nothing.
[[nodiscard]] std::optional<std::size_t> graphCanvasStrokeRowOf(const std::vector<GraphCanvasNode> &nodes,
                                                                std::string_view stack, std::size_t index);

// One filter the + Filter menu offers: its op (Pelican::canvasFilterOperations(),
// in that order -- the colour filters, then CV-3a's Gaussian Blur, Drop
// Shadow and Outer Glow) and its words. + Adjustment offers the colour
// filters alone (Pelican::canvasColorFilterOperations()).
struct GraphCanvasFilterChoice {
    std::string op;
    std::string label;

    bool operator==(const GraphCanvasFilterChoice &) const = default;
};
[[nodiscard]] const std::vector<GraphCanvasFilterChoice> &graphCanvasFilterChoices();
[[nodiscard]] const std::vector<GraphCanvasFilterChoice> &graphCanvasAdjustmentChoices();
// The words for a filter op ("Invert"), the op itself for one not offered.
[[nodiscard]] std::string graphCanvasFilterLabel(std::string_view op);
// What a new filter.<kind> starts with: nothing changed where it can (an
// overlay and a map bring colours of their own; CV-3a: a blur, a shadow and
// a glow Photoshop's defaults). Null for an op no menu offers.
[[nodiscard]] nlohmann::json graphCanvasFilterDefaults(std::string_view op);
// + Adjustment: a canvas.backdrop, `op` reading it and a layer reading that,
// just above the layer at `above` (on top without one), named `<Filter> <n>`.
// One list; empty for an op that is not a colour filter.
[[nodiscard]] nlohmann::json graphCanvasAddAdjustmentLayerOps(const std::vector<GraphCanvasNode> &nodes,
                                                              std::string_view scope, std::string_view stack,
                                                              std::optional<std::size_t> above,
                                                              std::string_view op);
// + Filter: `op` put between the layer and the stack, above the filters the
// layer has and its transform. One list.
[[nodiscard]] nlohmann::json graphCanvasAddFilterOps(const std::vector<GraphCanvasNode> &nodes,
                                                     std::string_view scope, std::string_view stack,
                                                     std::size_t index, std::string_view op);
// Remove filter: what read `filter` reads what it read, and it goes. Empty
// for a filter the row does not have, and for an adjustment's last one
// (Delete takes that layer).
[[nodiscard]] nlohmann::json graphCanvasRemoveFilterOps(const std::vector<GraphCanvasNode> &nodes,
                                                        std::string_view scope, std::string_view stack,
                                                        std::size_t index, std::string_view filter);

// A 3D pass a stack can be placed in: a render.material_pass of the scope, a
// colour attachment of it (an output of its) and its depth output when the
// pass draws with a depth image (empty when not).
struct GraphCanvasScenePass {
    std::string node;
    std::string color;
    std::string depth;

    bool operator==(const GraphCanvasScenePass &) const = default;
};
// Every material pass's colour attachments, in document order.
[[nodiscard]] std::vector<GraphCanvasScenePass> graphCanvasScenePasses(const std::vector<GraphCanvasNode> &nodes);
// The canvas.place nodes that read `stack`, in document order.
[[nodiscard]] std::vector<std::string> graphCanvasPlacesOf(const std::vector<GraphCanvasNode> &nodes,
                                                           std::string_view stack);
// Place in 3D scene: a canvas.place of `stack` at the origin, unturned, scale
// 1 and two units wide (pixels_per_unit half the stack's width), drawn onto
// `pass`'s colour and tested against its depth. The canvas.output stays.
[[nodiscard]] nlohmann::json graphCanvasPlaceOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                                                 std::string_view stack, const GraphCanvasScenePass &pass);
// One parameter of a canvas.place, the rest as they are; the engine's
// default (rotation [0, 0, 0], scale 1, anchor [0.5, 0.5], depth_test true,
// depth_write false, double_sided true) is written by leaving it out.
[[nodiscard]] nlohmann::json graphCanvasSetPlaceParameterOps(const std::vector<GraphCanvasNode> &nodes,
                                                             std::string_view scope, std::string_view place,
                                                             std::string_view key, const nlohmann::json &value);

} // namespace PelicanStudio
