# studio_gui_rt_procedural_callable

RTL-2s b's second scenario fixture: a cube and a procedural ball (asset
`ball`, an aabb of [-1, 1] in `assets/asset_data.json`, its object scaled
0.6), a ray group layout (`passes/ray_layout.json`: ray type `primary`, the
triangles group `mesh` and the procedural group `ball`, miss `sky`) and the
assignments that put the ball's asset in group `ball`
(`passes/ray_bindings.json`). The root graph's task `trace` names
`shaders/none.rint` for the ball, an intersection that reports nothing, so
the ball is not drawn until the scenario writes a sphere from the Graph's
ray tracing fields; it then adds a callable shader.
