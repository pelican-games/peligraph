"""QD-1: what every scenario is held to after its last step, without asking.

A scenario says what it does and what it expects of each step.  What it does
not have to say -- because it is true of every run -- is checked here, by the
driver, after the last step passed:

1. ``undo``: every Undo Studio has, pressed until it has nothing left, brings
   the project's documents back to what they were when the engine first
   answered; replaying this check's Undo sequence with Redo brings them to what
   they were after the last step, preserving any older Redo at that endpoint.
2. ``reopen``: the project, saved, closed and opened again (the engine started
   again from the files), holds the same documents and draws the same picture.
3. ``health``: the engine still answers, Studio's and the engine's logs hold no
   error, assert or fatal line that is not allowed by name, and the Problems
   count is what the scenario declares (0 unless it says).

A scenario that one of these does not fit says so, with a reason:

    "invariants": {"undo": false, "undo_reason": "..."}

and one that is red for a known bug says which entry of the list it is, so the
run stays green while the bug is open and the report says KNOWN RED:

    "invariants": {"undo": "known_red", "undo_ticket": "QD1-3"}

(``reason`` / ``ticket`` stand for every invariant the scenario turns off or
marks.)  ``undo_exempt`` names documents an operation writes outside every
Undo by design, each with its reason:

    "invariants": {"undo_exempt": [{"path": "file:shaders/*.frag",
                                     "reason": "New... writes the shader outside Undo"}]}
"""

from __future__ import annotations

import difflib
import fnmatch
import hashlib
import json
import os
import re
import time
from pathlib import Path
from typing import Any, Callable

INVARIANTS = ("undo", "reopen", "health")

# Studio's Undo, one per kind of document (automation `history`). The order is
# the one they are undone in when nothing says which came last; the scene's is
# last because every other kind can refer to an object, and an object comes
# back with the scene.
KINDS = ("graph", "render", "materials", "material_row", "assets", "scene")

# Histories with no Redo: what their Undo took back cannot be put again.
NO_REDO = {
    "materials": "the Materials dock's Undo has no Redo",
    "material_row": "a Material row's Undo (Every <model>) has no Redo",
    "assets": "the Assets dock's Undo has no Redo",
}

# The scene's journal is the engine's: one stack per editor session, and
# Studio opens one, the Inspector's.
INSPECTOR_ACTOR = "Pelican Studio Inspector"

# Keys of an engine answer that count the engine's own clock rather than say
# what the document holds: a revision goes up on an Undo too.
VOLATILE_KEYS = {
    "scene_revision",
    "revision",
    "preview_epoch",
    "pending_ticket_ids",
    "published_generation",
}

# What no Undo takes back, by design, in every scenario: each is said in the
# report when it is what made two documents differ.
DEFAULT_UNDO_EXEMPT = (
    {
        "path": "*",
        "key": "layout.frame_plan",
        "reason": "the Frame Plan's placement (set_layout) is not a render edit; "
        "RU-1 keeps it through Undo",
    },
)

# Files Studio writes outside every Undo, by design, when it makes them: what
# an Undo leaves behind of these is said in the report and not counted. Only a
# file that was not there at the start: a change to one that was is compared.
DEFAULT_UNDO_EXEMPT_ADDED = (
    ("file:shaders/*", "a shader file New... writes is not a render edit (GL-5f); Undo leaves it"),
    ("file:assets/audio/*", "Import sound copies the file into the project; Undo does not delete it"),
    ("file:assets/analysis/*", "an audio analysis is baked into a file on import; Undo does not delete it"),
    ("file:assets/images/*", "an image imported into the project is copied there; Undo does not delete it"),
    ("file:*.glb", "Model from file copies the model; Undo takes back the object, not the copy (manual 13, decision 3)"),
    ("file:*.gltf", "Model from file copies the model; Undo takes back the object, not the copy (manual 13, decision 3)"),
)

MAX_PRESSES_PER_KIND = 400
UNDO_BUDGET_S = 240.0
DIFF_LINES = 12
TEXT_LIMIT = 512 * 1024


class InvariantConfigError(ValueError):
    pass


def invariant_mode(scenario: dict[str, Any], name: str) -> tuple[str, str]:
    """("on" | "off" | "known_red", the reason or the ticket)."""

    config = scenario.get("invariants") or {}
    if not isinstance(config, dict):
        raise InvariantConfigError('"invariants" is an object')
    value = config.get(name, True)
    if value is True:
        return "on", ""
    if value is False:
        reason = config.get(f"{name}_reason") or config.get("reason")
        if not reason:
            raise InvariantConfigError(
                f'"invariants.{name}" is false without a reason ("{name}_reason")'
            )
        return "off", str(reason)
    if value == "known_red":
        ticket = config.get(f"{name}_ticket") or config.get("ticket")
        if not ticket:
            raise InvariantConfigError(
                f'"invariants.{name}" is known_red without a ticket ("{name}_ticket")'
            )
        return "known_red", str(ticket)
    raise InvariantConfigError(
        f'"invariants.{name}" is true, false or "known_red", not {json.dumps(value)}'
    )


# ---------------------------------------------------------------------------
# The documents, as one flat map of key -> canonical text


def strip_volatile(value: Any) -> Any:
    if isinstance(value, dict):
        return {
            key: strip_volatile(item)
            for key, item in value.items()
            if key not in VOLATILE_KEYS
        }
    if isinstance(value, list):
        return [strip_volatile(item) for item in value]
    return value


def canonical(value: Any) -> str:
    return json.dumps(value, indent=1, sort_keys=True, ensure_ascii=False)


def content_text(content: Any) -> str:
    """A document's content, as lines a diff can show: JSON pretty, text as is."""

    if isinstance(content, str):
        try:
            return canonical(json.loads(content))
        except ValueError:
            return content
    return canonical(content)


