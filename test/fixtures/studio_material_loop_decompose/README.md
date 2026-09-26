# studio_material_loop_decompose

The project `test/studio_scenarios/material_loop_decompose.json` opens: a
graph root that selects no preset (a direct config), whose own rendering
section declares a `forward_opaque` material pass writing `scene_color` and
`scene_depth`, and a `present` that shows `scene_color` through
`engine://passthrough`. `material_route_default` is `forward`, so every
material of the project (and the engine's built-in ones) is drawn by that pass.

The model and its material are `studio_material_pass_authoring`'s:
`checker_box.glb` (the box `projects/pathtrace_demo` ships) bound to
`g1_flat` (`materials/g1_flat.material.json`), whose surface returns one flat
tint -- the box is one colour against the cleared background.

The pass is declared in the root itself, not in a feature, so
`decompose_material_pass` takes it with the root's digest alone (LP-3b): the
scenario decomposes it, checks the picture has not changed, then gives the
box's material a branch of its own that draws nothing, and checks the box is
gone.
