#!/usr/bin/env -S uv run --quiet --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Keep `#L<line>` links in docs/ pointing at what they were written to point at.

    uv run tools/doclink.py check            # verify; non-zero exit if anything is stale
    uv run tools/doclink.py update           # rewrite stale line numbers, refresh the ledger
    uv run tools/doclink.py update --staged  # only docs in the git index (pre-commit hook)

Source line numbers rot on every refactor, and a rotted link is worse than a missing one
because it silently points at unrelated code. Anchors, not line numbers, are the durable
thing; the line number is a cache of where the anchor currently lives.

Two tiers of anchor:

  symbol  the link text is already a C++ identifier (`Foo::bar()`), so the anchor is its
          definition, relocated by scoring lines for definition-ness.
  ledger  the link text carries no symbol (a file name, prose, a bare `#L123`), so
          docs/link_anchors.json remembers the target line's text and the anchor is that
          text.

When neither tier can decide, nothing is rewritten and the link is named. uv provisions the
interpreter, so no system Python is required.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

SCHEMA = "pelican.doc_link_anchors"
VERSION = 1
LEDGER_RELATIVE_PATH = Path("docs/link_anchors.json")
DOC_DIRS = (Path("docs/source-code-guide"), Path("docs/manual"))

# Repo-specific declaration idioms. A generic C++ parser misses all of these, which is the
# whole reason this tool is hand-written instead of borrowed.
DECL_MACROS = ("DECLARE_MODULE", "PELICAN_DEFINE_HANDLE", "PELICAN_REGISTER_EVENT")

LINK_RE = re.compile(r"\[((?:[^\[\]]|\[[^\]]*\])*)\]\(([^)\s]+?)#L(\d+)\)")

# Adoption takes the author's word that the link already points somewhere meaningful. When
# the target line carries no meaning at all, that word is almost certainly stale -- the code
# moved and the number stayed. Worth surfacing, not worth blocking on.
MEANINGLESS_LINE_RE = re.compile(r"^\s*(\}\s*;?|\{|\)\s*;?|#pragma once|namespace\s*\{|else\s*\{?)?\s*$")

# Above this many exact matches, a ledger anchor is boilerplate rather than an identifier
# and relocating by proximity is a coin flip dressed up as a decision.
MAX_ANCHOR_OCCURRENCES = 4

# Text that is a path or a file name refers to the file, not to a symbol inside it.
FILE_TEXT_RE = re.compile(r"[/\\]|\.(hpp|cpp|h|json|txt|py|cmake|md|frag|vert|comp|js)$", re.IGNORECASE)
CJK_RE = re.compile(r"[　-鿿＀-￯]")
IDENTIFIER_RE = re.compile(r"^[A-Za-z_~][A-Za-z0-9_]*$")


class DocLinkError(RuntimeError):
    """The repository cannot be processed as a set of anchored documentation links."""


@dataclass(frozen=True)
class Symbol:
    key: str
    qualifier: str | None


@dataclass
class Link:
    doc: str
    raw_text: str
    rel: str
    recorded_line: int
    match_start: int
    match_text: str
    key: str
    tier: str = ""
    status: str = ""
    line: int | None = None
    confidence: str = ""
    reason: str = ""
    anchor_text: str | None = None
    suspect: bool = False


@dataclass
class Ledger:
    anchors: dict[str, str] = field(default_factory=dict)


def _normalize(text: str) -> str:
    return re.sub(r"\s+", " ", text).strip()


def _is_meaningless(line: str | None) -> bool:
    return line is None or bool(MEANINGLESS_LINE_RE.match(line))


# --------------------------------------------------------------------- anchor derivation


def derive_symbol(raw_text: str) -> Symbol | None:
    """Pull a C++ identifier out of markdown link text, or return None if there is none."""
    text = raw_text.strip().strip("`").strip()
    if not text:
        return None
    if FILE_TEXT_RE.search(text):
        return None
    if CJK_RE.search(text):
        return None
    if re.search(r"#L\d", text):
        return None
    key = re.sub(r"\s*\([^)]*\)\s*$", "", text)
    key = re.sub(r"<[^>]*>", "", key)
    key = re.sub(r"^(class|struct|enum|namespace|using|typedef)\s+", "", key)
    key = key.strip()
    if re.search(r"\s", key):
        return None
    parts = [p for p in key.split("::") if p]
    if not parts or not IDENTIFIER_RE.match(parts[-1]):
        return None
    return Symbol(key=parts[-1], qualifier=parts[-2] if len(parts) > 1 else None)


