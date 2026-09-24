# studio_render_target_authoring

The project `test/studio_scenarios/render_target_authoring.json` opens: a copy
of `studio_frameplan_edit_roundtrip` (a graph root) with two changes.

* `passes/authoring/render-targets.json` is the managed fragment the editor
  writes render targets into, already holding one target,
  `f1_referenced_target`. The root lists it in `rendering.features`.
* `passes/f1_target_writer.json` is the probe feature renamed: its one pass,
  `f1_writer_pass`, writes `f1_referenced_target`. That is the pass the
  scenario expects to be named when it tries to delete the target
  (`render_target_referenced`).

The scenario creates a second target of its own, changes its size and deletes
it; it expects that target at `render_targets[1]` of
`get_render_authoring_context`, because the editor-owned targets are listed
first and in the fragment's order.
