# studio_play_mode_roundtrip

The project `test/studio_scenarios/play_mode_roundtrip.json` opens (PM-3b,
PM-3d). It is `studio_render_undo_roundtrip` -- a `present` pass the Pass
Editor can switch to `shaders/ru1_invert.frag`, a change the viewport shows
(the background turns its inverse) -- with the game code of
`studio_play_pause_step` (`test/fixtures/pm3a_mover`, built as this scenario's
game logic DLL) on two objects:

- `Mover` goes a quarter unit along x per game step, which says whether the
  game runs and what Stop and Take from game leave.
- The scene camera goes 0.05 along x per game step. `CheckerBox` (a model, lit
  by nothing, so its colour does not depend on the light) stands in front of
  where it starts. Once the game has run a while its camera has left the box
  behind, so the paused game seen through its own camera differs from the same
  moment seen through the editor camera, which starts where the scene camera
  was.

`Sun` is a directional light whose `intensity` the scenario edits in the
Inspector while the game runs: once put back by Stop, once kept. The kept one
is a Stop with `keep` in a scene that draws a model, which asserted in
`ModelInstanceSlots::releaseWithheld` until PM-2b; the scenario lets the engine
draw 30 more frames after that Stop before it reads the value.
