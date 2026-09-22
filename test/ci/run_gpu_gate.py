#!/usr/bin/env python3
"""Run GPU tests and require every shipped Studio E2E scenario.

The full gate rejects missing Studio coverage and non-allowlisted skips. Use
--engine-only for a limited check without Studio; it is not a release gate.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from skip_policy import SkipPolicyError, run_gate

NO_DEVICE_HINT = (
    "most of the suite skipped at once, which usually means no Vulkan device was "
    "available to this runner. A GPU gate on a GPU-less machine is a failure, not a pass."
)


def required_studio_tests(scenario_dir: Path) -> set[str]:
    names = {f"studio_scenario_{scenario.stem}" for scenario in scenario_dir.glob("*.json")}
    if not names:
        raise SkipPolicyError(f"no required Studio scenarios were found in {scenario_dir}")
    return names


def main(argv: list[str] | None = None) -> int:
    # Parse only this driver's selection first; the shared gate owns the rest.
    selection = argparse.ArgumentParser(add_help=False)
    selection.add_argument(
        "--engine-only", action="store_true",
        help="run the engine subset without required Studio E2E coverage; not a release gate",
    )
    args, _ = selection.parse_known_args(argv)
    parser = argparse.ArgumentParser(
        description="Run the GPU gate, including every Studio E2E scenario by default.",
        parents=[selection],
    )
    try:
        required = set() if args.engine_only else required_studio_tests(
            Path(__file__).resolve().parents[1] / "studio_scenarios"
        )
    except SkipPolicyError as error:
        print(f"GPU gate error: {error}", file=sys.stderr)
        return 2
    return run_gate(
        gate_name="GPU (engine-only)" if args.engine_only else "GPU",
        label_args=["-L", "^gpu$"] + (["-LE", "^studio_scenario$"] if args.engine_only else []),
        default_allowlist=Path(__file__).with_name("gpu_skip_allowlist.txt"),
        bulk_skip_hint=NO_DEVICE_HINT,
        argv=argv,
        required_tests=required,
        parser=parser,
    )


if __name__ == "__main__":
    raise SystemExit(main())
