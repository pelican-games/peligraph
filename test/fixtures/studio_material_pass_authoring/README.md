# studio_material_pass_authoring

The project `test/studio_scenarios/material_pass_authoring.json` opens: a
graph root that selects no preset (a direct config, like a Blank pipeline
project), one model and one material of its own.

* `checker_box.glb` is the box `projects/pathtrace_demo` ships.
  `material_bindings.json` binds its one primitive to `g1_flat`
  (`materials/g1_flat.material.json`), whose `render_path` is `forward`: the
  box is drawn by a `forward_opaque_v1` pass. Its surface,
  `shaders/g1_flat.surface`, returns its tint from the lighting hook, so the
  box is one flat colour with no light in the scene.
* `present` draws a UV gradient (`shaders/present.frag`) and reads nothing,
  as a Blank pipeline project's does.

The scenario adds a `forward_opaque` pass with "Add material pass...",
letting "Create missing targets" make `scene_color` and `scene_depth`,
rewrites `present.frag` to pass its input 0 through, and drags `scene_color`
onto `present`. The box is then on screen.

The pipeline starts with no material pass at all. A pipeline with any
material pass must have one for every route a material takes (G-4): the
engine's own built-in materials and the box's glTF material are automatic,
which is `deferred_geometry` by default. The pipeline has no pass for that,
so the form offers "Draw this project's materials with forward", checked:
Save sends the pass and `material_route_default: "forward"` in one
`add_authored_pass`, and Studio restarts the engine on the same project to
apply it. The box itself is `g1_flat`, whose `render_path` is `forward`
either way.
