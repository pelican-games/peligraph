#pragma once

// NB-3 (docs/design_node_bodies.md 2.4, 2.5): the pictures on the canvas
// nodes -- a stack's composite, each of its layers, and the card of a canvas
// source or filter -- drawn by the CPU reference (canvasreference.hpp) from a
// small document Studio makes for each: the part of the authored document the
// picture depends on, a stack that reads only what is to be drawn, and a
// canvas.output reading that stack. The engine's own drawing is never
// waited for.
//
// Qt-free. The canvas calls graphCanvasThumbnailJob on its own thread (it is
// cheap: a walk up the edges and the files' sizes and times) and hands the job
// to another thread, which calls renderGraphCanvasThumbnail. The job's digest
// is the cache key: the document part, the published values it reads, the
// files it reads (their size and time) and the size of the picture.

#include "../../project/canvas/canvasreference.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {

enum class GraphCanvasThumbnailKind : std::uint8_t {
    // A canvas.stack's output, as a canvas.output reading it draws it.
    composite,
    // One element of a stack's `layers`, alone on a stack like its own.
    layer,
    // The layer a canvas source or filter hands on (the card's picture).
    card,
};

// The pictures' long side: 64 for a composite and a card, 32 for a layer row.
inline constexpr std::uint32_t graphCanvasThumbnailPixels = 64;
inline constexpr std::uint32_t graphCanvasLayerThumbnailPixels = 32;

struct GraphCanvasThumbnailTarget {
    GraphCanvasThumbnailKind kind = GraphCanvasThumbnailKind::composite;
    // The scope the node is in ("" for the root).
    std::string scope;
    // The stack (composite, layer) or the card's node.
    std::string node;
    // A layer's `input`.
    std::string input;
    // The long side, in pixels.
    std::uint32_t pixels = graphCanvasThumbnailPixels;

    auto operator<=>(const GraphCanvasThumbnailTarget &) const = default;
};

// The ops drawn as a card (design 2.4): the canvas sources and the paints and
// filters -- canvas.image, canvas.color, canvas.from_image, shape.fill,
// shape.stroke and filter.*. A shape (geometry, before it is painted) is not.
[[nodiscard]] bool graphCanvasCardOperation(std::string_view op);

// A filter card's one number beside its heading ("Blur 4 px", "Shadow 10");
// empty for a filter with no number to say (Invert) and for other ops. A
// number a value line drives says so ("Blur: line").
[[nodiscard]] std::string graphCanvasFilterSummary(std::string_view op, const nlohmann::json &parameters,
                                                   const std::vector<std::string> &driven_ports = {});

// The document one picture draws.
struct GraphCanvasThumbnailDocument {
    // {schema, version, definitions, nodes, outputs}: the target's scope's
    // nodes it reads (up its edges; a non-canvas node that gives a number is
    // kept, its own inputs are not followed), the definitions its instances
    // name, and the stack and canvas.output added for it.
    nlohmann::json document;
    // The canvas.output to draw.
    std::string output;
    // The canvas's size, in canvas units (the stack the output reads).
    std::array<double, 2> size{};
    // Why there is no document (no such node, no size to draw at); then
    // `document` is null.
    std::string error;
};

// Composite: the stack itself when it has a size, else the stack as the one
// layer of a stack of the size of the first stack it is drawn into. Layer: a
// copy of the stack (its parameters, the size it draws at) with `layers` the
// one element, shown (`visible` and `clip` left out), and the inputs that
// element reads. Card: the node's `layer` as the one layer of a stack of the
// first stack's size it is drawn into (a canvas.image on no stack: its own
// offset plus size).
[[nodiscard]] GraphCanvasThumbnailDocument graphCanvasThumbnailDocument(const nlohmann::json &scope_nodes,
                                                                      const nlohmann::json &definitions,
                                                                      const GraphCanvasThumbnailTarget &target);

// Every `project://` string the document's nodes hold (the images and fonts
// it reads), sorted, each once.
[[nodiscard]] std::vector<std::string> graphCanvasThumbnailReferences(const nlohmann::json &document);

// A project file as the cache key sees it.
struct GraphCanvasThumbnailFile {
    std::string reference;
    bool exists = false;
    std::uintmax_t size = 0;
    std::int64_t modified = 0;

    bool operator==(const GraphCanvasThumbnailFile &) const = default;
};

// The project file a `project://` reference names (empty for another kind).
[[nodiscard]] std::filesystem::path graphCanvasProjectFile(const std::filesystem::path &project_root,
                                                           std::string_view reference);

