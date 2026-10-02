#pragma once

#include "canvaslowering.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Pelican {

// CV-5: the ship policy's first step (design section 6) -- the runs of a
// stack's layers that never move, each drawn once into an image the stack
// then reads instead. The plan and its application are pure functions of the
// document (and of the bytes of the files it names, for the digests); the
// drawing itself is the caller's (pelican_cli canvas-lower --policy ship runs
// each run's document through the player, the same generated shader the
// edit policy draws with).
//
// A layer is static when nothing it draws moves: every node under it is a
// canvas node with no edge from outside the canvas (no `value` edge into a
// shape, a paint, an opacity; no canvas.from_image), and the stack's own
// `<input>.opacity` port is unconnected. A hidden layer draws nothing and
// joins the run around it. A base and its clip layers count as one layer.
//
// Each stack a canvas.output composites (its own stack, and a nested stack
// with a `size` that is a layer of it, not pass-through, through any
// canvas.transform) is cut bottom to top:
//   - every layer static: the whole stack is one run (blends and adjustment
//     layers included), reason "static_unit";
//   - otherwise a run is a stretch of static layers that read no backdrop (a
//     blend other than normal, a canvas.backdrop, either inside a
//     pass-through group): reason "static_from_bottom" when nothing below it
//     in the stack stays, "static_above_cut" when something does (premultiplied
//     over is associative, so the run composited alone and laid over what is
//     below is the same picture);
//   - a run of shapes and text alone (shape.*, canvas.color, canvas.transform,
//     canvas.stack) stays as it is, unless a layer of it says
//     `"bake": "texture"` (reason "bake_texture"); so does a run that is one
//     canvas.image already.
// A run's image is the stack's `size` times its `bake_scale` (1 when absent)
// in pixels. Stacks a canvas.place draws, and stacks reached through a
// pass-through layer, are not cut (their layers keep being drawn as authored).
//
// Named failures:
//   canvas.flatten_size_exceeded@1 {stack, pixels, limit}  a run's image past canvas_flatten_pixel_limit a side
struct CanvasFlattenRun {
    // Where the stack is: the scope ("" the root), the canvas.output whose
    // picture it is part of, the stack, and whether the stack is a group (not
    // the output's own stack).
    std::string scope;
    std::string output;
    std::string stack;
    bool group = false;
    // The layers it replaces: `layers[first..last]` of the stack, and the
    // names of the ones drawn (a layer's `name`, or its index).
    std::size_t first = 0;
    std::size_t last = 0;
    nlohmann::json layers = nlohmann::json::array();
    std::string reason;
    // The stack's size, the image's pixels.
    std::array<double, 2> size{};
    std::array<std::uint32_t, 2> pixels{};
    // A graph document that draws the run alone, in the edit policy: the
    // run's nodes, the stack with the run's layers alone, a canvas.output
    // `bake_output` of `pixels`, and a rendering section that presents it.
    nlohmann::json document;
    std::string bake_output;
    // The project files the document reads (canvasDocumentAssets).
    std::vector<std::string> assets;
    // Of the document and the bytes of its files: 16 hex digits.
    std::string digest;
};

struct CanvasFlattenPlan {
    std::vector<CanvasFlattenRun> runs;
    // [{code: canvas.flatten_skipped, reason: placed, node}] per canvas.place.
    nlohmann::json diagnostics = nlohmann::json::array();
};

inline constexpr std::uint32_t canvas_flatten_pixel_limit = 16384;

// The runs of `document` (as authored: canvas definitions are expanded
// first, as the lowering does). `read_asset` reads the files the runs name,
// for the digests (a file it does not have counts as missing).
CanvasFlattenPlan planCanvasFlatten(const nlohmann::json &document, const CanvasAssetReader &read_asset);

// Where a run's image goes in the shipped project: canvas_baked/<digest>.png.
std::string canvasFlattenImagePath(const CanvasFlattenRun &run);

// `document` with its canvas definitions expanded and each run replaced by one
// layer: a canvas.image `<stack>$baked<k>` reading project://<image path>,
// alpha premultiplied, over the stack's size.
nlohmann::json applyCanvasFlatten(const nlohmann::json &document, const CanvasFlattenPlan &plan);

// The files the document's canvas nodes read: canvas.image assets (masks
// too) and shape.text fonts, in every scope, sorted, each once.
std::vector<std::string> canvasDocumentAssets(const nlohmann::json &document);

} // namespace Pelican