def journal_depths(journal: Any) -> dict[str, dict[str, int]]:
    """Per editor session (by name): what its Undo and its Redo can still reach."""

    depths: dict[str, dict[str, int]] = {}
    if not isinstance(journal, dict):
        return depths
    undo: dict[Any, int] = {}
    redo: dict[Any, int] = {}
    names: dict[Any, str] = {}
    for record in journal.get("records") or []:
        if record.get("status") not in (None, "committed"):
            continue
        actor = record.get("actor_id")
        names[actor] = str(record.get("actor_display_name") or actor)
        kind = record.get("operation_kind")
        if kind == "undo":
            undo[actor] = max(0, undo.get(actor, 0) - 1)
            redo[actor] = redo.get(actor, 0) + 1
        elif kind == "redo":
            undo[actor] = undo.get(actor, 0) + 1
            redo[actor] = max(0, redo.get(actor, 0) - 1)
        else:
            undo[actor] = undo.get(actor, 0) + 1
            redo[actor] = 0
    for actor, name in names.items():
        depths[f"{name} (actor {actor})"] = {
            "undo": undo.get(actor, 0),
            "redo": redo.get(actor, 0),
        }
    return depths


def available_redo(kinds: dict[str, Any], journal: Any) -> list[str]:
    """Redo already present at the endpoint, excluding an empty scene journal."""

    scene_known = isinstance(journal, dict) and isinstance(journal.get("records"), list)
    scene_redo = sum(depth["redo"] for actor, depth in journal_depths(journal).items()
                     if actor.startswith(INSPECTOR_ACTOR))
    return sorted(
        kind for kind, entry in kinds.items()
        if (entry.get("redo") or {}).get("found")
        and (entry.get("redo") or {}).get("enabled")
        and (kind != "scene" or not scene_known or scene_redo > 0)
    )


class ProjectState:
    """One reading of `project_state`: the documents, flattened."""

    def __init__(self, reply: dict[str, Any]) -> None:
        self.raw = reply
        self.dir = str(reply.get("dir") or "")
        self.pid = reply.get("engine_process_id")
        self.engine_ready = bool(reply.get("engine_ready"))
        self.entries: dict[str, str] = {}
        self.texts: dict[str, str] = {}
        self.scene_files: set[str] = set()
        self.errors: list[str] = []
        root = Path(self.dir) if self.dir else None
        for entry in reply.get("files") or []:
            key = "file:" + str(entry.get("path"))
            self.entries[key] = str(entry.get("sha256") or f"unreadable: {entry.get('error')}")
            if root is None or int(entry.get("size") or 0) > TEXT_LIMIT:
                continue
            try:
                text = (root / str(entry["path"])).read_bytes().decode("utf-8")
            except (OSError, UnicodeDecodeError):
                continue
            if chr(0) in text:
                continue  # a binary file that happens to decode: its sha is the comparison
            self.texts[key] = text
            if '"pelican.scene"' in text:
                try:
                    if json.loads(text).get("schema") == "pelican.scene":
                        self.scene_files.add(key)
                except (ValueError, AttributeError):
                    pass
        rpc = reply.get("rpc") or {}
        self.journal = rpc.get("query_journal")
        for method, value in rpc.items():
            if method == "query_journal":
                continue
            if isinstance(value, dict) and set(value) == {"error"}:
                self.errors.append(f"{method}: {value['error']}")
                continue
            if method == "get_render_documents" and isinstance(value, dict):
                for document in value.get("documents") or []:
                    key = f"render:{document.get('reference')}"
                    self.entries[key] = content_text(document.get("content"))
                rest = {k: v for k, v in value.items() if k != "documents"}
                self.entries["render:(answer)"] = canonical(strip_volatile(rest))
                continue
            if method == "export_scene_snapshot" and isinstance(value, dict):
                semantic = value.get("semantic_scene_bytes")
                if isinstance(semantic, str):
                    self.entries["scene:(semantic)"] = content_text(semantic)
                rest = {
                    k: v for k, v in value.items()
                    if k not in ("semantic_scene_bytes", "digest")
                }
                self.entries["scene:(answer)"] = canonical(strip_volatile(rest))
                continue
            self.entries[f"{method}:"] = canonical(strip_volatile(value))

    def text_of(self, key: str) -> str | None:
        if key.startswith("file:"):
            return self.texts.get(key)
        return self.entries.get(key)

    def fingerprint(self) -> str:
        return hashlib.sha256(canonical(self.entries).encode("utf-8")).hexdigest()


def without_subtree(text: str | None, dotted: str) -> str | None:
    """`text` as canonical JSON with the value at `dotted` taken out."""

    if text is None:
        return None
    try:
        value = json.loads(text)
    except ValueError:
        return None
    parts = dotted.split(".")
    def remove(parent: Any, path: list[str]) -> None:
        if not isinstance(parent, dict) or not path:
            return
        key = path[0]
        if len(path) == 1:
            parent.pop(key, None)
        elif key in parent:
            remove(parent[key], path[1:])
            # The explicitly exempt subtree may be the only reason its
            # container exists. Keep every sibling and unrelated empty map.
            if parent[key] == {}:
                del parent[key]
    remove(value, parts)
    return canonical(value)


def diff_states(
    before: ProjectState,
    after: ProjectState,
    *,
    skip: Callable[[str, str], str | None] = lambda key, change: None,
    subtrees: list[dict[str, str]] = (),
) -> tuple[list[dict[str, Any]], list[dict[str, str]]]:
    """(the documents that differ, with the head of each diff; those skipped).

    `subtrees` are {path, key, reason}: in a JSON document whose key matches
    `path`, the value at `key` is left out of the comparison.
    """

    differing: list[dict[str, Any]] = []
    skipped: list[dict[str, str]] = []
    for key in sorted(set(before.entries) | set(after.entries)):
        was = before.entries.get(key)
        now = after.entries.get(key)
        if was == now:
            continue
        reason = skip(key, "added" if was is None else "removed" if now is None else "changed")
        if reason:
            skipped.append({"key": key, "reason": reason})
            continue
        old_text = before.text_of(key) if was is not None else ""
        new_text = after.text_of(key) if now is not None else ""
        left_out = []
        for exemption in subtrees:
            if was is None or now is None or not fnmatch.fnmatchcase(key, exemption["path"]):
                continue
            old_stripped = without_subtree(old_text, exemption["key"])
            new_stripped = without_subtree(new_text, exemption["key"])
            if old_stripped is None or new_stripped is None:
                continue
            if old_stripped != content_text(old_text) or new_stripped != content_text(new_text):
                left_out.append(f"{exemption['key']}: {exemption['reason']}")
            old_text, new_text = old_stripped, new_stripped
        if left_out and old_text == new_text:
            skipped.append({"key": key, "reason": "exempt " + "; ".join(left_out)})
            continue
        entry: dict[str, Any] = {
            "key": key,
            "change": "added" if was is None else "removed" if now is None else "changed",
        }
        if left_out:
            entry["left_out"] = left_out
        if old_text is not None and new_text is not None:
            lines = list(
                difflib.unified_diff(
                    old_text.splitlines(),
                    new_text.splitlines(),
                    "expected",
                    "found",
                    n=1,
                    lineterm="",
                )
            )
            entry["diff"] = lines[2 : 2 + DIFF_LINES]
            if len(lines) > 2 + DIFF_LINES:
                entry["diff"].append(f"... {len(lines) - 2 - DIFF_LINES} more lines")
        else:
            entry["diff"] = [f"expected sha {was}", f"found sha {now}"]
        differing.append(entry)
    return differing, skipped


