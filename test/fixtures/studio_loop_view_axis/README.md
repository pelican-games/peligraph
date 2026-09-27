# studio_loop_view_axis

The project `test/studio_scenarios/loop_view_axis.json` opens (LP-4f1): the
same document as `studio_view_select_field` -- the engine's `hybrid_v1` preset
and a copy of the engine's `cube_capture` feature as a project feature -- so
the frame plan lists passes of the `$capture/cube` family, with the `face`
axis (length 6, labels +X .. -Z) on them. Its root graph starts empty: the
scenario puts a loop over that axis there, with Studio's controls only.

Its graph holds no definition either: the loop's body is the new, empty one
Add loop makes, and the pass in it is Studio's Add fullscreen pass. In the
body of a loop over the cube's axis, that pass draws into an image made at the
size the cube's family draws at -- 256 x 256, six layers, as the frame plan's
`family_extent` / `family_layers` on the family's nodes say (LP-4f3, VF-2) --
because the engine does not
run one family at two raster extents (render.view_family_extent_mismatch@1,
VF-1).

The scenario copies this directory before opening it; nothing here is written.
