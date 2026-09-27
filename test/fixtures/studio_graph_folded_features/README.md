# studio_graph_folded_features

The project `test/studio_scenarios/graph_folded_features.json` opens: a copy of
`studio_material_loop_decompose` (GL-1). A graph root with no graph nodes,
whose own rendering section declares a `forward_opaque` material pass writing
`scene_color` and `scene_depth`, and a `present` that shows `scene_color`
through `engine://passthrough`.

No graph node made either pass, so the Graph tab draws both as folded,
read-only nodes on the root canvas, with the frame plan's `scene_color`
between them as a dashed line. The scenario looks inside `forward_opaque`
(a `render.material_pass` and a draw_loop zone drawing each material), tries
an edit there (nothing is sent; the notice offers Expand to edit), comes back
out by the breadcrumb, and opens the pass in the Graph tab from the Frame Plan.
The document is never written.
