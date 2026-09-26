# studio_loop_view_axis

The project `test/studio_scenarios/loop_view_axis.json` opens (LP-4f1): the
same document as `studio_view_select_field` -- the engine's `hybrid_v1` preset
and a copy of the engine's `cube_capture` feature as a project feature -- so
the frame plan lists passes of the `$capture/cube` family, with the `face`
axis (length 6, labels +X .. -Z) on them. Its root graph starts empty: the
scenario puts a loop over that axis there, with Studio's controls only.

The document also holds one definition no node runs yet, `cube_face`: a
fullscreen pass of the `$capture/cube` family drawing into a 256 x 256, six
layer image of its own -- the size of the cube's other passes. The scenario
chooses it as the loop's body. (Studio's Add fullscreen pass draws into an
image the size of the screen, and the engine does not run one family at two
raster extents: `renderer.cpp` stops with "spans multiple raster extents".)

The scenario copies this directory before opening it; nothing here is written.
