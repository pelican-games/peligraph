# Devstudio frame-plan fixture

`example_frame_plan.json` is the unmodified JSON value returned in `result`
by `get_frame_plan` after one `step_frame`. It was captured from
`C:/src/pelican2/projects/example` with a Debug
`pelican_player` using `--headless --rpc --frames 0 --size 160x90 --fps 30`.

This capture predates the additive `runtime_resolution` section. The fixture
test supplies that section through the producer's production extent resolver,
using the captured 160x90 runtime output. Current `get_frame_plan` responses
publish the concrete integer extents directly; Studio does not reconstruct
them from `display` or repeat the producer's floating-point arithmetic.

The fixture is the complete response value, not a hand-written or sliced
projection. Tests make their negative-control variants by editing the parsed
copy in memory.
