#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// CV-1: the pelican.canvas lowering. A pure function of the graph document:
// it reads no clock, and the same document always lowers to the same bytes
// (document, shaders and atlases). CV-4: the one file it reads is a font a
// shape.text names, through the caller's CanvasAssetReader.
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
// CV-2a adds:
//   canvas.op_unsupported@1        {node, op}            canvas.backdrop and filter.* (CV-2b)
//   canvas.clip_base_missing@1     {node, layer}         a clip layer with no unclipped layer below it
//   canvas.mask_unsupported@1      {node, layer, op}     a mask that is not a canvas.image
//   canvas.instance_inputs_unsupported@1 {node, definition}  a canvas definition with graph.input
//   canvas.draw_images_exceeded@1  {output, count, limit}    one item reads more images than a draw can
// CV-4 adds:
//   canvas.font_missing@1          {node, font, reason}       a shape.text font the reader does not have
//   canvas.font_unreadable@1       {node, font, face, reason} not a font, or no such face in it
//   canvas.glyph_missing@1         {node, codepoint}          a character the face has no glyph for
// and the diagnostic canvas.glyph_range_exceeded {node, width, range, limit}:
// a shape.stroke of a shape.text wider than the atlas's distances reach,
// cut to `limit` (stack units).
// `layer` names the layer by its `name`, or by its index when it has none.
// Wrong types and unknown ports are the graph's own checks.

enum class CanvasShape : std::uint8_t { rect, ellipse };
enum class CanvasPaint : std::uint8_t { fill, stroke };
enum class CanvasFit : std::uint8_t { contain, stretch };

// CV-2a: what an item draws. A shape is CV-1's (shape times paint); an image
// is a texture over a rectangle (canvas.image, canvas.from_image); a color
// fills the canvas (canvas.color); a group is the image a nested stack was
// composited into, read back over the whole canvas. CV-4: a glyph is one
// character of a shape.text, read from its output's atlas.
enum class CanvasItemSource : std::uint8_t { shape, image, color, group, glyph };

// CV-4: reads a project file by its reference (project://...): the same
// buffer while the file is unchanged, nullptr when there is none. The canvas
// lowering reads the fonts shape.text names through it.
using CanvasAssetReader = std::function<std::shared_ptr<const std::vector<std::byte>>(std::string_view reference)>;

// How a texture holds alpha, and the conversion between the stack's blend
// space and the texture's values.
enum class CanvasAlpha : std::uint8_t { straight, premultiplied, opaque };
enum class CanvasEncode : std::uint8_t { none, linear_to_srgb, srgb_to_linear };

// One image the generated shader samples. A file is read through a
// render.image `source` (`linear` reads the file's bytes as they are, else
// the sRGB decode); a graph image is the endpoint a canvas.from_image reads;
// a group is the image of CanvasOutput::groups[group].
struct CanvasTexture {
    enum class Origin : std::uint8_t { file, graph, group };
    Origin origin = Origin::file;
    std::string asset;
    bool linear = false;
    std::optional<std::string> node;
    std::string port;
    std::size_t group = 0;
    bool nearest = false;
    auto operator<=>(const CanvasTexture &) const = default;
};

