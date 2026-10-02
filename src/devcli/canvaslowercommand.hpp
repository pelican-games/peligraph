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
int runCanvasLowerCommand(int argc, char *argv[]);

} // namespace Pelican::DevCli
