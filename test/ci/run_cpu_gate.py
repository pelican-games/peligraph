#!/usr/bin/env python3
"""Run the GPU-free CTest gate and reject every non-allowlisted skip."""

from __future__ import annotations

from pathlib import Path

from skip_policy import run_gate

if __name__ == "__main__":
    raise SystemExit(
        run_gate(
            gate_name="CPU",
            label_args=["-LE", "gpu"],
            default_allowlist=Path(__file__).with_name("cpu_skip_allowlist.txt"),
        )
    )
