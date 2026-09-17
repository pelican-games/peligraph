# graph_root_example

The shipping `projects/example` rendering config, moved verbatim under the
`rendering` key of a `pelican.graph` root (P-2a-4a), plus the same move applied
to `projects/animgraph_demo` (a config that selects a preset) in
`passes/preset_main.json`.

None of the five shipped projects uses a `pelican.graph` root yet, so the
acceptance for "authoring reads and writes the `rendering` section" is taken
here.  Moving a shipped project is P-2a-4b, which also has to follow the 23
places that read the root shape.

* `passes/main.json` -- a direct config: `features`, `render_targets` and
  `rendering_passes` all live in the section.  Tests assert it is byte-for-byte
  the shipping config so a drift in `projects/example` is named here instead of
  silently comparing two different documents.
* `passes/preset_main.json` -- `pipeline.preset` plus three features in the
  section.  A preset under a graph root had no test before this fixture.
* `project.json` / `scene.json` / `assets.json` -- a shell so the directory can
  be opened as a project.  The scene is empty on purpose: the shipping example's
  models are gitignored, so anything that needs geometry synthesizes it (see
  `golden_harness.cpp`'s generated projects).
