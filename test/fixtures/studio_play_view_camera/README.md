# studio_play_view_camera

The project `test/studio_scenarios/play_view_camera.json` opens (PM-3b): the
box of `studio_render_undo_roundtrip` in front of the scene camera, and that
camera moved 0.05 along x per game step by the game code of
`studio_play_pause_step` (`test/fixtures/pm3a_mover`, built as this
scenario's game logic DLL). Once the game has run a while its camera has left
the box behind, so the paused game seen through its own camera differs from
the same moment seen through the editor camera, which starts where the scene
camera was.
