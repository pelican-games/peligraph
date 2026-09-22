#!/usr/bin/env python3
"""Run Studio's GPU-free suite with the compiler-OFF CI skip policy."""

from pathlib import Path

from skip_policy import run_gate

if __name__ == "__main__":
    raise SystemExit(
        run_gate(
            gate_name="Studio",
            label_args=["-L", "^studio_cpu$"],
            default_allowlist=Path(__file__).with_name("studio_skip_allowlist.txt"),
        )
    )
