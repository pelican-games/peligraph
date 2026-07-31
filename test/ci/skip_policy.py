#!/usr/bin/env python3
"""Shared exact-skip policy for the CTest gates.

A test has three outcomes, and the dangerous one is the third: a failing test is
red and gets looked at, but a skipped test stays green while not existing. So a
gate names the tests that may skip and fails on every other skip -- and fails
too when a named test never appeared, so the list cannot rot.

`run_cpu_gate.py` and `run_gpu_gate.py` are drivers over this module; they differ
only in the CTest label selection and their allowlist.
"""

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


def validate_skip_policy(
    junit_path: Path,
    allowlist_path: Path,
    gate_name: str = "CPU",
    bulk_skip_hint: str | None = None,
) -> set[str]:
    allowed = load_allowlist(allowlist_path)
    reported, skipped = read_junit(junit_path)

    stale = allowed - reported
    if stale:
        raise SkipPolicyError(
            f"allowlisted CTest names were not present in the {gate_name} gate: "
            + ", ".join(sorted(stale))
        )

    unexpected = skipped - allowed
    if unexpected:
        message = (
            f"unexpected skipped CTest names ({len(unexpected)} of {len(reported)} reported): "
            + ", ".join(sorted(unexpected))
        )
        # Every test skipping at once is a different diagnosis from one test
        # skipping, and enumerating a hundred names buries it.
        if bulk_skip_hint and len(unexpected) * 2 >= len(reported):
            message = f"{bulk_skip_hint}\n{message}"
        raise SkipPolicyError(message)
    return skipped


def run_ctest(
    ctest: str,
    build_dir: Path,
    config: str,
    label_args: list[str],
    junit_path: Path,
    log_path: Path,
) -> int:
    command = [
        ctest,
        "--test-dir",
        str(build_dir),
        "-C",
        config,
        *label_args,
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


def run_gate(
    gate_name: str,
    label_args: list[str],
    default_allowlist: Path,
    bulk_skip_hint: str | None = None,
    argv: list[str] | None = None,
) -> int:
    parser = argparse.ArgumentParser(description=f"Run the {gate_name} CTest gate.")
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", default="Debug")
    parser.add_argument("--artifacts-dir", type=Path, required=True)
    parser.add_argument("--allowlist", type=Path, default=default_allowlist)
    parser.add_argument("--ctest", default="ctest")
    args = parser.parse_args(argv)

    ctest = shutil.which(args.ctest)
    if ctest is None:
        print(f"{gate_name} gate error: ctest executable not found: {args.ctest}", file=sys.stderr)
        return 2

    build_dir = args.build_dir.resolve()
    artifacts_dir = args.artifacts_dir.resolve()
    artifacts_dir.mkdir(parents=True, exist_ok=True)
    junit_path = artifacts_dir / "ctest-junit.xml"
    log_path = artifacts_dir / "ctest.log"
    policy_path = artifacts_dir / "skip-policy.txt"

    ctest_result = run_ctest(
        ctest, build_dir, args.config, label_args, junit_path, log_path
    )
    try:
        skipped = validate_skip_policy(
            junit_path, args.allowlist.resolve(), gate_name, bulk_skip_hint
        )
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
        print(f"{gate_name} gate error: ctest exited with {ctest_result}", file=sys.stderr)
    return ctest_result
