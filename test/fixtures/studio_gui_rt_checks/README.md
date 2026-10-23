# studio_gui_rt_checks

RTL-3s's scenario fixture (`gui_rt_checks`): a cube and a procedural ball
(asset `ball`, its record `value`), a ray group layout
(`passes/ray_layout.json`: ray types `primary` and `shadow`, the triangles
group `mesh` and the procedural group `ball`, misses `sky` and
`shadow_miss`, the engine's record of normals and the record field `value`)
and the root graph's ray tracing task `trace`, which traces `primary` only and
calls `shaders/tint.rcall` through a callable record (`strength`). The root
declares a named TLAS `actors` (instance mask 1) for the Frame Plan's table of
each TLAS's BLAS.

`shaders/start.rgen` has the two switches of Studio's raygen template, both 0.
`shaders/start.rchit` holds its record in the block Studio writes again.
`shaders/wrong_record.rchit` declares a record that is not the layout's: the
scenario puts it in a traced cell to have the engine refuse it.
