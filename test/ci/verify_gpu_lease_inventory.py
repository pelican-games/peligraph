"""WP385 inventory gate: the four GPU lease mechanisms agree, test by test.

Decision 6 makes the inventory an equivalence, checked per CTest test on the
generated `ctest --show-only=json-v1` report:

    label `gpu`  <=>  RESOURCE_LOCK=pelican_gpu  <=>  command starts with the
    lease launcher  <=>  TIMEOUT = lease_timeout_max + intrinsic + grace

`intrinsic` comes from the one generated timeout config
(`test/gpu_lease_config.cmake` -> `gpu_lease_timeouts.json`), never from a
value repeated in this script.

Removing the launcher from a single test, dropping its lock or its label, or
changing one timeout term makes this gate red and names the test.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

LOCK = "pelican_gpu"
LABEL = "gpu"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--launcher", required=True)
    parser.add_argument("--timeouts", type=Path, required=True)
    parser.add_argument("--ctest", default="ctest")
    parser.add_argument("--config")
    args = parser.parse_args()

    policy = json.loads(args.timeouts.read_text(encoding="utf-8"))
    lease_max = policy["lease_timeout_max"]
    grace = policy["grace"]
    default_intrinsic = policy["default_intrinsic"]
    intrinsic_by_test = policy["intrinsic"]

    command = [args.ctest, "--test-dir", str(args.build_dir), "--show-only=json-v1"]
    if args.config:
        command += ["-C", args.config]
    report = subprocess.run(command, check=True, capture_output=True, text=True)
    tests = json.loads(report.stdout)["tests"]

    launcher_name = Path(args.launcher).name
    errors: list[str] = []
    counts = {"gpu": 0, "lock": 0, "wrapped": 0, "total": len(tests)}
    for test in tests:
        properties = {p["name"]: p["value"] for p in test.get("properties", [])}
        name = test["name"]
        command_line = test.get("command", [])
        labelled = LABEL in (properties.get("LABELS") or [])
        locked = properties.get("RESOURCE_LOCK") == [LOCK]
        wrapped = bool(command_line) and Path(command_line[0]).name == launcher_name
        counts["gpu"] += labelled
        counts["lock"] += locked
        counts["wrapped"] += wrapped
        if len({labelled, locked, wrapped}) != 1:
            errors.append(
                f"{name}: gpu label / RESOURCE_LOCK={LOCK} / launcher command must agree, got "
                f"label={labelled} lock={locked} launcher={wrapped} (command={command_line!r}, "
                f"lock={properties.get('RESOURCE_LOCK')!r})")
            continue
        if not labelled:
            continue
        intrinsic = intrinsic_by_test.get(name, default_intrinsic)
        expected = lease_max + intrinsic + grace
        if properties.get("TIMEOUT") != expected:
            errors.append(
                f"{name}: TIMEOUT must be lease_timeout_max({lease_max}) + "
                f"intrinsic({intrinsic}) + grace({grace}) = {expected}, got "
                f"{properties.get('TIMEOUT')!r}")

    if errors:
        print("WP385 gpu lease inventory mismatch:", *errors, sep="\n", file=sys.stderr)
        return 1
    if counts["gpu"] == 0:
        print("WP385 gpu lease inventory: no gpu-labelled test was discovered at all",
              file=sys.stderr)
        return 1
    print(f"WP385 gpu lease inventory: {counts['gpu']} gpu tests, {counts['lock']} locked, "
          f"{counts['wrapped']} launcher-wrapped of {counts['total']} tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
