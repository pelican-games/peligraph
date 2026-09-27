# studio_graph_layers_origin

The project `test/studio_scenarios/graph_layers_origin.json` opens (GL-2b): the
engine's `hybrid_v1` preset, and the engine's `shadow_directional` feature
whose `shadow_depth` pass is replaced by the graph's loop `cascades`
(`pass_replacements`) -- the graph of GL-2a's compiler test, in a project.

Its root graph holds

* `tint`, a fullscreen pass (the engine's passthrough) drawing into
  `tint_image`, and `show`, a fullscreen pass reading `tint_image` as its
  input 0 and drawing into `show_image`, so the frame plan has a barrier
  between two root graph nodes;
* `left`, an instance of the definition `paint` (an image and a fullscreen
  pass into it);
* `cascades`, a loop of three over the shadow cascades. Element 0 runs the
  body `near`, the others the body `cascade` (the `else` branch): both draw
  the cascade's shadow depth into `shadow_map`, the feature's target, which
  the root reaches through the `render.target_ref` `shadow`.

So the frame plan's passes carry `origin` for a root node, an instance's node
and a loop's copies, and none for the preset's and the feature's passes. The
scenario reads the loop's band (`×3 copies`), opens a barrier row of the
Frame Plan in the Graph tab, and opens a copy of element 1 or 2, whose zone's
tab moves to the `else` branch.

The scenario copies this directory before opening it; nothing here is written.
