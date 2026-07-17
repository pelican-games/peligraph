#!/usr/bin/env python3
"""Generate and validate the GPU-free golden fixture inventory."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


SCHEMA = "pelican.golden_inventory"
VERSION = 1
MANIFEST_RELATIVE_PATH = Path("test/golden/inventory.json")
GOLDEN_RELATIVE_PATH = Path("test/golden")
TRACE_SOURCES = {
    "canonical_frame_plan": "test/fixtures/canonical_frame_plan_trace.txt",
    "renderer_execution": "test/fixtures/renderer_execution_traces.json",
    "rgba8": "test/fixtures/wp73_rgba8_hashes.json",
}
VAT_ANY = "on_and_off"
VAT_ON_ONLY = "on_only"


class InventoryError(RuntimeError):
    """The repository cannot be represented as a valid golden inventory."""


@dataclass(frozen=True)
class Snapshot:
    inventory: dict[str, Any]
    trace_cases: dict[str, set[str]]
    issues: list[str]


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def _read_json_trace_cases(path: Path, label: str, issues: list[str]) -> set[str]:
    try:
        value = _load_json(path)
    except (OSError, json.JSONDecodeError) as error:
        issues.append(f"trace {label}: cannot read {path.as_posix()}: {error}")
        return set()
    if not isinstance(value, dict):
        issues.append(f"trace {label}: root must be a JSON object: {path.as_posix()}")
        return set()
    return set(value)


def _read_frame_plan_trace_cases(path: Path, issues: list[str]) -> set[str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        issues.append(f"trace canonical_frame_plan: cannot read {path.as_posix()}: {error}")
        return set()

    names: set[str] = set()
    for line_number, line in enumerate(lines, 1):
        if not line:
            issues.append(
                f"trace canonical_frame_plan: blank line at {path.as_posix()}:{line_number}"
            )
            continue
        name, separator, _ = line.partition(": ")
        if not separator or not name:
            issues.append(
                f"trace canonical_frame_plan: malformed line at "
                f"{path.as_posix()}:{line_number}"
            )
            continue
        if name in names:
            issues.append(f"trace canonical_frame_plan: duplicate case: {name}")
        names.add(name)
    return names


def _read_trace_cases(repo_root: Path, issues: list[str]) -> dict[str, set[str]]:
    return {
        "canonical_frame_plan": _read_frame_plan_trace_cases(
            repo_root / TRACE_SOURCES["canonical_frame_plan"], issues
        ),
        "renderer_execution": _read_json_trace_cases(
            repo_root / TRACE_SOURCES["renderer_execution"],
            "renderer_execution",
            issues,
        ),
        "rgba8": _read_json_trace_cases(
            repo_root / TRACE_SOURCES["rgba8"], "rgba8", issues
        ),
    }


def snapshot_repository(repo_root: Path) -> Snapshot:
    repo_root = repo_root.resolve()
    golden_root = repo_root / GOLDEN_RELATIVE_PATH
    issues: list[str] = []
    trace_cases = _read_trace_cases(repo_root, issues)

    if not golden_root.is_dir():
        raise InventoryError(f"golden directory does not exist: {golden_root}")

    root_files = sorted(
        path.name for path in golden_root.iterdir() if path.is_file()
    )
    if MANIFEST_RELATIVE_PATH.name not in root_files:
        root_files.append(MANIFEST_RELATIVE_PATH.name)
        root_files.sort()

    cases: list[dict[str, Any]] = []
    for case_root in sorted(
        (path for path in golden_root.iterdir() if path.is_dir()), key=lambda path: path.name
    ):
        files = sorted(
            path.relative_to(case_root).as_posix()
            for path in case_root.rglob("*")
            if path.is_file()
        )
        config_path = case_root / "case.json"
        mode: str | None = None
        try:
            config = _load_json(config_path)
            if not isinstance(config, dict) or not isinstance(config.get("mode"), str):
                issues.append(f"case {case_root.name}: case.json must contain string mode")
            else:
                mode = config["mode"]
        except (OSError, json.JSONDecodeError) as error:
            issues.append(f"case {case_root.name}: cannot read case.json: {error}")

        expected_path = case_root / "expected.png"
        expected_sha256: str | None = None
        if expected_path.is_file():
            expected_sha256 = _sha256(expected_path)
        else:
            issues.append(f"case {case_root.name}: expected.png is missing")

        cases.append(
            {
                "name": case_root.name,
                "mode": mode,
                "files": files,
                "expected_png_sha256": expected_sha256,
                "tolerance": (case_root / "tolerance.json").is_file(),
                "vat": VAT_ON_ONLY if mode == "vat_playback" else VAT_ANY,
                "traces": sorted(
                    label for label, names in trace_cases.items() if case_root.name in names
                ),
            }
        )

    inventory = {
        "schema": SCHEMA,
        "version": VERSION,
        "root_files": root_files,
        "trace_sources": TRACE_SOURCES,
        "cases": cases,
    }
    return Snapshot(inventory, trace_cases, issues)


def _load_manifest(path: Path) -> dict[str, Any]:
    try:
        value = _load_json(path)
    except (OSError, json.JSONDecodeError) as error:
        raise InventoryError(f"cannot read inventory manifest {path}: {error}") from error
    if not isinstance(value, dict):
        raise InventoryError(f"inventory manifest root must be an object: {path}")
    return value


def _index_cases(
    cases: Any, source: str, issues: list[str]
) -> dict[str, dict[str, Any]]:
    if not isinstance(cases, list):
        issues.append(f"{source}: cases must be an array")
        return {}
    indexed: dict[str, dict[str, Any]] = {}
    for index, case in enumerate(cases):
        if not isinstance(case, dict) or not isinstance(case.get("name"), str):
            issues.append(f"{source}: cases[{index}] must contain string name")
            continue
        name = case["name"]
        if name in indexed:
            issues.append(f"{source}: duplicate case: {name}")
            continue
        indexed[name] = case
    return indexed


def _as_string_set(value: Any, field: str, issues: list[str]) -> set[str]:
    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
        issues.append(f"{field} must be an array of strings")
        return set()
    if len(value) != len(set(value)):
        issues.append(f"{field} contains duplicate entries")
    return set(value)


def validate_inventory(repo_root: Path, manifest_path: Path | None = None) -> list[str]:
    repo_root = repo_root.resolve()
    manifest_path = manifest_path or repo_root / MANIFEST_RELATIVE_PATH
    expected = _load_manifest(manifest_path)
    actual = snapshot_repository(repo_root)
    issues = list(actual.issues)

    if expected.get("schema") != SCHEMA:
        issues.append(f"manifest schema must be {SCHEMA!r}")
    if expected.get("version") != VERSION:
        issues.append(f"manifest version must be {VERSION}")
    if expected.get("trace_sources") != TRACE_SOURCES:
        issues.append("manifest trace_sources do not match the supported exact set")

    expected_roots = _as_string_set(expected.get("root_files"), "root_files", issues)
    actual_roots = set(actual.inventory["root_files"])
    for name in sorted(expected_roots - actual_roots):
        issues.append(f"missing golden root file: {name}")
    for name in sorted(actual_roots - expected_roots):
        issues.append(f"unknown golden root file: {name}")

    expected_cases = _index_cases(expected.get("cases"), "manifest", issues)
    actual_cases = _index_cases(actual.inventory["cases"], "repository", issues)
    for name in sorted(set(expected_cases) - set(actual_cases)):
        issues.append(f"missing golden case directory: {name}")
    for name in sorted(set(actual_cases) - set(expected_cases)):
        issues.append(f"unknown golden case directory: {name}")

    for name in sorted(set(expected_cases) & set(actual_cases)):
        wanted = expected_cases[name]
        observed = actual_cases[name]
        wanted_files = _as_string_set(wanted.get("files"), f"case {name}: files", issues)
        observed_files = set(observed["files"])
        for path in sorted(wanted_files - observed_files):
            issues.append(f"case {name}: missing file: {path}")
        for path in sorted(observed_files - wanted_files):
            issues.append(f"case {name}: unknown file: {path}")

        for field in ("mode", "expected_png_sha256", "tolerance", "vat"):
            if wanted.get(field) != observed.get(field):
                issues.append(
                    f"case {name}: {field} mismatch: manifest={wanted.get(field)!r}, "
                    f"repository={observed.get(field)!r}"
                )

        wanted_traces = _as_string_set(
            wanted.get("traces"), f"case {name}: traces", issues
        )
        observed_traces = set(observed["traces"])
        for trace in sorted(wanted_traces - observed_traces):
            issues.append(f"case {name}: missing trace membership: {trace}")
        for trace in sorted(observed_traces - wanted_traces):
            issues.append(f"case {name}: unknown trace membership: {trace}")

    manifest_names = set(expected_cases)
    for label, names in actual.trace_cases.items():
        for name in sorted(names - manifest_names):
            issues.append(f"trace {label}: unknown golden case: {name}")
    return issues


def write_inventory(repo_root: Path, manifest_path: Path | None = None) -> Path:
    repo_root = repo_root.resolve()
    manifest_path = manifest_path or repo_root / MANIFEST_RELATIVE_PATH
    snapshot = snapshot_repository(repo_root)
    if snapshot.issues:
        raise InventoryError("cannot update invalid inventory:\n" + "\n".join(snapshot.issues))
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(
        json.dumps(snapshot.inventory, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    return manifest_path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    parser.add_argument(
        "--manifest",
        type=Path,
        help="override the manifest path (primarily for fixture tests)",
    )
    parser.add_argument(
        "--update",
        action="store_true",
        help="rewrite the manifest deterministically from the current repository",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    manifest_path = args.manifest.resolve() if args.manifest else None
    try:
        if args.update:
            written = write_inventory(args.repo_root, manifest_path)
            print(f"golden inventory updated: {written}")
            return 0
        issues = validate_inventory(args.repo_root, manifest_path)
    except InventoryError as error:
        print(f"golden inventory: FAIL\n{error}", file=sys.stderr)
        return 2

    if issues:
        print("golden inventory: FAIL", file=sys.stderr)
        for issue in issues:
            print(f"- {issue}", file=sys.stderr)
        return 1
    print("golden inventory: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
