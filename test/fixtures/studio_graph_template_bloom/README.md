# studio_graph_template_bloom

The project `test/studio_scenarios/graph_template_bloom.json` opens (LP-4f2):
`studio_automation_graph`'s rendering document as it is -- a graph root with a
direct rendering section (no preset) whose features list
`engine://features/bloom.json` -- and a scene with something for bloom to
spread. `checker_box.glb` (the box `projects/pathtrace_demo` ships) is bound to
`g1_glow` (`materials/g1_glow.material.json`), the engine's OpenPBR opaque
surface with an emission well past bloom's threshold: the box is white with a
halo bloom spreads around it, so the picture bloom makes is not the background
alone. (A project surface would route to a forward pass, which this deferred
document does not have.)

The scenario replaces the bloom feature with the engine's
`engine://graphs/bloom_loop.json` from Render Features (Replace with loop
version...), checks the picture is the one bloom drew, then takes it back with
Undo and puts it again with Redo. The feature is in the root itself, so
`apply_graph_template` goes with the root's digest alone (GT-1).

The scenario copies this directory before opening it; nothing here is written.