def definition_score(line: str, symbol: Symbol) -> int:
    """How strongly does this line look like the *definition* of `symbol`?

    A score <= 0 means "not a definition".
    """
    word = r"\b" + re.escape(symbol.key) + r"\b"
    # A mention inside a comment, an include, or a statement is a *use*. No amount of looking
    # like a signature redeems `return importSceneDocument(doc);`, so stop here rather than
    # letting a positive rule outvote a penalty.
    if re.match(r"^\s*(//|\*|/\*)", line):
        return -1
    if re.match(r"^\s*#\s*(include|if|endif|else|elif)\b", line):
        return -1
    if re.match(r"^\s*(return|if|for|while|else|switch|case|do|throw|co_return|co_await)\b", line):
        return -1

    score = 0
    # class/struct/enum, tolerating an export macro (class PELICAN_API Foo)
    if re.match(r"^\s*(class|struct|enum(\s+class)?|union)\s+([A-Z_][A-Z0-9_]{2,}\s+)?" + word, line):
        score += 100
    if re.match(r"^\s*#\s*define\s+" + word, line):
        score += 100
    if re.match(r"^\s*(" + "|".join(DECL_MACROS) + r")\s*\(\s*" + word + r"\s*\)", line):
        score += 100
    if re.match(r"^\s*(concept|using|typedef)\s+" + word + r"\b", line):
        score += 90
    if re.match(r"^\s*namespace\s+" + word, line):
        score += 80
    if re.match(r"^\s*(function|macro)\s*\(\s*" + word + r"\b", line):  # CMake
        score += 90
    if symbol.qualifier and re.search(r"\b" + re.escape(symbol.qualifier) + "::" + re.escape(symbol.key) + r"\s*\(", line):
        score += 95
    # `Ret name(` and `Ret Class::name(` -- the separator before the name may be whitespace,
    # a pointer/reference sigil, or the `:` closing a `::` qualification.
    if re.match(r"^\s*[A-Za-z_][\w:<>,\s&*]*[\s:*&]" + word + r"\s*\(", line):
        score += 60
    if re.match(r"^\s*" + word + r"\s*\(", line):
        score += 40
    return score


# --------------------------------------------------------------------------- relocation


def relocate_symbol(lines: list[str], symbol: Symbol, recorded_line: int) -> dict[str, Any]:
    word = re.compile(r"\b" + re.escape(symbol.key) + r"\b")
    candidates: list[tuple[int, int]] = []
    for index, line in enumerate(lines):
        if not word.search(line):
            continue
        score = definition_score(line, symbol)
        if score > 0:
            candidates.append((index + 1, score))
    if not candidates:
        return {"status": "unresolved", "reason": "definition not found"}

    best = max(score for _, score in candidates)
    top = [line for line, score in candidates if score == best]
    if len(top) == 1:
        return {"status": "moved", "line": top[0], "confidence": "unique definition"}

    # Several equally definition-like lines (overloads, or a declaration plus its
    # out-of-class definition). Drift is local, so the nearest one to the recorded line is
    # the intended one -- unless two are equidistant, in which case refuse to guess.
    top.sort(key=lambda line: abs(line - recorded_line))
    if abs(top[1] - recorded_line) == abs(top[0] - recorded_line):
        listed = ", ".join(str(line) for line in top[:4])
        return {"status": "unresolved", "reason": f"{len(top)} equally plausible definitions (lines {listed})"}
    return {"status": "moved", "line": top[0], "confidence": f"nearest of {len(top)} definitions"}