// A mask: the texture's alpha or luminance over its rectangle, `outside`
// beyond it, in the coordinates of the stack that declared it (`affine`, the
// map from that stack to the canvas: a b c d tx ty).
struct CanvasMask {
    CanvasTexture texture;
    bool luminance = false;
    double outside = 0.0;
    std::array<double, 4> rect{}; // offset.x offset.y size.x size.y
    std::array<double, 6> affine{1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
    std::string node;
};

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

// CV-2a kinds past CV-1's four, and their parameters in table order:
//   4 image:  offset.x offset.y size.x size.y alpha(0 straight, 1 premultiplied, 2 opaque) encode
//   5 color:  color.r color.g color.b color.a (sRGB, straight alpha)
//   6 group:  encode (0 none, 1 linear to sRGB, 2 sRGB to linear)
inline constexpr std::uint32_t canvas_kind_image = 4;
inline constexpr std::uint32_t canvas_kind_color = 5;
inline constexpr std::uint32_t canvas_kind_group = 6;
// CV-4, a glyph of a shape.text (one instance per character with an outline):
//   7 glyph_fill:   origin.x origin.y size  cell.x cell.y cell.w cell.h  uv.x uv.y uv.w uv.h  texels
//                   then fill's color.r .. color.a
//   8 glyph_stroke: the same, then stroke's color.r .. color.a width align
// `cell` is the glyph's atlas cell in em, relative to the origin (+y down; the
// glyph's pen position is folded in), so the cell covers origin + size * cell
// in the stack. `uv` is the cell in the atlas (normalized), `texels` its side.
inline constexpr std::uint32_t canvas_kind_glyph_fill = 7;
inline constexpr std::uint32_t canvas_kind_glyph_stroke = 8;
inline constexpr std::size_t canvas_glyph_parameter_count = 12;

// The images one draw may read; past it the items are split across draws of
// the same image. The engine sizes a pass's descriptor budget by the same
// number (passVariadicInputLimit).
inline constexpr std::size_t canvas_draw_image_limit = 16;

// CV-4: which glyph a glyph item draws, before the atlas places it.
struct CanvasGlyph {
    // The lowering's font face (in the order shape.text nodes name them).
    std::size_t font = 0;
    int glyph = 0;
    char32_t codepoint = 0;
    // The pen position (baseline, left) in em from the text's origin, +y down.
    std::array<double, 2> pen{};
    // shape.text atlas_cell, or 0 for the size the codepoint asks for.
    std::uint32_t cell = 0;
};

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
    // CV-2a.
    CanvasItemSource source = CanvasItemSource::shape;
    CanvasTexture texture; // image, group and (CV-4) glyph items
    std::vector<CanvasMask> masks;
    // A clip layer's base: the index of the item in the same list. The item
    // is drawn atop what is below it (the base alone: a base and its clip
    // layers always make a group), keeping that alpha.
    std::optional<std::size_t> clip_base;
    // The map from the item's stack to the canvas (canvas.transform).
    std::array<double, 6> affine{1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
    // CV-4: a glyph item's character (its cell, uv and texture are filled in
    // when the output's atlas is baked).
    std::optional<CanvasGlyph> glyph;
};

std::uint32_t canvasItemKind(const CanvasItem &item);

// CV-2a: a nested stack composited into one image before its parent reads it
// (not pass_through, a blend_space boundary, a base and its clip layers).
// Its items are in the canvas's coordinates, like the output's.
struct CanvasGroup {
    std::vector<CanvasItem> items;
    // The blend space: sRGB values (premultiplied) instead of linear.
    bool srgb = false;
    // The authored stack (or clip base) it was made for.
    std::string node;
};

// CV-4: an output's glyph atlas for one font face (canvastext.hpp has the
// layout): RGBA bytes, and the same as a PNG, which the lowering writes.
struct CanvasAtlasPixels {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
    std::vector<std::byte> png;
};

struct CanvasAtlas {
    // The font as shape.text names it, and the face.
    std::string font;
    int face = 0;
    std::uint32_t cell = 0;
    // `canvas_<output>_font<k>_<first 8 hex digits of the PNG's digest>`.
    std::string stem;
    // The file, relative to the project (CanvasLoweringOptions), and the
    // render.image source asset that reads it (project://<path>).
    std::string path;
    std::string asset;
    std::shared_ptr<const CanvasAtlasPixels> pixels;
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
    // CV-2a: the images composited before the output reads them; group n is
    // drawn into `<id>$canvas_g<n>_image`, numbered in the order the walk of
    // the document meets them.
    std::vector<CanvasGroup> groups;
    // CV-4: one per font face its glyph items read, in first-use order.
    std::vector<CanvasAtlas> atlases;
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

struct CanvasGeneratedSource {
    // `canvas_<output id>_<first 8 hex digits of the sources' digest>`; the
    // pass reads project://<path less its extension>, which the caller writes.
    // CV-2a: each draw of an output (its groups', its split draws') has its
    // own stem, and its own path by the same CanvasLoweringOptions rule.
    std::string stem;
    // "vert" or "frag"; CV-4: "atlas" for a glyph atlas, whose PNG is `bytes`.
    std::string stage;
    std::string text;
    std::vector<std::byte> bytes;
    // The canvas.output the draw belongs to.
    std::string output;
    // The file, relative to the project (CanvasLoweringOptions).
    std::string path;
    // The definition the output is in; empty at the root.
    std::string scope;
};

struct CanvasLoweringOptions {
    // Where an output's shader pair lives, relative to the project:
    // `<shader_directory>/<stem>.<stage>`, or with directory_per_stem
    // `<shader_directory>/<stem>/<stem>.<stage>`. The shader compiler keys its
    // cache on every file beside the source, so a directory of its own keeps
    // one pair's key from moving when another output's pair is written.
    std::string shader_directory = "shaders/generated";
    bool directory_per_stem = false;
    // CV-4: where a glyph atlas lives, by the same rule (the shader
    // compiler's cache key never sees it: it is not beside a shader).
    std::string asset_directory = "canvas_generated";
};

struct CanvasLowering {
    // The lowered document. Without any canvas op it is the input, untouched.
    nlohmann::json document;
    bool changed = false;
    std::vector<CanvasGeneratedSource> generated_sources;
    // [{output, instance, kind, nodes}]: which authored nodes drew each
    // instance of each output's item table. CV-2a: a group's draws add
    // {group}, a split draw {draw}.
    nlohmann::json items = nlohmann::json::array();
    nlohmann::json diagnostics = nlohmann::json::array();
    CanvasIr ir;
};

// CV-4: `read_asset` reads the fonts shape.text names (without it a
// shape.text is canvas.font_missing@1); the options place the atlases.
CanvasIr buildCanvasIr(const nlohmann::json &document, const CanvasAssetReader &read_asset = {},
                       const CanvasLoweringOptions &options = {});

// The names a canvas.output `<id>` lowers to. The raster pass keeps `<id>`
// itself, so its pass name is the author's node and its written image port is
// `image`.
std::string canvasImageNodeId(const std::string &output);
std::string canvasItemsNodeId(const std::string &output);
std::string canvasBakeNodeId(const std::string &output);
std::string canvasFrameNodeId(const std::string &output);
std::string canvasFrameSinkNodeId(const std::string &output, std::size_t index);
// CV-2a: group n of an output is drawn by the raster `<output>$canvas_g<n>`
// into `<output>$canvas_g<n>_image`; its tables are `<output>$canvas_g<n>_items`
// and so on. A file image is `<output>$canvas_asset<k>`, numbered by first use.
// A draw split off before the last is `<raster>$d<k>` with the tables
// `<raster>$d<k>_items`, ...; the last draw keeps the raster's own name.
std::string canvasGroupImageNodeId(const std::string &output, std::size_t group);
std::string canvasGroupRasterNodeId(const std::string &output, std::size_t group);
std::string canvasAssetNodeId(const std::string &output, std::size_t index);

// CV-2a: copies each graph.instance of a canvas definition (one whose output is
// produced by a canvas op, or whose graph.input feeds a layer or geometry
// port) into its scope as `<instance id>$<node id>`, rewrites the edges that
// read the instance, and drops the canvas definitions. The canvas lowering
// runs before the graph's own instance expansion, so it does this itself.
nlohmann::json expandCanvasInstances(const nlohmann::json &document);

// CV-1b: a pass of the rendering section that reads a root canvas.output by
// its id (`"input": ["out"]`, or `out@history`) reads the image the lowering
// made, `out$canvas_image`, instead -- unless the section declares a target or
// buffer of that name itself.
CanvasLowering lowerCanvasNodes(const nlohmann::json &document, const CanvasLoweringOptions &options = {},
                                const CanvasAssetReader &read_asset = {});

} // namespace Pelican