struct GraphCanvasThumbnailJob {
    GraphCanvasThumbnailTarget target;
    GraphCanvasThumbnailDocument made;
    // The published values (get_graph_node_values) of the nodes the document
    // holds, by the endpoint an edge names. An edge-driven number without
    // one is drawn at its rest value, the one the lowering's tables start at.
    Pelican::CanvasValues live;
    std::vector<GraphCanvasThumbnailFile> files;
    // The cache key (SHA-256, hex).
    std::string digest;
};

// `live`: every published value of the scope (filtered here to the nodes the
// document holds). Files are looked at under `project_root`.
[[nodiscard]] GraphCanvasThumbnailJob graphCanvasThumbnailJob(const nlohmann::json &scope_nodes,
                                                            const nlohmann::json &definitions,
                                                            const GraphCanvasThumbnailTarget &target,
                                                            const Pelican::CanvasValues &live,
                                                            const std::filesystem::path &project_root);

// The picture: straight-alpha sRGB bytes, row-major from the top left, or
// why there is none.
struct GraphCanvasThumbnailPixels {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
    std::string error;
    // NB-4: the part of the canvas a layer's or a card's picture shows
    // (x, y, width, height in canvas units), when it is cut to where the
    // layer draws; none for the whole canvas.
    std::optional<std::array<double, 4>> crop;

    [[nodiscard]] bool ok() const noexcept { return error.empty() && width != 0 && height != 0; }
};

// Decodes a picture file into straight RGBA bytes (Studio's decoder: the
// reference reads no file), or nothing when it cannot.
using GraphCanvasImageDecoder =
    std::function<std::optional<Pelican::CanvasReferenceImage>(const std::filesystem::path &file)>;

// The picture's width and height: `pixels` on the canvas's long side.
[[nodiscard]] std::array<std::uint32_t, 2> graphCanvasThumbnailExtent(std::array<double, 2> size,
                                                                     std::uint32_t pixels);

// The values the reference reads for `output`'s edge-driven numbers: the
// published value when `live` has one, else the rest value -- the number the
// parameters hold, 1 for a layer's opacity, a filter's own constant.
[[nodiscard]] Pelican::CanvasValues graphCanvasThumbnailValues(const Pelican::CanvasOutput &output,
                                                               const Pelican::CanvasValues &live);

// NB-4: where a lowered output draws, in canvas units (x, y, width,
// height): the union of its items' quads as the reference draws them (a
// shape's turned box and stroke, an image's or a glyph's cell, a shadow's or
// a glow's reach, a nested stack's own items, a colour over the canvas), each
// under its stack's transform and cut to its masks' rectangles when they
// show nothing outside. An item at no opacity draws nothing. None when
// nothing is drawn.
[[nodiscard]] std::optional<std::array<double, 4>> graphCanvasDrawnBounds(const Pelican::CanvasOutput &output,
                                                                        const Pelican::CanvasValues &values);

// NB-4: the part of a canvas of `size` a picture of `drawn` shows: `drawn`
// with a margin all round (4 % of its long side), inside
// the canvas. None when that is the whole canvas, or `drawn` is off it.
[[nodiscard]] std::optional<std::array<double, 4>> graphCanvasThumbnailCrop(const std::array<double, 4> &drawn,
                                                                          std::array<double, 2> size);

// NB-4: `document` with its canvas.output `output` reading `crop` of what it
// read: a canvas.transform moving the stack it read by -crop's corner, as
// the one layer of a stack of crop's size (in the read stack's blend space).
[[nodiscard]] nlohmann::json graphCanvasCroppedDocument(const nlohmann::json &document, const std::string &output,
                                                        const std::array<double, 4> &crop);

// The reference's image (linear, premultiplied) as the picture's bytes.
[[nodiscard]] GraphCanvasThumbnailPixels graphCanvasThumbnailBytes(const Pelican::CanvasImage &image);

// Lowers the job's document to its items (fonts read under `project_root`),
// decodes the pictures it samples and draws it with the CPU reference. Runs
// on any thread. A refusal (a missing font, a picture that cannot be read, a
// rendered image the canvas cannot see) is the result's `error`. NB-4: a
// layer's and a card's picture shows graphCanvasThumbnailCrop of where it
// draws (graphCanvasDrawnBounds), drawn by graphCanvasCroppedDocument at the
// picture's size; a composite's shows the whole canvas.
[[nodiscard]] GraphCanvasThumbnailPixels renderGraphCanvasThumbnail(const GraphCanvasThumbnailJob &job,
                                                                  const std::filesystem::path &project_root,
                                                                  const GraphCanvasImageDecoder &decode);

} // namespace PelicanStudio