def relocate_ledger(lines: list[str], anchor_text: str, recorded_line: int) -> dict[str, Any]:
    want = _normalize(anchor_text)
    if not want:
        return {"status": "unresolved", "reason": "ledger anchor is blank"}
    hits = [index + 1 for index, line in enumerate(lines) if _normalize(line) == want]
    if not hits:
        return {"status": "unresolved", "reason": "anchor text no longer present"}
    if len(hits) == 1:
        return {"status": "moved", "line": hits[0], "confidence": "unique anchor text"}
    # An anchor's job is to identify one line. `endif()` occurs 48 times in the root
    # CMakeLists, and normalising indentation away makes every one of them identical --
    # proximity then picks confidently and wrongly. Past a handful of matches the anchor
    # names nothing, so say so instead of guessing.
    if len(hits) > MAX_ANCHOR_OCCURRENCES:
        return {
            "status": "unresolved",
            "reason": f"anchor text occurs {len(hits)} times and identifies no single line",
        }
    hits.sort(key=lambda line: abs(line - recorded_line))
    if abs(hits[1] - recorded_line) == abs(hits[0] - recorded_line):
        return {"status": "unresolved", "reason": f"anchor text occurs {len(hits)} times, ambiguous"}
    return {"status": "moved", "line": hits[0], "confidence": f"nearest of {len(hits)} occurrences"}


# -------------------------------------------------------------------------- repo access


def _read_lines(path: Path) -> list[str]:
    return path.read_text(encoding="utf-8").splitlines()


def list_docs(repo_root: Path) -> list[str]:
    docs: list[str] = []
    for directory in DOC_DIRS:
        absolute = repo_root / directory
        if not absolute.is_dir():
            continue
        for entry in sorted(absolute.iterdir()):
            if entry.suffix == ".md":
                docs.append((directory / entry.name).as_posix())
    return docs