def describe(differing: list[dict[str, Any]]) -> str:
    return "; ".join(f"{entry['key']} {entry['change']}" for entry in differing)


# ---------------------------------------------------------------------------
# Talking to Studio


class Epilogue:
    """Everything the invariants need from the run, gathered while it ran."""

    def __init__(
        self,
        run: Any,
        scenario: dict[str, Any],
        variables: dict[str, str] | None = None,
        repository: Path | None = None,
    ) -> None:
        self.run = run
        self.scenario = scenario
        self.variables = dict(variables or {})
        self.repository = repository
        self.log_lines: dict[str, set[str]] = {}
        self.restarts: list[dict[str, Any]] = []
        self.stopped_play = ""
        self.written_by_scenario: dict[str, int] = {}
        self.settled: tuple[str, str, Any] | None = None
        # The last time the engine was started again on the same project
        # during the run (a restart_required edit). The scene journal starts
        # over there; Graph/Render histories can survive the engine restart.
        self.cut: dict[str, Any] | None = None
        self.last_pid: Any = None
        self.problems_at_end: dict[str, Any] | None = None
        self.start: ProjectState | None = None
        self.start_step = 0
        self.history_seen: dict[str, str] = {}
        self.last_change: dict[str, int] = {}
        self.opened_projects: list[int] = []
        self.timings: dict[str, float] = {}
        self.results: dict[str, Any] = {}

    # -- while the scenario runs --------------------------------------------

    def call(self, request: dict[str, Any], timeout: float = 120.0) -> dict[str, Any]:
        return self.run.call(request, timeout=timeout)

    def history(self) -> dict[str, Any] | None:
        reply = self.call({"cmd": "history"}, timeout=30.0)
        if not reply.get("ok") or not isinstance(reply.get("result"), dict):
            return None
        return reply["result"]["kinds"]

    def state(self, journal: bool = False, quick: bool = False) -> ProjectState | None:
        """project_state; the scene journal only when asked (it is the slow part).

        `quick` reads the files and the scene only: what a poll needs to see
        that something moved (the render documents, the materials and
        project.json are files the engine writes as it commits).
        """

        request: dict[str, Any] = {"cmd": "project_state", "timeout_ms": 60000, "journal": journal}
        if quick:
            request["methods"] = ["export_scene_snapshot"]
        reply = self.call(request, timeout=400.0)
        if not reply.get("ok") or not isinstance(reply.get("result"), dict):
            return None
        return ProjectState(reply["result"])

    def capture_start(self, step_index: int, need_engine: bool = True) -> None:
        started = time.perf_counter()
        state = self.state(journal=True)
        if state is not None and (state.engine_ready or not need_engine):
            self.start = state
            self.start_step = step_index
            self.last_pid = state.pid
            self.cut = None
        self.timings["start_capture_s"] = time.perf_counter() - started

    def after_step(self, step: dict[str, Any], index: int, ok: bool) -> None:
        if not ok:
            return
        if step.get("cmd") == "open_project":
            self.opened_projects.append(index)
        if step.get("file") in ("write", "replace", "copy") and step.get("path"):
            # What a person does in an editor of their own: no Undo of
            # Studio's is meant to take it back.
            self.written_by_scenario[str(Path(str(step["path"])).resolve())] = index
        if step.get("cmd") != "wait_engine":
            return
        if self.start is None:
            self.capture_start(index)
            return
        # A scenario that makes a project (File > New Project) or opens one
        # again establishes a new baseline when that project's engine answers.
        # A stale history surviving the switch can then be exposed by Undo.
        reply = self.call({"cmd": "project_state", "brief": True}, timeout=30.0)
        identity = reply.get("result") or {}
        reopened = any(opened > self.start_step for opened in self.opened_projects)
        if reply.get("ok") and (identity.get("dir") != self.start.dir or reopened):
            self.restarts.append(
                {"step": index, "dir": identity.get("dir"),
                 "why": "another project" if identity.get("dir") != self.start.dir
                 else "the project opened again"}
            )
            self.capture_start(index)
        elif reply.get("ok") and identity.get("engine_process_id") != self.last_pid:
            state = self.state(journal=True)
            if state is not None and state.engine_ready:
                self.cut = {"step": index, "pid": state.pid, "state": state}
            self.last_pid = identity.get("engine_process_id")

    # -- after the last step ------------------------------------------------

    def close_graph_drafts(self) -> list[str]:
        """Leave text-edit mode before walking committed document history.

        A node-label click can leave an uncommitted editor open throughout
        sidebar edits. Graph Undo correctly stays disabled while it is open.
        Escape cancels that draft; it does not submit an edit or omit a file.
        """
        reply = self.call({"cmd": "list", "pattern": "::graph_inline_editor#"}, timeout=30.0)
        widgets = (reply.get("result") or {}).get("widgets")
        if not reply.get("ok") or not isinstance(widgets, list):
            raise ValueError("could not inspect Graph inline editors before Undo")
        closed = []
        for widget in widgets:
            name = widget.get("name", "")
            if not name.startswith("pelican.graphCanvasView::graph_inline_editor#"):
                continue
            answer = self.call({"cmd": "key", "name": name, "sequence": "Escape"}, timeout=30.0)
            if not answer.get("ok"):
                raise ValueError(f"could not close Graph inline editor {name}")
            gone = self.call({"cmd": "wait", "name": name, "missing": True,
                              "timeout_ms": 10000}, timeout=30.0)
            if not gone.get("ok"):
                raise ValueError(f"Graph inline editor did not close: {name}")
            closed.append(name)
        return closed

    @staticmethod
    def order(step: str) -> list[str]:
        """The kinds in the order they are pressed: KINDS for Undo, reversed for Redo.

        The Graph tab's Undo comes before Edit > Undo Render Edit because it
        takes back the render edits the tab sent in the order it sent them.
        """

        return list(KINDS) if step == "undo" else list(reversed(KINDS))

    def signature(self) -> tuple[str, str, Any] | None:
        kinds = self.history()
        state = self.state(quick=True)
        if kinds is None or state is None or not state.engine_ready:
            return None
        return canonical(kinds), state.fingerprint(), state.pid

    def settle(self, before: tuple[str, str] | None) -> tuple[str, str] | None:
        """The reading once Studio and the engine have stopped moving."""

        pressed = time.monotonic()
        time.sleep(0.25)
        last: tuple[str, str] | None = None
        deadline = pressed + 30.0
        while time.monotonic() < deadline:
            current = self.signature()
            if current is None:
                # An engine restarting (an Undo of a restart_required edit).
                self.call({"cmd": "wait_engine", "timeout_ms": 60000}, timeout=90.0)
                last = None
                continue
            if current == last and (current != before or time.monotonic() - pressed > 1.5):
                return current
            last = current
            time.sleep(0.15)
        return last

    def select_for_scene(self) -> str:
        """The Inspector's Undo needs an object selected; select the first one."""

        for index in range(0, 8):
            reply = self.call(
                {"cmd": "select", "name": "pelican.outliner", "index": index, "timeout_ms": 500},
                timeout=30.0,
            )
            if not reply.get("ok"):
                return f"no Outliner row {index} could be selected"
            deadline = time.monotonic() + 3.0
            while time.monotonic() < deadline:
                kinds = self.history() or {}
                if ((kinds.get("scene") or {}).get("undo") or {}).get("enabled"):
                    return f"selected Outliner row {index} ({reply['result'].get('text')})"
                time.sleep(0.1)
        return "no Outliner row made the Inspector's Undo pressable"

    def scene_pressable(self, step: str, control: dict[str, Any], notes: list[str]) -> dict[str, Any]:
        """The Inspector's control, made pressable when the journal has something for it."""

        if not control.get("found") or control.get("enabled"):
            return control
        state = self.state(journal=True)
        depth = journal_depths(state.journal if state else None)
        reachable = sum(
            value[step] for name, value in depth.items() if name.startswith(INSPECTOR_ACTOR)
        )
        if reachable <= 0:
            return control
        self.settled = None
        notes.append(f"scene {step}: {self.select_for_scene()}")
        kinds = self.history() or {}
        return (kinds.get("scene") or {}).get(step) or {}

    def press_once(self, kind: str, step: str, log: list[dict[str, Any]],
                   notes: list[str]) -> str:
        """Press one control: "moved", "still" (nothing changed), "off" or "refused"."""

        kinds = self.history() or {}
        control = (kinds.get(kind) or {}).get(step) or {}
        if kind == "scene":
            control = self.scene_pressable(step, control, notes)
        if not control.get("found") or not control.get("enabled"):
            return "off"
        # A parent panel may be hidden while its Undo remains available.
        # The material widget disables its button when its history is empty;
        # visibility does not tell us whether there is an edit to take back.
        # The reading the last press settled on, when nothing came between.
        before = self.settled if self.settled is not None else self.signature()
        self.settled = None
        reply = self.call({"cmd": "history", "kind": kind, "step": step}, timeout=60.0)
        entry: dict[str, Any] = {
            "kind": kind,
            "step": step,
            "label": control.get("tooltip") or control.get("text"),
        }
        if not reply.get("ok"):
            entry["refused"] = (reply.get("error") or {}).get("message", "refused")
            log.append(entry)
            return "refused"
        after = self.settle(before)
        self.settled = after
        entry["moved"] = not (after is not None and after == before)
        if before is not None and after is not None:
            entry["documents_moved"] = before[1] != after[1]
            if before[2] != after[2]:
                entry["engine_started_again"] = f"pid {before[2]} -> {after[2]}"
        notice_of = {"scene": "pelican.inspectorNotice", "graph": "pelican.graphCanvasNotice",
                     "materials": "pelican.materials.notice", "render": "pelican.framePlanStatus"}
        if kind in notice_of:
            # What the panel said of it: a refusal, if any.
            notice = self.call({"cmd": "text", "name": notice_of[kind]}, timeout=30.0)
            if notice.get("ok") and isinstance(notice.get("result"), dict):
                entry["notice"] = str(notice["result"].get("text", ""))[:300]
        log.append(entry)
        return "moved" if entry["moved"] else "still"

    def press_all(self, step: str, deadline: float) -> dict[str, Any]:
        """Press `step` of every history until none has anything left."""

        log: list[dict[str, Any]] = []
        sequence: list[str] = []
        notes: list[str] = []
        self.settled = None
        progress = True
        exhausted: set[str] = set()
        retried_scene = False
        while progress and time.monotonic() < deadline:
            progress = False
            for kind in self.order(step):
                if (step == "redo" and kind in NO_REDO) or kind in exhausted:
                    continue
                count = 0
                while count < MAX_PRESSES_PER_KIND and time.monotonic() < deadline:
                    outcome = self.press_once(kind, step, log, notes)
                    # "still": pressed, and neither a control nor a document
                    # moved -- the history has nothing left (the scene's buttons
                    # stay on with an empty journal) or it said no.
                    if outcome == "still" and kind == "scene" and not retried_scene:
                        # The Inspector may have refused while it read the
                        # object again ("Value changed elsewhere; refreshing
                        # ..."); one more press once it has, unless the engine
                        # said the journal is empty.
                        notice = str(log[-1].get("notice", "")) if log else ""
                        if "no committed transaction" not in notice and "no successful revert" not in notice:
                            retried_scene = True
                            time.sleep(1.0)
                            continue
                    if outcome != "moved":
                        if outcome == "still":
                            exhausted.add(kind)
                        break
                    count += 1
                    if log and log[-1].get("documents_moved"):
                        # A previously refused Undo can become valid after
                        # another history removes the document it referenced.
                        exhausted.clear()
                    sequence.append(kind)
                    progress = True
        if time.monotonic() >= deadline:
            notes.append(f"{step}: stopped at the time budget ({UNDO_BUDGET_S:.0f} s)")
        return self.pressed_summary(log, sequence, notes)

    def redo_in_reverse(self, undone: dict[str, Any], deadline: float,
                        *, drain_remaining: bool = True) -> dict[str, Any]:
        """Redo what Undo took back, one press per Undo press, last first.

        Two of the histories share one stack (the Graph tab's Undo reaches the
        render edits it sent), so Redo goes back the way Undo came rather than
        one kind at a time. Remaining Redo entries are drained only when
        the scenario did not already have a Redo stack at its endpoint;
        otherwise those entries would take us past the captured endpoint.
        """

        log: list[dict[str, Any]] = []
        sequence: list[str] = []
        notes: list[str] = []
        self.settled = None
        for kind in reversed(undone["sequence"]):
            if kind in NO_REDO:
                continue
            if time.monotonic() >= deadline:
                notes.append(f"redo: stopped at the time budget ({UNDO_BUDGET_S:.0f} s)")
                break
            outcome = self.press_once(kind, "redo", log, notes)
            if outcome == "moved":
                sequence.append(kind)
            else:
                notes.append(f"the {kind} Redo was {outcome} where its Undo had moved")
        if drain_remaining:
            rest = self.press_all("redo", deadline)
            log.extend(rest["log"])
            sequence.extend(rest["sequence"])
            notes.extend(rest["notes"])
        else:
            notes.append("the scenario already had Redo entries at its endpoint; "
                         "only this check's Undo sequence was replayed")
        return self.pressed_summary(log, sequence, notes)

    @staticmethod
    def pressed_summary(log: list[dict[str, Any]], sequence: list[str],
                        notes: list[str]) -> dict[str, Any]:
        presses: dict[str, int] = {}
        for kind in sequence:
            presses[kind] = presses.get(kind, 0) + 1
        return {"presses": presses, "sequence": sequence, "notes": notes, "log": log}


