# studio_gui_rt_layout_edits

RTL-2s-c's fixture for `gui_rt_layout_edits`: the project as Enable hit groups
leaves a Blank one with a ray tracing task, and a little more to take away. A
cube above a floor; a ray group layout (`passes/ray_layout.json`) of two ray
types (`primary`, `shadow`), two triangles groups (`default`, `extra`) and two
misses (`miss`, `miss_2`), the engine's record of normals; the root graph's
task `trace` traces `primary` with `shaders/ray_tracing_layout.*` (the layout's
starting set) in every place. `shaders/ray_tracing.*` beside it is the set the
task ran before Enable (no ray layout), which Disable hit groups goes back to.
The scenario moves a ray type, removes the extra group, miss and ray type, and
disables hit groups.