def staged_docs(repo_root: Path) -> list[str]:
    try:
        completed = subprocess.run(
            ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"],
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise DocLinkError(f"could not read the git index: {error}") from error
    staged = {line.strip() for line in completed.stdout.splitlines() if line.strip()}
    return [doc for doc in list_docs(repo_root) if doc in staged]


def load_ledger(repo_root: Path) -> Ledger:
    path = repo_root / LEDGER_RELATIVE_PATH
    if not path.exists():
        return Ledger()
    try:
        parsed = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise DocLinkError(f"{LEDGER_RELATIVE_PATH.as_posix()} is not valid JSON: {error}") from error
    if parsed.get("schema") != SCHEMA or parsed.get("version") != VERSION:
        raise DocLinkError(
            f"{LEDGER_RELATIVE_PATH.as_posix()} declares "
            f"{parsed.get('schema')}/v{parsed.get('version')}, expected {SCHEMA}/v{VERSION}"
        )
    anchors = parsed.get("anchors")
    if not isinstance(anchors, dict):
        raise DocLinkError(f"{LEDGER_RELATIVE_PATH.as_posix()} has no anchors object")
    return Ledger(anchors=dict(anchors))


def write_ledger(repo_root: Path, ledger: Ledger) -> None:
    body = json.dumps(
        {"schema": SCHEMA, "version": VERSION, "anchors": dict(sorted(ledger.anchors.items()))},
        indent=2,
        ensure_ascii=False,
    )
    (repo_root / LEDGER_RELATIVE_PATH).write_text(body + "\n", encoding="utf-8")


# ----------------------------------------------------------------------- the pass itself


def ledger_key(doc: str, target_rel: str, raw_text: str, ordinal: int) -> str:
    """A stable identity for a link, so the ledger survives edits elsewhere in the document.

    The line number is deliberately absent -- it is the thing we expect to change. `ordinal`
    disambiguates a document that links the same text at the same file more than once; it is
    stable unless those links are reordered.
    """
    base = f"{doc}|{target_rel}|{_normalize(raw_text)}"
    return f"{base}|#{ordinal}" if ordinal > 0 else base


def inspect_document(repo_root: Path, doc: str, ledger: Ledger) -> tuple[str, list[Link]]:
    absolute = repo_root / doc
    text = absolute.read_text(encoding="utf-8")
    results: list[Link] = []
    seen: dict[str, int] = {}

    for match in LINK_RE.finditer(text):
        raw_text, rel, line_text = match.group(1), match.group(2), match.group(3)
        if not rel.startswith(".."):
            continue  # in-document or cross-doc links have no source target
        recorded_line = int(line_text)
        target = (absolute.parent / rel).resolve()
        base_key = f"{rel}|{_normalize(raw_text)}"
        ordinal = seen.get(base_key, 0)
        seen[base_key] = ordinal + 1
        link = Link(
            doc=doc,
            raw_text=raw_text,
            rel=rel,
            recorded_line=recorded_line,
            match_start=match.start(),
            match_text=match.group(0),
            key=ledger_key(doc, rel, raw_text, ordinal),
        )

        if not target.is_file():
            link.status, link.tier = "unresolved", "path"
            link.reason = f"target file does not exist: {rel}"
            results.append(link)
            continue

        lines = _read_lines(target)
        current = lines[recorded_line - 1] if 1 <= recorded_line <= len(lines) else None

        symbol = derive_symbol(raw_text)
        if symbol is not None:
            if current is not None and definition_score(current, symbol) > 0:
                link.status, link.tier, link.anchor_text = "ok", "symbol", current
                results.append(link)
                continue
            moved = relocate_symbol(lines, symbol, recorded_line)
            if moved["status"] == "moved":
                link.status, link.tier = "moved", "symbol"
                link.line = moved["line"]
                link.confidence = moved["confidence"]
                link.anchor_text = lines[moved["line"] - 1]
                results.append(link)
                continue
            # The text names something we cannot locate as a definition *in this file*: a
            # member variable, a use site, or a class declared in the matching header. That
            # is not a broken link, only one the symbol tier cannot carry. Fall through.

        # Tier B: track the target line's text verbatim.
        # `in`, not truthiness: a recorded anchor of "" is a real recorded value (the link
        # points at a blank line), and treating it as absent re-adopts it forever.
        link.tier = "ledger"
        if link.key not in ledger.anchors:
            if current is None:
                link.status = "unresolved"
                link.reason = (
                    f"line {recorded_line} is past end of file ({len(lines)} lines) "
                    "and no anchor is recorded"
                )
            else:
                # First sighting: adopt whatever it points at now. Adoption is only honest
                # when the line exists; it cannot verify the author aimed correctly.
                link.status, link.anchor_text = "adopt", current
                link.suspect = _is_meaningless(current)
            results.append(link)
            continue

        remembered = ledger.anchors[link.key]
        if remembered == "":
            # A blank line cannot be searched for. Nothing to follow, nothing to fix; say so
            # once and move on rather than failing a commit over a link that was already lost.
            link.status, link.anchor_text, link.suspect = "unanchorable", "", True
            results.append(link)
            continue
        if current is not None and _normalize(current) == _normalize(remembered):
            link.status, link.anchor_text = "ok", current
            results.append(link)
            continue

        moved = relocate_ledger(lines, remembered, recorded_line)
        link.status = moved["status"]
        link.line = moved.get("line")
        link.confidence = moved.get("confidence", "")
        link.reason = moved.get("reason", "")
        link.anchor_text = lines[link.line - 1] if link.status == "moved" else remembered
        results.append(link)

    return text, results


def rewrite(text: str, results: list[Link]) -> str:
    """Apply relocations right to left, so earlier offsets stay valid."""
    moves = sorted((r for r in results if r.status == "moved"), key=lambda r: r.match_start, reverse=True)
    out = text
    for result in moves:
        replacement = re.sub(r"#L\d+\)$", f"#L{result.line})", result.match_text)
        out = out[: result.match_start] + replacement + out[result.match_start + len(result.match_text) :]
    return out


def _short(link: Link) -> dict[str, Any]:
    return {
        "doc": link.doc,
        "text": _normalize(link.raw_text),
        "target": link.rel,
        "from": link.recorded_line,
        "to": link.line,
        "tier": link.tier,
        "reason": link.reason,
        "confidence": link.confidence,
    }


def _report(
    mode: str,
    summary: dict[str, int],
    touched: list[str],
    moved: list[Link],
    unresolved: list[Link],
    suspect: list[Link],
) -> None:
    verb = "updated" if mode == "update" else "stale"
    print(
        f"doclink: {summary['ok']} ok, {len(moved)} {verb}, {summary['adopt']} newly anchored, "
        f"{summary['unanchorable']} unanchorable, {len(unresolved)} unresolved"
    )
    if suspect:
        print(f"\n-- anchored to a meaningless line ({len(suspect)}): the link was probably already stale --")
        for link in suspect[:25]:
            print(f"  {link.doc}: [{_normalize(link.raw_text)}] {link.rel} L{link.recorded_line}")
        if len(suspect) > 25:
            print(f"  ... and {len(suspect) - 25} more")
    if moved:
        head = "relocated" if mode == "update" else "stale (run: uv run tools/doclink.py update)"
        print(f"\n-- {head} --")
        for link in moved[:40]:
            print(
                f"  {link.doc}: [{_normalize(link.raw_text)}] {link.rel} "
                f"L{link.recorded_line} -> L{link.line}  ({link.tier}, {link.confidence})"
            )
        if len(moved) > 40:
            print(f"  ... and {len(moved) - 40} more")
    if unresolved:
        print("\n-- unresolved: needs a human --")
        for link in unresolved:
            print(f"  {link.doc}: [{_normalize(link.raw_text)}] {link.rel} L{link.recorded_line}  ({link.tier}: {link.reason})")
    if touched:
        joined = "\n  ".join(touched)
        print(f"\nrewrote {len(touched)} document(s):\n  {joined}")


def run(
    mode: str,
    repo_root: Path,
    *,
    only_staged: bool = False,
    only: set[str] | None = None,
    as_json: bool = False,
) -> int:
    repo_root = repo_root.resolve()
    if not (repo_root / "docs").is_dir():
        raise DocLinkError(f"no docs/ under {repo_root} -- pass --repo-root")

    ledger = load_ledger(repo_root)
    docs = staged_docs(repo_root) if only_staged else list_docs(repo_root)
    if only is not None:
        docs = [doc for doc in docs if doc in only]

    summary = {"ok": 0, "moved": 0, "adopt": 0, "unresolved": 0, "unanchorable": 0}
    moved: list[Link] = []
    unresolved: list[Link] = []
    suspect: list[Link] = []
    touched: list[str] = []

    for doc in docs:
        text, results = inspect_document(repo_root, doc, ledger)
        for link in results:
            summary[link.status] += 1
            if link.status == "moved":
                moved.append(link)
            if link.status == "unresolved":
                unresolved.append(link)
            if link.suspect:
                suspect.append(link)
        if mode == "update":
            updated = rewrite(text, results)
            if updated != text:
                (repo_root / doc).write_text(updated, encoding="utf-8")
                touched.append(doc)
            for link in results:
                if link.status in ("ok", "adopt", "moved", "unanchorable") and link.anchor_text is not None:
                    ledger.anchors[link.key] = _normalize(link.anchor_text)

    if mode == "update":
        write_ledger(repo_root, ledger)

    if as_json:
        print(
            json.dumps(
                {
                    "mode": mode,
                    "summary": summary,
                    "touched": touched,
                    "moved": [_short(link) for link in moved],
                    "unresolved": [_short(link) for link in unresolved],
                    "suspect": [_short(link) for link in suspect],
                },
                indent=2,
                ensure_ascii=False,
            )
        )
    else:
        _report(mode, summary, touched, moved, unresolved, suspect)

    # `check` fails on anything stale; `update` fixes what it can and fails only on the rest.
    if unresolved:
        return 1
    if mode == "check" and moved:
        return 1
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("mode", nargs="?", choices=("check", "update"), default="check")
    parser.add_argument("--repo-root", type=Path, default=Path.cwd(), help="repository root (default: cwd)")
    parser.add_argument("--staged", action="store_true", help="only documents present in the git index")
    parser.add_argument(
        "--only",
        default=None,
        help="comma-separated repo-relative documents to restrict the pass to",
    )
    parser.add_argument("--json", action="store_true", help="machine-readable report")
    return parser.parse_args()


def main() -> int:
    # Link text is largely Japanese and a Windows console defaults to cp932, where printing
    # it raises UnicodeEncodeError. Report legibly where we can, mangled where we cannot,
    # but never fail the run over an encoding.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    args = parse_args()
    only = None
    if args.only:
        only = {part.strip() for part in args.only.split(",") if part.strip()}
    try:
        return run(
            args.mode,
            args.repo_root,
            only_staged=args.staged,
            only=only,
            as_json=args.json,
        )
    except DocLinkError as error:
        print(f"doclink: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
