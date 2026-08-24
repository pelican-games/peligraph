# Devstudio frame-plan fixture

`example_frame_plan.json` is the unmodified JSON value returned in `result`
by `get_frame_plan` after one `step_frame`. It was captured from the
repository `projects/example` configuration with a Debug `pelican_player`
using `--headless --rpc --frames 0 --size 160x90 --fps 30`.

This capture includes the additive `runtime_resolution` and resolved resource
`usage` sections published by the current engine. The concrete integer extents
come directly from the 160x90 runtime output; Studio does not reconstruct them
from `display` or repeat the producer's floating-point arithmetic.

The fixture is the complete response value, not a hand-written or sliced
projection. Tests make their negative-control variants by editing the parsed
copy in memory.