# ---------------------------------------------------------------------------
# Invariant 1: Undo to the start, Redo to the end


def undo_exemptions(scenario: dict[str, Any]) -> tuple[list[tuple[str, str]], list[dict[str, str]]]:
    """(whole documents by glob, subtrees of JSON documents), each with its reason."""

    config = scenario.get("invariants") or {}
    whole: list[tuple[str, str]] = []
    subtrees: list[dict[str, str]] = [dict(entry) for entry in DEFAULT_UNDO_EXEMPT]
    for entry in config.get("undo_exempt") or []:
        if not isinstance(entry, dict) or not entry.get("path") or not entry.get("reason"):
            raise InvariantConfigError(
                '"invariants.undo_exempt" entries are {"path": <glob>, "key"?: <a.b>, "reason": ...}'
            )
        if entry.get("key"):
            subtrees.append({key: str(entry[key]) for key in ("path", "key", "reason")})
        else:
            whole.append((str(entry["path"]), str(entry["reason"])))
    return whole, subtrees


def check_undo(epilogue: Epilogue) -> dict[str, Any]:
    """Run invariant 1 and say what it found; the caller records it."""

    result: dict[str, Any] = {"invariant": "undo"}
    start = epilogue.start
    if start is None:
        result.update(status="not_applicable", reason="the engine never answered project_state "
                      "after the scenario's first wait_engine")
        return result
    end = epilogue.state(journal=True)
    if end is None or not end.engine_ready:
        result.update(status="red", reason="the engine did not answer project_state after the last step")
        return result
    result["start_step"] = epilogue.start_step
    if epilogue.stopped_play:
        result["stopped_play"] = epilogue.stopped_play
    if epilogue.restarts:
        result["started_again"] = epilogue.restarts
    if start.errors:
        result["start_unread"] = start.errors
    if end.dir != start.dir:
        result.update(
            status="not_applicable",
            reason=f"the scenario ends in another project ({Path(end.dir).name}, "
            f"started in {Path(start.dir).name}); every history ends with its project",
        )
        return result
    later_opens = [index for index in epilogue.opened_projects if index > epilogue.start_step]
    if later_opens:
        result.update(
            status="not_applicable",
            reason=f"the scenario opened a project again at step {later_opens[0]}; "
            "every history starts over with the project",
        )
        return result
    restarted = start.pid != end.pid
    exemptions, subtrees = undo_exemptions(epilogue.scenario)

    def skip_for_undo(key: str, change: str) -> str | None:
        if change == "added":
            for pattern, reason in DEFAULT_UNDO_EXEMPT_ADDED:
                if fnmatch.fnmatchcase(key, pattern):
                    return f"exempt: {reason}"
        for pattern, reason in exemptions:
            if fnmatch.fnmatchcase(key, pattern):
                return f"exempt: {reason}"
        if key.startswith("file:") and start.dir:
            written = epilogue.written_by_scenario.get(
                str((Path(start.dir) / key[len("file:"):]).resolve()))
            if written is not None:
                return f"written by the scenario's own file step {written} (an editor outside Studio)"
        if not start.engine_ready and not key.startswith("file:"):
            return "the engine was not up when the start was read (the scenario has no wait_engine)"
        for unread in start.errors:
            method = unread.split(":", 1)[0]
            prefix = {"get_render_documents": "render", "export_scene_snapshot": "scene"}.get(method, method)
            if key.startswith(prefix + ":"):
                return f"the engine did not answer it at the start: {unread}"
        if key in start.scene_files or key in end.scene_files:
            return "a scene file on disk: Save is not undone; the scene in memory is compared"
        if restarted and key.startswith("scene:"):
            return (
                f"the engine was started again during the run (pid {start.pid} -> {end.pid}); "
                "the scene journal lives in the engine and does not survive it"
            )
        return None

    def skip_for_redo(key: str, change: str) -> str | None:
        for pattern, reason in exemptions:
            if fnmatch.fnmatchcase(key, pattern):
                return f"exempt: {reason}"
        return None

    result["end_journal"] = journal_depths(end.journal)
    result["closed_graph_drafts"] = epilogue.close_graph_drafts()
    if result["closed_graph_drafts"]:
        epilogue.settled = epilogue.settle(None)
    kinds = epilogue.history() or {}
    existing_redo = available_redo(kinds, end.journal)
    result["redo_available_at_end"] = existing_redo
    result["history_at_end"] = {
        kind: {
            step: (f"{'on' if control.get('enabled') else 'off'}: "
                   f"{control.get('tooltip') or control.get('text')}") if control else None
            for step, control in entry.items()
        }
        for kind, entry in kinds.items()
        if any(control and control.get("found") for control in entry.values())
    }
    budget_end = time.monotonic() + UNDO_BUDGET_S
    started = time.perf_counter()
    undone = epilogue.press_all("undo", budget_end)
    result["undo"] = undone
    after_undo = epilogue.state(journal=True)
    result["undo_s"] = round(time.perf_counter() - started, 1)
    if after_undo is None:
        result.update(status="red", reason="the engine did not answer after the Undo presses")
        return result
    result["journal_after_undo"] = journal_depths(after_undo.journal)
    undo_diffs, undo_skipped = diff_states(start, after_undo, skip=skip_for_undo, subtrees=subtrees)
    result["undo_differs"] = undo_diffs
    result["undo_skipped"] = undo_skipped
    cut_explains = False
    if undo_diffs and epilogue.cut is not None:
        from_cut, _ = diff_states(epilogue.cut["state"], after_undo, skip=skip_for_undo,
                                  subtrees=subtrees)
        cut_explains = not from_cut
        result["cut"] = {"step": epilogue.cut["step"], "pid": epilogue.cut["pid"],
                         "undo_returns_there": cut_explains,
                         "differs_from_there": [entry["key"] for entry in from_cut]}

    started = time.perf_counter()
    redone = epilogue.redo_in_reverse(
        undone, time.monotonic() + UNDO_BUDGET_S, drain_remaining=not existing_redo)
    result["redo"] = redone
    after_redo = epilogue.state()
    result["redo_s"] = round(time.perf_counter() - started, 1)
    if after_redo is None:
        result.update(status="red", reason="the engine did not answer after the Redo presses")
        return result
    no_redo_used = sorted(kind for kind in undone["presses"] if kind in NO_REDO)
    if no_redo_used:
        result["redo_not_covered"] = [f"{kind}: {NO_REDO[kind]}" for kind in no_redo_used]
        redo_diffs: list[dict[str, Any]] = []
        redo_skipped = [{"key": "*", "reason": "Undo took back an edit of a history with no Redo: "
                         + ", ".join(no_redo_used)}]
    else:
        redo_diffs, redo_skipped = diff_states(end, after_redo, skip=skip_for_redo, subtrees=subtrees)
    result["redo_differs"] = redo_diffs
    result["redo_skipped"] = redo_skipped
    result["after_redo_fingerprint"] = after_redo.fingerprint()
    reasons = []
    if undo_diffs:
        reasons.append("after Undo, not the start: " + describe(undo_diffs))
    if redo_diffs:
        reasons.append("after Redo, not the end: " + describe(redo_diffs))
    result["status"] = "red" if (undo_diffs or redo_diffs) else "green"
    if cut_explains:
        # Still red: the Graph tab's history outlives an engine start (OT-1)
        # and the render edits' does too; only the scene journal does not,
        # and the scene is not compared across one. Said, to point at where.
        reasons.append(f"what is left is exactly what the engine was started again with at "
                       f"step {epilogue.cut['step']}: no Undo reaches back past that start")
    incomplete = [note for walk in (undone, redone) for note in walk.get("notes", [])
                  if "stopped at the time budget" in note]
    if incomplete:
        # An interrupted traversal did not establish the invariant, even if
        # its final snapshots happen to match. A known product finding must
        # not mask a check that never finished.
        result.update(status="red", check_error=True)
        reasons.extend(incomplete)
    result["reason"] = " / ".join(reasons)
    return result


