# Devstudio frame-plan fixture

`example_frame_plan.json` is the unmodified JSON value returned in `result`
by `get_frame_plan` after one `step_frame`. It was captured from
`C:/src/pelican2/projects/example` with a Debug
`pelican_player` using `--headless --rpc --frames 0 --size 160x90 --fps 30`.

For Studio extent reconstruction, the fixed `display` target (160x90) is the
canonical output extent. An `output_relative` target resolves each dimension
by multiplying that output dimension by `scale_x` / `scale_y` and truncating
the positive floating-point result to an integer, matching the runtime rule.

The fixture is the complete response value, not a hand-written or sliced
projection. Tests make their negative-control variants by editing the parsed
copy in memory.
