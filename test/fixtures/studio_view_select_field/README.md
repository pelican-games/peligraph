# studio_view_select_field

The project `test/studio_scenarios/view_select_field.json` opens (LP-4b): the
engine's `hybrid_v1` preset, and one feature -- a copy of the engine's
`cube_capture` feature, referenced as `project://features/cube_capture.json`.
(`cube_capture` inherits bindings from the preset's `deferred_lighting`, so it
needs a pipeline that has one.)

The copy has no `parameters` (its `$resolution` is written out as 256; the
cube's position and distances take the provider's defaults): the engine does
not offer a parameterized feature's passes for editing. So the scenario can
open `cube_capture_ssao_blur` -- a fullscreen pass of the `$capture/cube`
family -- in the Pass Editor and choose its face in the View field.

The scenario copies this directory before opening it; nothing here is written.
