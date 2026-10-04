# studio_gui_rt_two_rays_two_misses

RTL-2s b's scenario fixture: a Blank-like project with a cube above a floor,
a ray group layout (`passes/ray_layout.json`: ray types `primary` and
`shadow`, one triangles group `mesh`, misses `sky` and `shadow`, the
engine's record of normals) and one ray tracing task of the root graph,
`trace`, in the layout's form: it traces `primary` only, with the starting
shaders `shaders/start.*` in every place (both misses included, as the
layout asks for one per miss). The scenario traces `shadow` too and fills
its places from the Graph's ray tracing fields.