# ---------------------------------------------------------------------------
# Invariant 2: saved, closed and opened again, the same


PIXEL_GRID = [[x, y] for y in (0.2, 0.5, 0.8) for x in (0.2, 0.5, 0.8)]
PIXEL_TOLERANCE = 3


def read_pixels(epilogue: Epilogue, stem: str) -> list[list[int]] | None:
    path = Path(epilogue.run.artifacts) / f"{stem}.png"
    reply = epilogue.call(
        {"cmd": "screenshot", "name": "pelican.viewportHost", "mode": "screen",
         "path": str(path), "pixels": PIXEL_GRID},
        timeout=60.0,
    )
    if not reply.get("ok"):
        return None
    return reply["result"].get("rgb")


def pixels_close(a: list[list[int]] | None, b: list[list[int]] | None) -> bool:
    if not a or not b or len(a) != len(PIXEL_GRID) or len(b) != len(PIXEL_GRID):
        return False
    if any(not isinstance(pixel, list) or len(pixel) != 3
           or any(type(channel) is not int or not 0 <= channel <= 255 for channel in pixel)
           for pixel in a + b):
        return False
    return all(
        abs(int(x) - int(y)) <= PIXEL_TOLERANCE
        for left, right in zip(a, b)
        for x, y in zip(left, right)
    )


