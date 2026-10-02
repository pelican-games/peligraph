#pragma once

namespace Pelican::DevCli {

// CV-1: `pelican_cli canvas-lower --project <dir> --out <dir>`. Copies the
// project to <out> (less .git and .pelican), replaces its rendering graph
// document with the lowered one, writes the generated shaders to
// <out>/shaders/generated/<stem>.{vert,frag}, drops `canvas` from the copied
// project.json's `requires`, and prints the report as JSON:
//   {"outputs": [{"id", "items", "kinds", "generated": [stem, ...]}], "diagnostics": [...]}
// A named refusal prints {"error": {"code", "detail"}} and exits 1. A build
// with PELICAN_WITH_CANVAS=OFF refuses with build_feature_disabled@1.
//
// CV-5: `--policy edit|ship` (edit, the default, is the above) and
// `--player <pelican_player>`. Ship folds each static run of layers
// (canvasship.hpp) into canvas_baked/<digest>.png, drawn by the player from the
// run alone; folds the constants into the shaders; and removes the images and
// fonts canvas nodes read that the copy no longer names. Without --player the
// runs stay (diagnostic canvas.flatten_skipped {reason: no_player}). The report
// adds "policy", each output's "instances" (every instance drawn, its groups'
// too) and, for ship, "flattened": [{output, group?, document, layers, image,
// pixels, reason}], "folded_constants" and "removed_assets". A run the player
// cannot draw is canvas.flatten_failed@1 {stack, layers, reason}.
int runCanvasLowerCommand(int argc, char *argv[]);

} // namespace Pelican::DevCli
