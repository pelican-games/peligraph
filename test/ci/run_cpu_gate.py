#!/usr/bin/env python3
"""Run the GPU-free CTest gate and reject every non-allowlisted skip."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


class SkipPolicyError(RuntimeError):
    """The CTest report or exact-name skip policy is invalid."""


def load_allowlist(path: Path) -> set[str]:
    names: set[str] = set()
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        name = raw_line.strip()
        if not name or name.startswith("#"):
            continue
        if any(character in name for character in "*?["):
            raise SkipPolicyError(
                f"{path}:{line_number}: wildcard syntax is forbidden; use an exact CTest name"
            )
        if name in names:
            raise SkipPolicyError(f"{path}:{line_number}: duplicate allowlist entry: {name}")
        names.add(name)
    return names


def _local_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def read_junit(path: Path) -> tuple[set[str], set[str]]:
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as error:
        raise SkipPolicyError(f"cannot read CTest JUnit report {path}: {error}") from error

    reported: set[str] = set()
    skipped: set[str] = set()
    for test_case in root.iter():
        if _local_name(test_case.tag) != "testcase":
            continue
        name = test_case.attrib.get("name", "").strip()
        if not name:
            raise SkipPolicyError(f"CTest JUnit report {path} contains an unnamed testcase")
        reported.add(name)
        has_skipped_element = any(
            _local_name(child.tag) == "skipped" for child in test_case
        )
        status = test_case.attrib.get("status", "").lower()
        if has_skipped_element or status in {"notrun", "skipped"}:
            skipped.add(name)

    if not reported:
        raise SkipPolicyError(f"CTest JUnit report {path} contains no testcases")
    return reported, skipped


def validate_skip_policy(junit_path: Path, allowlist_path: Path) -> set[str]:
    allowed = load_allowlist(allowlist_path)
    reported, skipped = read_junit(junit_path)

    stale = allowed - reported
    if stale:
        raise SkipPolicyError(
            "allowlisted CTest names were not present in the CPU gate: "
            + ", ".join(sorted(stale))
        )

    unexpected = skipped - allowed
    if unexpected:
        raise SkipPolicyError(
            "unexpected skipped CTest names: " + ", ".join(sorted(unexpected))
        )
    return skipped


def run_ctest(
    ctest: str,
    build_dir: Path,
    config: str,
    junit_path: Path,
    log_path: Path,
) -> int:
    command = [
        ctest,
        "--test-dir",
        str(build_dir),
        "-C",
        config,
        "-LE",
        "gpu",
        "--output-on-failure",
        "--no-tests=error",
        "--output-junit",
        str(junit_path),
    ]
    with log_path.open("w", encoding="utf-8", newline="") as log:
        log.write("command: " + subprocess.list2cmdline(command) + "\n\n")
        log.flush()
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        assert process.stdout is not None
        for line in process.stdout:
            sys.stdout.write(line)
            log.write(line)
        return process.wait()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", default="Debug")
    parser.add_argument("--artifacts-dir", type=Path, required=True)
    parser.add_argument(
        "--allowlist",
        type=Path,
        default=Path(__file__).with_name("cpu_skip_allowlist.txt"),
    )
    parser.add_argument("--ctest", default="ctest")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    ctest = shutil.which(args.ctest)
    if ctest is None:
        print(f"CPU gate error: ctest executable not found: {args.ctest}", file=sys.stderr)
        return 2

    build_dir = args.build_dir.resolve()
    artifacts_dir = args.artifacts_dir.resolve()
    artifacts_dir.mkdir(parents=True, exist_ok=True)
    junit_path = artifacts_dir / "ctest-junit.xml"
    log_path = artifacts_dir / "ctest.log"
    policy_path = artifacts_dir / "skip-policy.txt"

    ctest_result = run_ctest(ctest, build_dir, args.config, junit_path, log_path)
    try:
        skipped = validate_skip_policy(junit_path, args.allowlist.resolve())
        summary = (
            "SKIP exact policy: PASS\n"
            + f"observed allowlisted skips: {len(skipped)}\n"
            + "\n".join(sorted(skipped))
            + ("\n" if skipped else "")
        )
        policy_path.write_text(summary, encoding="utf-8")
        print(summary, end="")
    except SkipPolicyError as error:
        summary = f"SKIP exact policy: FAIL\n{error}\n"
        policy_path.write_text(summary, encoding="utf-8")
        print(summary, file=sys.stderr, end="")
        return 2

    if ctest_result != 0:
        print(f"CPU gate error: ctest exited with {ctest_result}", file=sys.stderr)
    return ctest_result


if __name__ == "__main__":
    raise SystemExit(main())