def steady_pixels(
    epilogue: Epilogue, stem: str, within_s: float
) -> tuple[list[list[int]] | None, bool]:
    """The picture once two readings half a second apart agree: (rgb, steady)."""

    deadline = time.monotonic() + within_s
    last = read_pixels(epilogue, stem)
    while time.monotonic() < deadline:
        time.sleep(0.5)
        current = read_pixels(epilogue, stem)
        if current is not None and pixels_close(current, last):
            return current, True
        last = current
    return last, False


def settled_state(epilogue: Epilogue, within_s: float = 20.0) -> ProjectState | None:
    """project_state once two readings agree (a save or a load still landing)."""

    deadline = time.monotonic() + within_s
    last: ProjectState | None = None
    while time.monotonic() < deadline:
        current = epilogue.state()
        if current is not None and current.engine_ready and not current.errors:
            if last is not None and current.fingerprint() == last.fingerprint():
                return current
            last = current
        else:
            last = None
        time.sleep(0.3)
    return None


def check_reopen(epilogue: Epilogue) -> dict[str, Any]:
    result: dict[str, Any] = {"invariant": "reopen"}
    config = epilogue.scenario.get("invariants") or {}
    pixels_why = ""
    if config.get("pixels") is False:
        pixels_why = str(config.get("pixels_reason") or "")
        if not pixels_why:
            raise InvariantConfigError('"invariants.pixels" is false without "pixels_reason"')
    before = epilogue.state()
    if before is None or not before.engine_ready:
        result.update(status="red", reason="the engine did not answer project_state before the save")
        return result
    alternate = next(
        (
            epilogue.variables[name]
            for name in ("other_project", "project")
            if epilogue.variables.get(name)
            and Path(epilogue.variables[name]).is_dir()
            and Path(epilogue.variables[name]).resolve() != Path(before.dir).resolve()
        ),
        None,
    )
    if alternate is None:
        result.update(status="not_applicable",
                      reason="no second project to open between the close and the reopen")
        return result
    # Unsaved scene edits are only in the engine; Save puts them in the file
    # the next engine reads. File > Save Scene is pressed whether or not
    # anything is unsaved: saving an unchanged scene writes the same scene.
    save = epilogue.call({"cmd": "click", "name": "pelican.file.saveScene"}, timeout=60.0)
    result["save"] = "pressed" if save.get("ok") else (save.get("error") or {}).get("message")
    if not save.get("ok"):
        result.update(status="red", reason="File > Save Scene failed: " + str(result["save"]))
        return result
    saved = settled_state(epilogue)
    if saved is None:
        result.update(status="red", reason="the engine did not answer after File > Save Scene")
        return result
    before_pixels: list[list[int]] | None = None
    before_steady = False
    if not pixels_why:
        before_pixels, before_steady = steady_pixels(epilogue, "qd1_reopen_before", 5.0)
    pid = saved.pid
    started = time.perf_counter()
    for directory in (alternate, saved.dir):
        reply = epilogue.call({"cmd": "open_project", "dir": directory}, timeout=60.0)
        if not reply.get("ok"):
            result.update(status="red", reason=f"open_project {Path(directory).name}: "
                          + (reply.get("error") or {}).get("message", ""))
            return result
        waited = epilogue.call(
            {"cmd": "wait_engine", "timeout_ms": 180000, "frames": 3}, timeout=240.0
        )
        if not waited.get("ok"):
            result.update(status="red", reason=f"the engine on {Path(directory).name} did not "
                          "come up: " + (waited.get("error") or {}).get("message", ""))
            return result
        opened = epilogue.state()
        if opened is None or Path(opened.dir).resolve() != Path(directory).resolve():
            result.update(status="red", reason=f"open_project did not switch to {Path(directory).name}")
            return result
    result["reopen_s"] = round(time.perf_counter() - started, 1)
    after = settled_state(epilogue)
    if after is None:
        result.update(status="red", reason="the engine did not answer project_state after the reopen")
        return result
    if after.pid == pid:
        result.update(status="red", reason="the engine was not started again: Studio kept "
                      f"pid {pid} through two open_project (an unsaved-changes question?)")
        return result
    differing, skipped = diff_states(saved, after)
    result["differs"] = differing
    result["skipped"] = skipped
    reasons = []
    if differing:
        reasons.append("after the reopen, not what was saved: " + describe(differing))
    if pixels_why:
        result["pixels"] = f"off: {pixels_why}"
    elif not before_steady:
        result["pixels"] = {"grid": PIXEL_GRID, "before": before_pixels}
        reasons.append("could not obtain a stable pixel grid before the close; animated scenarios "
                       "must declare pixels: false with pixels_reason")
    else:
        after_pixels, after_steady = steady_pixels(epilogue, "qd1_reopen_after", 10.0)
        result["pixels"] = {"grid": PIXEL_GRID, "before": before_pixels, "after": after_pixels}
        if not after_steady or not pixels_close(before_pixels, after_pixels):
            reasons.append(
                f"the picture differs after the reopen: {before_pixels} -> {after_pixels}"
            )
    result["status"] = "red" if reasons else "green"
    result["reason"] = " / ".join(reasons)
    return result


