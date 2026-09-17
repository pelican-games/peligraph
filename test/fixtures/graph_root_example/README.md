# graph_root_example

The shipping `projects/example` rendering config, moved verbatim under the
`rendering` key of a `pelican.graph` root (P-2a-4a), plus the same move applied
to `projects/animgraph_demo` (a config that selects a preset) in
`passes/preset_main.json`.

P-2a-4b moved `projects/example` itself to a `pelican.graph` root, so this
fixture and the shipping project are now the **same document under two
formattings**: the fixture is `dump(2)` output, the shipping file keeps its
hand-written compact arrays.  `featurecompose_test.cpp` asserts both halves of
that claim -- `graph_root.at("rendering") == renderingSectionOf(shipping)` and
`graph_root == wrapLegacyRenderingConfig(renderingSectionOf(shipping))` -- so a
drift in either file, in the section or in the five graph root keys, is named
here instead of two different documents being quietly compared.
`wrapLegacyRenderingConfig` is the one production wrap
(`src/project/renderconfigsection.hpp`); `devcli project init` emits its
template through the same function.

* `passes/main.json` -- a direct config: `features`, `render_targets` and
  `rendering_passes` all live in the section.
* `passes/preset_main.json` -- `pipeline.preset` plus three features in the
  section.  A preset under a graph root had no test before this fixture.
  `projects/animgraph_demo` is still a legacy root, so this one is a fixture in
  its own right rather than a mirror of a shipping file.
* `project.json` / `scene.json` / `assets.json` -- a shell so the directory can
  be opened as a project.  The scene is empty on purpose: the shipping example's
  models are gitignored, so anything that needs geometry synthesizes it (see
  `golden_harness.cpp`'s generated projects).
