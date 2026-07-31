#!/usr/bin/env python3
"""Run the `gpu`-labelled CTest gate and reject every non-allowlisted skip.

The counterpart to `run_cpu_gate.py`. CI0 runs `ctest -LE gpu`, so everything
this gate covers -- golden image comparison, validation layers, the player
process integrations -- has only ever run on someone's machine, once. Running it
on a machine without a Vulkan device is a failure, not a pass: `ci.md` puts the
fail-on-no-GPU responsibility here rather than letting the suite evaporate into
skips.
"""

from __future__ import annotations

from pathlib import Path

from skip_policy import run_gate

NO_DEVICE_HINT = (
    "most of the suite skipped at once, which usually means no Vulkan device was "
    "available to this runner. A GPU gate on a GPU-less machine is a failure, not a pass."
)

if __name__ == "__main__":
    raise SystemExit(
        run_gate(
            gate_name="GPU",
            label_args=["-L", "gpu"],
            default_allowlist=Path(__file__).with_name("gpu_skip_allowlist.txt"),
            bulk_skip_hint=NO_DEVICE_HINT,
        )
    )