# ---------------------------------------------------------------------------
# Invariant 3: nothing broke


LOG_PATTERN = re.compile(r"LOG_ERROR|LOG_FATAL|\bERROR\b|\bassert(ion)?\b|\bfatal\b", re.IGNORECASE)


def log_allowlist(epilogue: Epilogue) -> list[tuple[re.Pattern[str], str]]:
    entries: list[dict[str, Any]] = []
    if epilogue.repository is not None:
        shared = epilogue.repository / "test" / "studio_scenarios" / "log_allowlist.json"
        if shared.is_file():
            entries += json.loads(shared.read_text(encoding="utf-8")).get("allow", [])
    entries += (epilogue.scenario.get("invariants") or {}).get("log_allow", [])
    allowed = []
    for entry in entries:
        if not entry.get("pattern") or not entry.get("reason"):
            raise InvariantConfigError('a log allowance is {"pattern": <regex>, "reason": ...}')
        allowed.append((re.compile(entry["pattern"]), str(entry["reason"])))
    return allowed


def logs_of(epilogue: Epilogue) -> list[Path]:
    paths = [Path(epilogue.run.artifacts) / "studio.log"]
    for name in ("project", "other_project"):
        if epilogue.variables.get(name):
            paths.append(Path(epilogue.variables[name]) / ".pelican" / "logs" / "player.log")
    if epilogue.start is not None and epilogue.start.dir:
        paths.append(Path(epilogue.start.dir) / ".pelican" / "logs" / "player.log")
    unique: list[Path] = []
    for path in paths:
        if all(path.resolve() != seen.resolve() for seen in unique):
            unique.append(path)
    return unique


def scan_logs(epilogue: Epilogue) -> None:
    """Keep every error line of every log as it stands (an engine start may rewrite one)."""

    for path in logs_of(epilogue):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        name = path.name if path.name == "studio.log" else f"{path.parents[2].name}/{path.name}"
        for line in text.splitlines():
            if LOG_PATTERN.search(line):
                epilogue.log_lines.setdefault(name, set()).add(line.strip())


