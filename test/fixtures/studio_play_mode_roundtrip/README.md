# studio_play_mode_roundtrip

The project `test/studio_scenarios/play_mode_roundtrip.json` opens (PM-3b). It
is `studio_render_undo_roundtrip` -- a `present` pass the Pass Editor can switch
to `shaders/ru1_invert.frag`, a change the viewport shows (the background
turns its inverse) -- with the game code of `studio_play_pause_step`
(`test/fixtures/pm3a_mover`, built as this scenario's game logic DLL) moving
`Mover` a quarter unit along x per game step, which says whether the game runs
and what Stop and Take from game leave. `Sun` is a directional light whose
`intensity` the scenario edits in the Inspector while the game runs: once put
back by Stop, once kept.

The scene draws no model: Stop with `keep` in a windowed engine whose scene
holds a model instance asserts in `ModelInstanceSlots::releaseWithheld`
(engine, PM-2; reported with PM-3b). The camera view is judged in
`studio_play_view_camera`, which stops without keeping anything.
