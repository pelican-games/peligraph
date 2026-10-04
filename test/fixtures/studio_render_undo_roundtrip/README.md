# studio_render_undo_roundtrip

The project `test/studio_scenarios/render_undo_roundtrip.json` opens. It is
`studio_material_loop_decompose` under its own name: a direct graph root whose
rendering section declares a `forward_opaque` material pass writing
`scene_color` and `scene_depth`, and a `present` that shows `scene_color`
through `engine://passthrough`, over the one flat-tinted box of
`studio_material_pass_authoring`.

It adds `shaders/ru1_invert.frag`, a fragment that draws `scene_color`
inverted. The scenario picks it for `present` in the Pass Editor (a change the
viewport shows), takes it back with Undo and puts it again with Redo; then
decomposes `forward_opaque` into a loop and takes that back and puts it again;
then the Pass Editor's New pass places a fullscreen pass on the Graph tab
(GL-5f: in the graph's nodes, no fragment under `passes/authoring/`), which the
Graph tab's Undo takes back and its Redo puts again. Last, a Pass Editor edit of `present`
is followed by moving `present` on the Frame Plan, which rewrites the root's
`layout`; Undo still takes the edit back and keeps the placement.
