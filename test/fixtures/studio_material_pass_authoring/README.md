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

Two material passes are here only because G-4 has not landed. Today a
pipeline with any material pass must have one for every route a material
takes, or loading fails: the engine's own default material routes to
`deferred_geometry`, which `g1_gbuffer` takes, and the box routes to
`forward_opaque`, which `g1_forward_probe` takes until the scenario adds its
own. Nothing reads what either writes. When G-4 lets a material with no pass
go undrawn, both can go.