def play_session(epilogue: Epilogue) -> bool | None:
    reply = epilogue.call({"cmd": "rpc", "method": "get_status", "params": {}}, timeout=60.0)
    if not reply.get("ok") or not isinstance(reply.get("result"), dict):
        return None
    play = ((reply["result"].get("result") or {}).get("play")) or {}
    return bool(play.get("session"))


def stop_play(epilogue: Epilogue) -> str:
    if not play_session(epilogue):
        return ""
    pressed = epilogue.call({"cmd": "click", "name": "pelican.play.play"}, timeout=60.0)
    if not pressed.get("ok"):
        return "the run was in Play and Stop could not be pressed: " + (
            pressed.get("error") or {}).get("message", "")
    deadline = time.monotonic() + 30.0
    while time.monotonic() < deadline:
        if play_session(epilogue) is False:
            return "the run ended in Play; the driver pressed Stop before the invariants"
        time.sleep(0.25)
    return "the run was in Play and did not stop within 30 s of Stop"


def check_problems(epilogue: Epilogue) -> dict[str, Any]:
    """Problems as the scenario left them (read right after the last step)."""

    expected = int((epilogue.scenario.get("invariants") or {}).get("problems", 0))
    reply = epilogue.call({"cmd": "rpc", "method": "get_problems", "params": {}}, timeout=60.0)
    if not reply.get("ok"):
        return {"status": "red",
                "reason": "get_problems: " + (reply.get("error") or {}).get("message", "")}
    problems = (reply["result"]["result"] or {}).get("problems") or []
    found: dict[str, Any] = {
        "count": len(problems),
        "expected": expected,
        "problems": [
            f"{p.get('severity')}: {p.get('code')}: {str(p.get('message'))[:200]}"
            for p in problems
        ],
    }
    if len(problems) != expected:
        found.update(
            status="red",
            reason=f"Problems holds {len(problems)}, the scenario declares {expected}: "
            + "; ".join(found["problems"][:3]),
        )
    else:
        found["status"] = "green"
    return found


def check_health(epilogue: Epilogue) -> dict[str, Any]:
    result: dict[str, Any] = {"invariant": "health"}
    reasons = []
    problems = epilogue.problems_at_end or {"status": "red", "reason": "Problems were not read"}
    result["problems"] = problems
    if problems["status"] == "red":
        reasons.append(problems["reason"])
    alive = epilogue.call({"cmd": "wait_engine", "timeout_ms": 30000}, timeout=60.0)
    result["engine_alive"] = bool(alive.get("ok"))
    if not alive.get("ok"):
        reasons.append("the engine does not answer: " + (alive.get("error") or {}).get("message", ""))
    scan_logs(epilogue)
    allowed = log_allowlist(epilogue)
    flagged = []
    allowed_hits = []
    for log, lines in sorted(epilogue.log_lines.items()):
        for line in sorted(lines):
            reason = next((why for pattern, why in allowed if pattern.search(line)), None)
            if reason:
                allowed_hits.append({"log": log, "line": line[:2000], "allowed": reason})
            else:
                flagged.append({"log": log, "line": line[:2000]})
    result["log_errors"] = flagged
    result["log_allowed"] = allowed_hits
    if flagged:
        reasons.append(f"{len(flagged)} error line(s) in the logs, the first: {flagged[0]['line'][:200]}")
    result["status"] = "red" if reasons else "green"
    result["reason"] = " / ".join(reasons)
    return result


def run_invariants(run: Any, epilogue: Epilogue) -> list[dict[str, Any]]:
    """Each invariant the scenario has on, in order; recorded as steps."""

    outcomes = []
    # A scenario that ends in Play (or paused) is stopped first: Save, Undo
    # and opening another project all wait for Stop, which puts the scene back
    # as it was when Play was pressed (its kept edits apart). The documents
    # compared are the ones after that Stop.
    epilogue.stopped_play = stop_play(epilogue)
    # Read before anything below moves the documents: what the scenario left.
    try:
        epilogue.problems_at_end = check_problems(epilogue)
    except Exception as error:  # noqa: BLE001 - a report, not a crash
        epilogue.problems_at_end = {"status": "red", "reason": f"get_problems failed: {error!r}"}
    scan_logs(epilogue)
    # QD1_INVARIANTS=undo,health runs only those (for measuring one at a time).
    only = {name.strip() for name in os.environ.get("QD1_INVARIANTS", "").split(",") if name.strip()}
    if only - set(INVARIANTS):
        reason = "unknown QD1_INVARIANTS: " + ", ".join(sorted(only - set(INVARIANTS)))
        run.record("invariant", "invariant configuration", ok=False, detail=reason,
                   started=time.perf_counter())
        return [{"invariant": "configuration", "status": "red", "reason": reason}]
    for name, check in (("undo", check_undo), ("reopen", check_reopen), ("health", check_health)):
        if only and name not in only:
            continue
        started = time.perf_counter()
        try:
            mode, why = invariant_mode(epilogue.scenario, name)
        except InvariantConfigError as error:
            run.record("invariant", f"invariant {name}", ok=False, detail=str(error), started=started)
            outcomes.append({"invariant": name, "status": "red", "reason": str(error)})
            continue
        if mode == "off":
            run.record(
                "invariant", f"invariant {name}: off", ok=True,
                detail=f"off: {why}", started=started,
            )
            outcomes.append({"invariant": name, "status": "off", "reason": why})
            continue
        try:
            outcome = check(epilogue)
        except Exception as error:  # noqa: BLE001 - a report, not a crash
            outcome = {"invariant": name, "status": "red", "check_error": True,
                       "reason": f"the check failed: {error!r}"}
        outcome["duration_s"] = round(time.perf_counter() - started, 1)
        status = outcome["status"]
        detail = f"{status}: {outcome.get('reason', '')}".rstrip(": ")
        ok = status != "red"
        if status == "red" and mode == "known_red" and not outcome.get("check_error"):
            outcome["status"] = status = "known_red"
            outcome["ticket"] = why
            detail = f"KNOWN RED ({why}): {outcome.get('reason', '')}"
            ok = True
        elif status != "red" and mode == "known_red":
            detail = f"{status} although marked known_red ({why}); the mark can come off. {detail}"
        run.record(
            "invariant", f"invariant {name}", ok=ok, response=outcome,
            detail=detail, started=started,
        )
        outcomes.append(outcome)
    return outcomes
