"""Prefix only discovered [gpu] CTest commands with the lease launcher.

Catch2's ``CatchAddTests.cmake`` writes one ``add_test(...)`` line and one
``set_tests_properties(... PROPERTIES ...)`` line per discovered case.  Each
argument is emitted either as a bare token (when it contains nothing outside
``[-./:a-zA-Z0-9_]``) or as a ``[==[bracket argument]==]``.  Both forms occur
in practice -- the test name is usually bracketed because it has spaces, while
``LABELS gpu`` is bare -- so this rewriter tokenises instead of pattern
matching one of the two spellings.
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

BRACKET = re.compile(r"\[==\[(?P<value>.*?)\]==\]", re.DOTALL)
BARE = re.compile(r"[^\s()]+")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--file", type=Path)
    parser.add_argument("--directory", type=Path)
    parser.add_argument("--target")
    parser.add_argument("--launcher", required=True)
    args = parser.parse_args()
    if args.file:
        files = [args.file]
    else:
        if not args.directory or not args.target:
            parser.error("provide --file or --directory with --target")
        # POST_BUILD discovery writes "<target>-<hash>_tests.cmake"; the
        # PRE_TEST multi-config lane would write "..._tests-<config>.cmake".
        # Accept both so the rewriter cannot silently match nothing.
        files = sorted(
            set(args.directory.glob(f"{args.target}-*_tests.cmake"))
            | set(args.directory.glob(f"{args.target}-*_tests-*.cmake")))
    if not files:
        raise SystemExit(
            f"patch_catch_gpu_lease_tests: no discovery output for {args.target} "
            f"in {args.directory}")
    for path in files:
        if not path.exists():
            continue  # A disabled/empty Catch discovery lane has no output.
        patch(path, args.launcher)
    return 0


def _next_argument(line: str, start: int) -> tuple[str, int, int]:
    """Return (value, begin, end) of the first argument at or after ``start``."""
    while start < len(line) and line[start].isspace():
        start += 1
    bracket = BRACKET.match(line, start)
    if bracket:
        return bracket.group("value"), start, bracket.end()
    bare = BARE.match(line, start)
    if not bare:
        return "", start, start
    return bare.group(0), start, bare.end()


def _arguments(line: str, start: int) -> list[str]:
    values: list[str] = []
    position = start
    while position < len(line):
        while position < len(line) and line[position].isspace():
            position += 1
        if position >= len(line) or line[position] == ")":
            break
        value, _, position = _next_argument(line, position)
        values.append(value)
    return values


def _is_gpu(properties: list[str]) -> bool:
    for index, value in enumerate(properties):
        if value == "LABELS" and index + 1 < len(properties):
            return "gpu" in properties[index + 1].split(";")
    return False


def patch(path: Path, launcher: str) -> None:
    lines = path.read_text(encoding="utf-8").splitlines(keepends=True)
    gpu_tests: set[str] = set()
    for line in lines:
        if not line.startswith("set_tests_properties("):
            continue
        name, _, after = _next_argument(line, len("set_tests_properties("))
        if _is_gpu(_arguments(line, after)):
            gpu_tests.add(name)
    prefix = f" [==[{launcher}]==] [==[--]==]"
    patched: list[str] = []
    for line in lines:
        if line.startswith("add_test("):
            name, _, after = _next_argument(line, len("add_test("))
            if name in gpu_tests and not line[after:].startswith(prefix):
                line = line[:after] + prefix + line[after:]
        patched.append(line)
    path.write_text("".join(patched), encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
