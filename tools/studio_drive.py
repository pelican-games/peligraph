#!/usr/bin/env -S uv run --quiet --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Drive Pelican Studio through its `--automation` channel and judge a scenario.

    uv run tools/studio_drive.py \
        --studio build/dist/Debug/pelican_studio.exe \
        --scenario test/studio_scenarios/graph_read.json \
        --project <a copy of a project, in a temp directory> \
        --report out/graph_read/report.json

The scenario is a JSON file of ordered steps.  Every step is either a *command*
(`{"cmd": ...}`), sent verbatim to Studio, or an *expectation* (`{"expect":
...}`), which issues the command it needs and compares the answer.  The exit
code is non-zero when any step failed or Studio did not exit cleanly, and
`report.json` says which one and why:
one line per step in `summary`, the reasons in `failures`, and a screenshot of
the window as it stood when a step failed.

Nothing here knows what a graph is.  What it knows is how to talk to a window
and how to write down what happened, so an agent reading `report.json` can say
"green" or "red, because ..." without having watched the run.
"""

from __future__ import annotations

import argparse
import json
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any

# QD-1: the checks every scenario gets after its last step. The module sits
# beside this script, which Python puts first on sys.path.
import studio_invariants

REPORT_SCHEMA = "pelican.studio_scenario_report"
REPORT_VERSION = 1
# The player reads it when no --audio-backend is given (AU-3).
AUDIO_BACKEND_VARIABLE = "PELICAN_AUDIO_BACKEND"
# PX-2: the engine's viewport, in picture pixels, for the whole of every run.
# A scenario reads pixels back as fractions of the picture, and the camera's
# vertical field of view is fixed, so a point "beside the box" stays beside it
# only while the picture keeps its shape; left to the layout, the viewport
# changed shape whenever a panel was added to Studio. 16:10 is the nearest
# common shape to the 700x426 (1.64) most points were first written against,
# and this size is about what the default layout gave the viewport anyway, so
# the window and its panels barely move. A scenario can name another size with
# a top-level "viewport": [width, height], or "viewport": null to leave the
# viewport to the layout (one that checks the layout itself).
DEFAULT_VIEWPORT = (720, 450)
# FX-7: only refusals made before an operation can be safely retried.
ENABLED_WAIT_TIMEOUT_S = 15.0
ENABLED_WAIT_POLL_S = 0.1
_ENABLED_WAIT_COMMANDS = frozenset({"click", "double_click", "action", "select", "history", "drag", "key", "type"})


class AutomationError(RuntimeError):
    """The channel itself failed: no connection, no reply, a broken pipe."""


# ---------------------------------------------------------------------------
# The channel


class Watchdog(threading.Thread):
    """The one thing that can end a wait on a pipe that will never answer.

    A Windows named pipe opened the ordinary way is a *synchronous* handle:
    Windows serialises operations on it, so a read parked in the kernel blocks
    the next write on the same handle -- and a reader thread would deadlock the
    writer rather than rescue it.  So the read is plain and blocking, and this
    is what breaks it: on a missed deadline it closes Studio, the pipe ends, and
    the blocked read returns to raise like any other channel failure.
    """

    def __init__(self) -> None:
        super().__init__(daemon=True)
        self._lock = threading.Lock()
        self._deadline: tuple[float, str] | None = None
        self.on_trip: Any = None
        self.tripped: str | None = None

    def arm(self, seconds: float, what: str) -> None:
        with self._lock:
            self._deadline = (time.monotonic() + seconds, what)

    def disarm(self) -> None:
        with self._lock:
            self._deadline = None

    def run(self) -> None:
        while True:
            time.sleep(0.5)
            with self._lock:
                deadline = self._deadline
                if deadline is None or time.monotonic() < deadline[0]:
                    continue
                self._deadline = None
            self.tripped = deadline[1]
            print(f"watchdog: {deadline[1]} never answered; closing Studio", flush=True)
            if self.on_trip is not None:
                self.on_trip()


class Channel:
    """One newline-delimited JSON conversation with a running Studio."""

    def __init__(self, handle: Any, watchdog: Watchdog) -> None:
        self._handle = handle
        self._buffer = b""
        self._next_id = 1
        self._watchdog = watchdog

    @classmethod
    def connect(cls, pipe_name: str, deadline: float, watchdog: Watchdog) -> "Channel":
        path = local_socket_path(pipe_name)
        last: OSError | None = None
        while time.monotonic() < deadline:
            try:
                return cls(open(path, "r+b", buffering=0), watchdog)
            except OSError as error:  # not there yet, or momentarily busy
                last = error
                time.sleep(0.05)
        raise AutomationError(f"could not connect to {path}: {last}")

    def send(self, request: dict[str, Any], timeout: float) -> dict[str, Any]:
        request = dict(request)
        request["id"] = self._next_id
        self._next_id += 1
        payload = json.dumps(request, separators=(",", ":")).encode() + b"\n"
        # A little past the caller's own patience: the command is expected to
        # answer within `timeout`, and the watchdog is for when it cannot.
        self._watchdog.arm(timeout + 15.0, f"{request.get('cmd')} (id {request['id']})")
        try:
            try:
                self._handle.write(payload)
                self._handle.flush()
            except OSError as error:
                raise AutomationError(
                    f"writing {request.get('cmd')}: {error}"
                ) from error
            return self._read_reply(request["id"])
        finally:
            self._watchdog.disarm()

    def _read_reply(self, request_id: int) -> dict[str, Any]:
        while True:
            newline = self._buffer.find(b"\n")
            if newline >= 0:
                line, self._buffer = self._buffer[:newline], self._buffer[newline + 1 :]
                reply = json.loads(line.decode("utf-8"))
                if reply.get("id") == request_id:
                    return reply
                continue
            try:
                chunk = self._handle.read(4096)
            except OSError as error:
                raise AutomationError(f"reading reply: {error}") from error
            if not chunk:
                raise AutomationError(
                    "Studio closed the automation channel"
                    + (
                        f" (watchdog: {self._watchdog.tripped} never answered)"
                        if self._watchdog.tripped
                        else ""
                    )
                )
            self._buffer += chunk

    def close(self) -> None:
        try:
            self._handle.close()
        except OSError:
            pass


def local_socket_path(name: str) -> str:
    """Where QLocalServer put the socket, spelled the way this platform spells it."""

    if os.name == "nt":
        return rf"\\.\pipe\{name}"
    return f"/tmp/{name}"


def local_socket_exists(name: str) -> bool:
    path = local_socket_path(name)
    if os.name == "nt":
        try:
            # Listing the pipe namespace sees a server that is listening but has
            # no free instance, which a plain open() would only report as busy.
            if name in os.listdir(r"\\.\pipe"):
                return True
        except OSError:
            pass
    try:
        handle = open(path, "r+b", buffering=0)
    except OSError:
        return False
    handle.close()
    return True


# ---------------------------------------------------------------------------
# Studio as a child process


class Studio:
    def __init__(self, command: list[str], log_path: Path, env: dict[str, str]) -> None:
        self.command = command
        self.log_path = log_path
        log_path.parent.mkdir(parents=True, exist_ok=True)
        self._log = log_path.open("w", encoding="utf-8", errors="replace")
        self._lines: "queue.Queue[str]" = queue.Queue()
        self.process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
            env=env,
        )
        self._pump = threading.Thread(target=self._drain, daemon=True)
        self._pump.start()

    def _drain(self) -> None:
        assert self.process.stdout is not None
        for line in self.process.stdout:
            self._log.write(line)
            self._log.flush()
            self._lines.put(line.rstrip("\n"))
        self._lines.put("")

    def wait_for_line(self, needle: str, timeout: float) -> str | None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                line = self._lines.get(timeout=0.2)
            except queue.Empty:
                if self.process.poll() is not None:
                    return None
                continue
            if needle in line:
                return line
        return None

    def wait_for_exit(self, timeout: float) -> int | None:
        try:
            return self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def stop(self) -> int | None:
        if self.process.poll() is None:
            self.process.terminate()
            code = self.wait_for_exit(10)
            if code is None:
                self.process.kill()
                code = self.wait_for_exit(10)
        self._log.flush()
        self._log.close()
        return self.process.poll()


# ---------------------------------------------------------------------------
# EW-2: the engine's windows, watched from outside


class EngineWindowWatch(threading.Thread):
    """Every top-level window a process Studio started puts on the desktop.

    Studio embeds the engine's window only once the project has loaded, so
    whether a window shows outside Studio before then is a question about the
    time between two steps, not about any one moment a step could look at.
    This thread asks it from the moment Studio is launched: every few
    milliseconds it lists the top-level windows of Studio's child processes
    and writes down each one that is visible.  An embedded window is Studio's
    child and is no longer top-level, so it drops out of the list by itself.

    It also counts, per engine, the samples in which the engine's marked render
    window (EW-1) was still top-level -- the load, before the embed.  A pass
    needs a few of those, so a watch that never saw the load cannot pass.
    """

    MARK = "PelicanEngineRenderWindow"
    INTERVAL_S = 0.01

    def __init__(self, studio_pid: int) -> None:
        super().__init__(daemon=True)
        import ctypes
        from ctypes import wintypes

        self._ctypes = ctypes
        self._wintypes = wintypes
        self._user32 = ctypes.WinDLL("user32", use_last_error=True)
        self._kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self._user32.GetPropW.restype = wintypes.HANDLE
        self._user32.GetPropW.argtypes = [wintypes.HWND, wintypes.LPCWSTR]
        self._user32.GetWindow.restype = wintypes.HWND
        self._user32.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
        self._user32.IsWindowVisible.argtypes = [wintypes.HWND]
        self._user32.IsWindow.argtypes = [wintypes.HWND]
        self._user32.GetAncestor.restype = wintypes.HWND
        self._user32.GetAncestor.argtypes = [wintypes.HWND, wintypes.UINT]
        self._user32.GetWindowThreadProcessId.argtypes = [
            wintypes.HWND, ctypes.POINTER(wintypes.DWORD)
        ]
        self._user32.GetWindowTextW.argtypes = [
            wintypes.HWND, wintypes.LPWSTR, ctypes.c_int
        ]
        self._enum_proc = ctypes.WINFUNCTYPE(
            wintypes.BOOL, wintypes.HWND, wintypes.LPARAM
        )
        self._kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
        self._kernel32.CreateToolhelp32Snapshot.argtypes = [
            wintypes.DWORD, wintypes.DWORD
        ]
        self._kernel32.CloseHandle.argtypes = [wintypes.HANDLE]

        class ProcessEntry(ctypes.Structure):
            _fields_ = [
                ("dwSize", wintypes.DWORD),
                ("cntUsage", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t),
                ("th32ModuleID", wintypes.DWORD),
                ("cntThreads", wintypes.DWORD),
                ("th32ParentProcessID", wintypes.DWORD),
                ("pcPriClassBase", wintypes.LONG),
                ("dwFlags", wintypes.DWORD),
                ("szExeFile", wintypes.WCHAR * 260),
            ]

        self._process_entry = ProcessEntry
        for walk in (self._kernel32.Process32FirstW, self._kernel32.Process32NextW):
            walk.restype = wintypes.BOOL
            walk.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
        self._user32.EnumWindows.argtypes = [self._enum_proc, wintypes.LPARAM]
        self.studio_pid = studio_pid
        self._lock = threading.Lock()
        self._stop_requested = threading.Event()
        self._origin = time.monotonic()
        self.samples = 0
        self.errors: list[str] = []
        # pid -> {"exe", "hidden_samples", "visible": {hwnd: {...}}}
        self.engines: dict[int, dict[str, Any]] = {}

    def stop(self) -> None:
        self._stop_requested.set()

    def run(self) -> None:
        while not self._stop_requested.is_set():
            try:
                self._sample()
            except Exception as error:  # noqa: BLE001 - a report, not a crash
                with self._lock:
                    if len(self.errors) < 10:
                        self.errors.append(repr(error))
            self._stop_requested.wait(self.INTERVAL_S)

    def _children(self) -> dict[int, str]:
        ctypes = self._ctypes
        snapshot = self._kernel32.CreateToolhelp32Snapshot(0x2, 0)  # PROCESS
        if snapshot in (None, self._wintypes.HANDLE(-1).value):
            return {}
        children: dict[int, str] = {}
        try:
            entry = self._process_entry()
            entry.dwSize = ctypes.sizeof(entry)
            more = self._kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
            while more:
                if entry.th32ParentProcessID == self.studio_pid:
                    children[int(entry.th32ProcessID)] = entry.szExeFile
                more = self._kernel32.Process32NextW(snapshot, ctypes.byref(entry))
        finally:
            self._kernel32.CloseHandle(snapshot)
        return children

    def _sample(self) -> None:
        children = self._children()
        if not children:
            with self._lock:
                self.samples += 1
            return
        ctypes = self._ctypes
        wintypes = self._wintypes
        user32 = self._user32
        seen: list[tuple[int, int, bool, bool, bool, str]] = []

        def visit(hwnd: Any, _: Any) -> bool:
            pid = wintypes.DWORD(0)
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value in children:
                marked = bool(user32.GetPropW(hwnd, self.MARK))
                owned = bool(user32.GetWindow(hwnd, 4))  # GW_OWNER
                visible = bool(user32.IsWindowVisible(hwnd))
                title = ""
                if visible:
                    buffer = ctypes.create_unicode_buffer(256)
                    user32.GetWindowTextW(hwnd, buffer, 256)
                    title = buffer.value
                seen.append((int(pid.value), int(hwnd or 0), marked, owned, visible, title))
            return True

        user32.EnumWindows(self._enum_proc(visit), 0)
        at_ms = round((time.monotonic() - self._origin) * 1000)
        with self._lock:
            self.samples += 1
            for pid, hwnd, marked, owned, visible, title in seen:
                engine = self.engines.setdefault(
                    pid,
                    {"exe": children.get(pid, ""), "hidden_samples": 0,
                     "visible": {}, "marked": set()},
                )
                if marked and not owned:
                    engine["marked"].add(hwnd)
                if marked and not owned and not visible:
                    engine["hidden_samples"] += 1
                if visible:
                    shown = engine["visible"].setdefault(
                        hwnd,
                        {"hwnd": hwnd, "title": title, "marked": marked,
                         "owned": owned, "first_ms": at_ms, "samples": 0},
                    )
                    shown["samples"] += 1
                    shown["last_ms"] = at_ms

    def _shown_in_studio(self, windows: set[int]) -> bool:
        """One of these windows is on screen now, as a child of Studio's."""

        wintypes = self._wintypes
        for hwnd in windows:
            if not self._user32.IsWindow(hwnd) or not self._user32.IsWindowVisible(hwnd):
                continue
            root = self._user32.GetAncestor(hwnd, 2)  # GA_ROOT
            if not root or root == hwnd:
                continue
            pid = wintypes.DWORD(0)
            self._user32.GetWindowThreadProcessId(root, self._ctypes.byref(pid))
            if pid.value == self.studio_pid:
                return True
        return False

    def reading(self) -> dict[str, Any]:
        with self._lock:
            return {
                "samples": self.samples,
                "errors": list(self.errors),
                "engines": [
                    {
                        "pid": pid,
                        "exe": engine["exe"],
                        "hidden_samples": engine["hidden_samples"],
                        "visible_windows": [dict(item) for item in engine["visible"].values()],
                        "shown_in_studio": self._shown_in_studio(engine["marked"]),
                    }
                    for pid, engine in self.engines.items()
                ],
            }


# ---------------------------------------------------------------------------
# Reading a value out of an RPC result


class PathError(LookupError):
    pass


def follow(value: Any, path: str) -> Any:
    """Walk `path` through a decoded JSON value.

    Segments are separated by `.`; `[n]` indexes a list; the segment `$len`
    is the length of the list or object it stands on.  A missing segment is an
    error rather than a None, so a typo in a scenario reads as a typo.
    """

    current = value
    walked: list[str] = []
    for raw in [segment for segment in path.split(".") if segment]:
        pieces = raw.replace("]", "").split("[")
        key, indices = pieces[0], pieces[1:]
        if key == "$len":
            if not isinstance(current, (list, dict, str)):
                raise PathError(
                    f"$len at {'.'.join(walked) or '<root>'} needs a list, "
                    f"object or string, found {type(current).__name__}"
                )
            current = len(current)
            walked.append(key)
            continue
        if key:
            if not isinstance(current, dict):
                raise PathError(
                    f"{'.'.join(walked) or '<root>'} is a "
                    f"{type(current).__name__}, not an object with \"{key}\""
                )
            if key not in current:
                raise PathError(
                    f"{'.'.join(walked) or '<root>'} has no \"{key}\" "
                    f"(it has: {', '.join(sorted(current)) or 'nothing'})"
                )
            current = current[key]
            walked.append(key)
        for index in indices:
            if not isinstance(current, list):
                raise PathError(
                    f"{'.'.join(walked) or '<root>'} is a "
                    f"{type(current).__name__}, not a list"
                )
            position = int(index)
            if position >= len(current) or position < -len(current):
                raise PathError(
                    f"{'.'.join(walked) or '<root>'} has {len(current)} "
                    f"entries; [{position}] is out of range"
                )
            current = current[position]
            walked.append(f"[{index}]")
    return current


def equal(left: Any, right: Any) -> bool:
    # QD-1 (GL-5g's rule 3): a bool is not a number. `True == 1` in Python,
    # and a step that expected `true` of a count of one passed.
    if isinstance(left, bool) or isinstance(right, bool):
        return left is right
    if isinstance(left, (int, float)) and isinstance(right, (int, float)):
        return abs(float(left) - float(right)) <= 1e-9
    if isinstance(left, list) and isinstance(right, list):
        return len(left) == len(right) and all(equal(a, b) for a, b in zip(left, right))
    if isinstance(left, dict) and isinstance(right, dict):
        return left.keys() == right.keys() and all(equal(left[k], right[k]) for k in left)
    return left == right


_MISSING = object()


class StepMisuse(ValueError):
    """A comparison that would pass whatever happened (QD-1, GL-5g's rules)."""


def kind_of(value: Any) -> str:
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "bool"
    if isinstance(value, (int, float)):
        return "number"
    if isinstance(value, str):
        return "string"
    if isinstance(value, list):
        return "list"
    return "object"


def compare(op: str, actual: Any, expected: Any) -> bool:
    """The comparisons a `result` step can ask for.

    `eq` is the one every other expectation uses.  The rest exist because a
    gesture is judged by which way a number moved, not by where it landed.

    QD-1 makes the forms that cannot fail an error rather than a pass:
    `contains` / `excludes` on an object (Python's `in` reads its keys, so a
    value never matched; `has_key` / `lacks_key` ask about a key by name), an
    `excludes` of a value of a kind no element of the list has, and `ne`
    between two kinds (a string is never a number).
    """

    if op == "eq":
        return equal(actual, expected)
    if op == "ne":
        if None not in (actual, expected) and kind_of(actual) != kind_of(expected):
            raise StepMisuse(
                f"ne between a {kind_of(actual)} and a {kind_of(expected)} is always true"
            )
        return not equal(actual, expected)
    if op in ("has_key", "lacks_key"):
        if not isinstance(actual, dict):
            raise StepMisuse(f"{op} needs an object, found a {kind_of(actual)}")
        return (str(expected) in actual) == (op == "has_key")
    if op in ("contains", "excludes"):
        if isinstance(actual, dict):
            raise StepMisuse(
                f"{op} on an object reads its keys; write has_key / lacks_key for a key, "
                "or a path to the value"
            )
        if isinstance(actual, str):
            if not isinstance(expected, str):
                raise StepMisuse(f"{op} in a string needs a string, not a {kind_of(expected)}")
            if expected == "":
                raise StepMisuse(f'{op} "" is decided before the step runs')
        elif isinstance(actual, list):
            kinds = {kind_of(item) for item in actual}
            if op == "excludes" and actual and kind_of(expected) not in kinds:
                raise StepMisuse(
                    f"excludes a {kind_of(expected)} from a list of {', '.join(sorted(kinds))} "
                    "is always true"
                )
            found = any(equal(item, expected) for item in actual)
            return found if op == "contains" else not found
        else:
            raise StepMisuse(f"{op} needs a string or a list, found a {kind_of(actual)}")
        return (expected in actual) if op == "contains" else (expected not in actual)
    if op in {"gt", "ge", "lt", "le"}:
        left, right = float(actual), float(expected)
        return {
            "gt": left > right,
            "ge": left >= right,
            "lt": left < right,
            "le": left <= right,
        }[op]
    raise ValueError(f'unknown comparison "{op}"')


# ---------------------------------------------------------------------------
# The run


class Run:
    def __init__(self, scenario: dict[str, Any], artifacts: Path) -> None:
        self.scenario = scenario
        self.artifacts = artifacts
        self.steps: list[dict[str, Any]] = []
        self.summary: list[str] = []
        self.failures: list[dict[str, Any]] = []
        self.screenshots: list[str] = []
        self.channel: Channel | None = None
        # What a `result` step wrote down for a later step to compare against.
        # A zoom is "bigger than it was", not "1.35": the number a view lands on
        # is the view's business, and a scenario that hard-codes it is testing
        # the arithmetic instead of the gesture.
        self.remembered: dict[str, Any] = {}
        # EW-2: set when the scenario asks for `watch_engine_windows`.
        self.window_watch: EngineWindowWatch | None = None
        # QD-1: what the report says of the run besides pass and fail.
        self.warnings: list[str] = []
        self.remember_names: dict[str, str] = {}
        self.used_names: set[str] = set()
        self.compared_steps = 0
        self.read_steps = 0
        self.file_seen: set[tuple[str, str]] = set()
        self.list_patterns: dict[str, int] = {}

    # -- recording -----------------------------------------------------------

    def record(
        self,
        kind: str,
        label: str,
        *,
        ok: bool,
        request: Any = None,
        response: Any = None,
        detail: str = "",
        started: float = 0.0,
        screenshot: str | None = None,
        waited_for_enabled_ms: int | None = None,
    ) -> dict[str, Any]:
        index = len(self.steps) + 1
        # perf_counter uses the high-resolution performance clock on Windows;
        # monotonic can have a ~15.6 ms tick on Python 3.12. Keep deadline
        # clocks separate from measurements of short GUI operations.
        finished = time.perf_counter()
        duration_ms = (finished - started) * 1000 if started else 0.0
        entry = {
            "index": index,
            "kind": kind,
            "label": label,
            "ok": ok,
            "duration_ms": round(duration_ms),
            "duration_precise_ms": duration_ms,
            "started_perf_s": started,
            "finished_perf_s": finished,
            "request": request,
            "response": response,
            "detail": detail,
        }
        if waited_for_enabled_ms is not None:
            entry["waited_for_enabled_ms"] = waited_for_enabled_ms
        if screenshot:
            entry["screenshot"] = screenshot
        self.steps.append(entry)
        mark = "ok  " if ok else "FAIL"
        line = f"{index:>3} {mark} {entry['duration_ms']:>6}ms  {kind}: {label}"
        if detail:
            line += f" -- {detail}"
        self.summary.append(line)
        # Printed as it happens, not only at the end: a run that is killed for
        # taking too long still says how far it got.
        print(line, flush=True)
        if not ok:
            failure = {
                "step": index,
                "kind": kind,
                "label": label,
                "reason": detail,
                "request": request,
                "response": response,
            }
            if screenshot:
                failure["screenshot"] = screenshot
            self.failures.append(failure)
        return entry

    # -- talking to Studio ---------------------------------------------------

    def call(self, request: dict[str, Any], timeout: float = 120.0) -> dict[str, Any]:
        if self.channel is None:
            raise AutomationError("the automation channel is not open")
        return self.channel.send(request, timeout)

    def capture(self, stem: str) -> str | None:
        """A picture of the window as it stands, for the reader of this report."""

        if self.channel is None:
            return None
        path = self.artifacts / f"{stem}.png"
        try:
            reply = self.call({"cmd": "screenshot", "path": str(path)}, timeout=30.0)
        except AutomationError:
            return None
        if not reply.get("ok"):
            return None
        saved = reply["result"]["path"]
        self.screenshots.append(saved)
        return saved


def expand(value: Any, variables: dict[str, str]) -> Any:
    """Fill `{project}`-style placeholders in a step.

    A scenario names a project it cannot know the path of: the runner copies the
    fixture to a temp directory first, precisely so the test does not edit a
    directory in the repository.  The names come from `--var`.
    """

    if isinstance(value, str):
        for key, replacement in variables.items():
            value = value.replace("{" + key + "}", replacement)
        return value
    if isinstance(value, dict):
        return {key: expand(item, variables) for key, item in value.items()}
    if isinstance(value, list):
        return [expand(item, variables) for item in value]
    return value


def step_label(step: dict[str, Any], index: int) -> str:
    if step.get("label"):
        return str(step["label"])
    if "cmd" in step:
        return str(step["cmd"])
    if "file" in step:
        return f"file {step['file']}"
    return f"{step.get('expect', 'step')} #{index}"


def command_of(step: dict[str, Any]) -> dict[str, Any]:
    return {key: value for key, value in step.items() if key not in _META_KEYS}


_META_KEYS = {"label", "expect", "expect_error", "note", "wait_s"}


def _disabled_operation(request: dict[str, Any], reply: dict[str, Any]) -> bool:
    """Recognise only Studio's pre-operation disabled refusals, not other errors."""
    if request.get("cmd") not in _ENABLED_WAIT_COMMANDS or reply.get("ok"):
        return False
    if request.get("cmd") == "history" and request.get("step") not in {"undo", "redo"}:
        return False
    if request.get("cmd") in {"key", "type"} and not request.get("name"):
        return False
    error = reply.get("error") or {}
    if error.get("code") != "automation.bad_request@1":
        return False
    message = error.get("message")
    return isinstance(message, str) and (
        message == "the window under the point is disabled"
        or (
            message.startswith(('the widget named "', 'the action named "', 'the action "'))
            and message.endswith('" is disabled')
        )
    )


def run_command_step(run: Run, step: dict[str, Any], label: str) -> bool:
    started = time.perf_counter()
    request = command_of(step)
    expected_error = step.get("expect_error")
    timeout = float(step.get("wait_s", 180))
    wait_started = None
    waited_for_enabled_ms = None
    reply = None
    try:
        reply = run.call(request, timeout=timeout)
        if "expect_error" not in step:
            while _disabled_operation(request, reply):
                if wait_started is None:
                    wait_started = time.monotonic()
                remaining = ENABLED_WAIT_TIMEOUT_S - (time.monotonic() - wait_started)
                if remaining <= 0:
                    break
                time.sleep(min(ENABLED_WAIT_POLL_S, remaining))
                # Sleep and channel calls can take longer than requested. Never
                # resend after the deadline, even if the last refusal was disabled.
                remaining = ENABLED_WAIT_TIMEOUT_S - (time.monotonic() - wait_started)
                if remaining <= 0:
                    break
                reply = run.call(request, timeout=min(timeout, remaining))
        if wait_started is not None:
            waited_for_enabled_ms = round((time.monotonic() - wait_started) * 1000)
    except AutomationError as error:
        detail = str(error)
        if wait_started is not None:
            waited_for_enabled_ms = round((time.monotonic() - wait_started) * 1000)
            detail += f"; waited_for_enabled_ms={waited_for_enabled_ms}; last reply: {reply}"
        run.record(
            "cmd", label, ok=False, request=request, detail=detail, started=started,
            response=reply, waited_for_enabled_ms=waited_for_enabled_ms,
        )
        return False
    if expected_error:
        code = (reply.get("error") or {}).get("code")
        if reply.get("ok"):
            run.record(
                "cmd",
                label,
                ok=False,
                request=request,
                response=reply,
                detail=f"expected error {expected_error}, but the command succeeded",
                started=started,
                screenshot=run.capture(f"step{len(run.steps) + 1}_failure"),
            )
            return False
        if code != expected_error:
            run.record(
                "cmd",
                label,
                ok=False,
                request=request,
                response=reply,
                detail=f"expected error {expected_error}, got {code}",
                started=started,
            )
            return False
        run.record(
            "cmd",
            label,
            ok=True,
            request=request,
            response=reply,
            detail=f"refused with {code}, as expected",
            started=started,
        )
        return True
    if not reply.get("ok"):
        error = reply.get("error") or {}
        detail = f"{error.get('code', 'error')}: {error.get('message', '')}"
        if waited_for_enabled_ms is not None:
            detail += (
                f"; waited_for_enabled_ms={waited_for_enabled_ms}"
                f" (limit {ENABLED_WAIT_TIMEOUT_S * 1000:.0f} ms)"
            )
        run.record(
            "cmd",
            label,
            ok=False,
            request=request,
            response=reply,
            detail=detail,
            started=started,
            screenshot=run.capture(f"step{len(run.steps) + 1}_failure"),
            waited_for_enabled_ms=waited_for_enabled_ms,
        )
        return False
    if request.get("cmd") == "screenshot":
        run.screenshots.append(reply["result"]["path"])
    # QA-1's suggestion: a select that had to wait over a second for its entry
    # is a scenario missing a wait before it.
    waited = (reply.get("result") or {}).get("waited_ms") if isinstance(reply.get("result"), dict) else None
    if request.get("cmd") == "select" and isinstance(waited, (int, float)) and waited > 1000:
        run.warnings.append(f'"{label}": select waited {waited:.0f} ms for its entry')
    run.record(
        "cmd", label, ok=True, request=request, response=reply, started=started,
        waited_for_enabled_ms=waited_for_enabled_ms,
    )
    return True


def run_file_step(
    run: Run, step: dict[str, Any], label: str, roots: list[Path]
) -> bool:
    """A file in the project copy, touched or read the way an editor would.

    F-3's scenario has to do what a person does between two Studio gestures:
    edit the shader "New..." wrote, in an editor of their own.  Studio has no
    command for that and should not grow one, so the driver does it itself --
    and only inside the directories this run owns: the project copy, and the
    run's own artifacts, which is where a scenario that starts from File > New
    Project puts the project it creates (F-4).

        {"file": "write",    "path": "{project}/x", "text": "..."}
        {"file": "replace",  "path": "{project}/x", "old": "...", "new": "..."}
        {"file": "contains", "path": "{project}/x", "text": "...", "value": true}
        {"file": "exists",   "path": "{project}/x", "value": true}
        {"file": "copy",     "path": "{project}/x", "from": "{artifacts}/y"}

    `copy` (IA-1s) replaces a file with another byte for byte -- a binary one
    a person bakes outside Studio and copies over the one the project reads,
    which `write` cannot make. Both ends are inside the directories this run
    owns.
    """

    started = time.perf_counter()
    action = str(step["file"])
    request = {key: value for key, value in step.items() if key not in _META_KEYS}
    try:
        path = Path(str(step["path"])).resolve()
        owned = [root.resolve() for root in roots]
        if not any(path == root or root in path.parents for root in owned):
            raise ValueError(
                f"{path} is outside the project copy and the run's artifacts "
                f"({', '.join(str(root) for root in owned)})"
            )
        if action == "write":
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(str(step["text"]).encode("utf-8"))
            detail = f"wrote {path.name}"
        elif action == "replace":
            text = path.read_bytes().decode("utf-8")
            old = str(step["old"])
            if text.count(old) != 1:
                raise ValueError(
                    f"{path.name} holds {text.count(old)} copies of the text to replace"
                )
            path.write_bytes(text.replace(old, str(step["new"])).encode("utf-8"))
            detail = f"replaced one span of {path.name}"
        elif action == "contains":
            found = str(step["text"]) in path.read_bytes().decode("utf-8")
            if found != bool(step.get("value", True)):
                raise ValueError(
                    f"{path.name} {'holds' if found else 'does not hold'} {step['text']!r}"
                )
            if found:
                run.file_seen.add(("contains", f"{path}|{step['text']}"))
            elif step.get("was"):
                # QD-1 (GL-5g's rule 6): "no longer holds" -- this run saw it
                # hold the text first, so the absence is news, not a typo.
                if ("contains", f"{path}|{step['text']}") not in run.file_seen:
                    raise ValueError(
                        f"{path.name} never held {step['text']!r} earlier in this run "
                        "(was: true says it did)"
                    )
            elif ("contains", f"{path}|{step['text']}") not in run.file_seen:
                run.warnings.append(
                    f'"{label}": {path.name} does not hold {step["text"]!r}, and no step of '
                    "this run saw it hold it (a misspelt text passes too; was: true says it did)"
                )
            detail = f"{path.name} {'holds' if found else 'does not hold'} the text"
        elif action == "copy":
            source = Path(str(step["from"])).resolve()
            if not any(source == root or root in source.parents for root in owned):
                raise ValueError(
                    f"{source} is outside the project copy and the run's artifacts"
                )
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, path)
            detail = f"copied {source.name} over {path.name}"
        elif action == "exists":
            present = path.is_file()
            if present != bool(step.get("value", True)):
                raise ValueError(f"{path.name} {'exists' if present else 'is missing'}")
            if present:
                run.file_seen.add(("exists", str(path)))
            elif step.get("was") and ("exists", str(path)) not in run.file_seen:
                raise ValueError(
                    f"{path.name} was never seen in this run (was: true says it was)"
                )
            elif not present and ("exists", str(path)) not in run.file_seen:
                run.warnings.append(
                    f'"{label}": {path.name} is missing, and no step of this run saw it '
                    "(a misspelt path passes too; was: true says it was there)"
                )
            detail = f"{path.name} {'exists' if present else 'is missing'}"
        elif action == "list":
            # QD-1 (GL-5g's rule 6): what a directory holds, so "nothing new
            # was written in shaders/" can be said without guessing a name.
            names = sorted(entry.name for entry in path.iterdir()) if path.is_dir() else []
            if "value" in step and names != sorted(step["value"]):
                raise ValueError(f"{path.name}/ holds {names}, expected {sorted(step['value'])}")
            if "count" in step and len(names) != int(step["count"]):
                raise ValueError(f"{path.name}/ holds {len(names)} entries ({names}), expected {step['count']}")
            if "value" not in step and "count" not in step:
                raise ValueError("a list step needs value (the names) or count")
            detail = f"{path.name}/ holds {names}"
        else:
            raise ValueError(f"unknown file action \"{action}\"")
    except (OSError, ValueError, KeyError) as error:
        run.record(
            "file",
            label,
            ok=False,
            request=request,
            detail=str(error),
            started=started,
            screenshot=run.capture(f"step{len(run.steps) + 1}_failure"),
        )
        return False
    run.record("file", label, ok=True, request=request, detail=detail, started=started)
    return True


def run_expect_step(run: Run, step: dict[str, Any], label: str) -> bool:
    started = time.perf_counter()
    kind = str(step["expect"])
    try:
        if kind == "rpc_equals":
            request = {
                "cmd": "rpc",
                "method": step["method"],
                "params": step.get("params", {}),
            }
            if "timeout_ms" in step:
                request["timeout_ms"] = step["timeout_ms"]
            # Some of what the engine holds arrives on its own clock: a
            # placement edit waits out a debounce before it is even sent, so
            # the first reading of it can be one edit behind.  `retry_ms` asks
            # the same question again until it is answered or the time is up;
            # without it the step is a single reading, as it always was.
            deadline = time.monotonic() + float(step.get("retry_ms", 0)) / 1000.0
            while True:
                reply = run.call(request, timeout=float(step.get("wait_s", 180)))
                if not reply.get("ok"):
                    break
                try:
                    reading = follow(reply["result"]["result"], step["path"])
                except (PathError, ValueError):
                    reading = _MISSING
                if equal(reading, step["value"]) or time.monotonic() >= deadline:
                    break
                time.sleep(0.1)
            if not reply.get("ok"):
                error = reply.get("error") or {}
                return _fail(
                    run,
                    kind,
                    label,
                    request,
                    reply,
                    f"{error.get('code', 'error')}: {error.get('message', '')}",
                    started,
                )
            result = reply["result"]["result"]
            try:
                actual = follow(result, step["path"])
            except (PathError, ValueError) as error:
                return _fail(
                    run, kind, label, request, reply, f"{step['path']}: {error}", started
                )
            if not equal(actual, step["value"]):
                return _fail(
                    run,
                    kind,
                    label,
                    request,
                    reply,
                    f"{step['method']} {step['path']}: expected "
                    f"{json.dumps(step['value'])}, got {json.dumps(actual)}",
                    started,
                )
            run.record(
                "expect",
                label,
                ok=True,
                request=request,
                response={"path": step["path"], "value": actual},
                detail=f"{step['method']} {step['path']} == {json.dumps(actual)}",
                started=started,
            )
            return True

        if kind == "result":
            # The general shape: send any command, walk into its result, and
            # either write the value down or judge it.  `view_state` and
            # `menu_list` answer with readings rather than with text, and this
            # is how a scenario asks a question about one.
            request = dict(step.get("command") or {})
            if not request.get("cmd"):
                return _fail(
                    run, kind, label, step, None, "a result step needs a command", started
                )
            path = str(step.get("path", ""))
            has_expectation = "value" in step or "value_of" in step or "value_format" in step
            # QD-1 (GL-5g's rule 2): a reading that is neither compared nor
            # written down for a later step passes whatever it reads. One kept
            # only for the report's reader says so with `record`.
            if not has_expectation and not step.get("remember") and not step.get("record"):
                return _fail(
                    run, kind, label, request, None,
                    "a result step compares (value / value_of / value_format), remembers "
                    "for a later step (remember), or is a reading for the record (record: true)",
                    started,
                )
            if step.get("remember"):
                run.remember_names.setdefault(str(step["remember"]), label)
            if "value_of" in step:
                run.used_names.add(str(step["value_of"]))
            if "value_format" in step:
                import string

                run.used_names.update(
                    field for _, field, _, _ in string.Formatter().parse(str(step["value_format"]))
                    if field
                )
            expected: Any = None
            shown = ""
            if "value" in step:
                expected = step["value"]
                shown = json.dumps(expected)
            elif "value_of" in step:
                expected = run.remembered.get(str(step["value_of"]), _MISSING)
                if expected is _MISSING:
                    return _fail(
                        run,
                        kind,
                        label,
                        request,
                        None,
                        f"no earlier step remembered \"{step['value_of']}\"",
                        started,
                    )
                shown = f"{step['value_of']} ({json.dumps(expected)})"
                # PM-3a: "one step later" is the remembered number plus one.
                if "plus" in step:
                    expected = expected + step["plus"]
                    shown = f"{step['value_of']} + {json.dumps(step['plus'])} ({json.dumps(expected)})"
            elif "value_format" in step:
                # PM-3a: a text made of remembered readings, e.g.
                # "Paused · frame {paused_frame}".
                try:
                    expected = str(step["value_format"]).format(**run.remembered)
                except (KeyError, IndexError, ValueError) as error:
                    return _fail(
                        run, kind, label, request, None,
                        f"value_format: nothing remembered for {error}", started,
                    )
                shown = json.dumps(expected)
            op = str(step.get("op", "eq"))
            # Like `rpc_equals`, `retry_ms` asks the same question again until
            # the answer satisfies the step or the time is up: what the engine
            # draws after a file is saved arrives on the engine's clock, not
            # the driver's.  Without it the step is a single reading.
            deadline = time.monotonic() + float(step.get("retry_ms", 0)) / 1000.0
            while True:
                reply = run.call(request, timeout=float(step.get("wait_s", 120)))
                if not reply.get("ok"):
                    error = reply.get("error") or {}
                    return _fail(
                        run,
                        kind,
                        label,
                        request,
                        reply,
                        f"{error.get('code', 'error')}: {error.get('message', '')}",
                        started,
                    )
                try:
                    actual = follow(reply["result"], path)
                except (PathError, ValueError) as error:
                    return _fail(run, kind, label, request, reply, f"{path}: {error}", started)
                if not has_expectation:
                    break
                try:
                    satisfied = compare(op, actual, expected)
                except (ValueError, TypeError) as error:
                    return _fail(run, kind, label, request, reply, str(error), started)
                if satisfied or time.monotonic() >= deadline:
                    break
                time.sleep(0.2)
            if step.get("remember"):
                run.remembered[str(step["remember"])] = actual
            if not has_expectation:
                run.record(
                    "expect",
                    label,
                    ok=True,
                    request=request,
                    response={"path": path, "value": actual},
                    detail=f"{request['cmd']} {path} = {json.dumps(actual)}"
                    + (f", remembered as {step['remember']}" if step.get("remember") else ""),
                    started=started,
                )
                return True
            if not satisfied:
                return _fail(
                    run,
                    kind,
                    label,
                    request,
                    reply,
                    f"{request['cmd']} {path}: {json.dumps(actual)} is not "
                    f"{op} {shown}",
                    started,
                )
            # QD-1 (GL-5g's rule 4): "and it stays so" -- the same reading
            # again once the engine has drawn this many more frames.
            if step.get("stable_for_frames"):
                frames = int(step["stable_for_frames"])
                status = run.call({"cmd": "rpc", "method": "get_status", "params": {}}, timeout=60.0)
                published = (((status.get("result") or {}).get("result") or {}).get("render") or {}).get(
                    "published_frames"
                )
                if not status.get("ok") or not isinstance(published, (int, float)):
                    return _fail(run, kind, label, request, status,
                                 "stable_for_frames: get_status has no render.published_frames", started)
                waited = run.call(
                    {"cmd": "wait_engine", "frames": int(published) + frames, "timeout_ms": 60000},
                    timeout=90.0,
                )
                if not waited.get("ok"):
                    return _fail(run, kind, label, request, waited,
                                 f"stable_for_frames: the engine did not draw {frames} more frames", started)
                again = run.call(request, timeout=float(step.get("wait_s", 120)))
                try:
                    later = follow(again["result"], path) if again.get("ok") else _MISSING
                except (PathError, ValueError):
                    later = _MISSING
                if later is _MISSING or not equal(later, actual):
                    return _fail(
                        run, kind, label, request, again,
                        f"{request['cmd']} {path}: {json.dumps(actual)}, then "
                        f"{json.dumps(None if later is _MISSING else later)} {frames} frames later",
                        started,
                    )
            run.record(
                "expect",
                label,
                ok=True,
                request=request,
                response={"path": path, "value": actual},
                detail=f"{request['cmd']} {path} {op} {shown} ({json.dumps(actual)})"
                + (f", the same {step['stable_for_frames']} frames later" if step.get("stable_for_frames") else ""),
                started=started,
            )
            return True

        if kind == "text_equals":
            request = {"cmd": "text", "name": step["name"]}
            reply = run.call(request, timeout=60.0)
            if not reply.get("ok"):
                error = reply.get("error") or {}
                return _fail(
                    run,
                    kind,
                    label,
                    request,
                    reply,
                    f"{error.get('code', 'error')}: {error.get('message', '')}",
                    started,
                )
            actual = reply["result"]["text"]
            if actual != step["value"]:
                return _fail(
                    run,
                    kind,
                    label,
                    request,
                    reply,
                    f"{step['name']}: expected {json.dumps(step['value'])}, "
                    f"got {json.dumps(actual)}",
                    started,
                )
            run.record(
                "expect",
                label,
                ok=True,
                request=request,
                response=reply["result"],
                detail=f"{step['name']} == {json.dumps(actual)}",
                started=started,
            )
            return True

        if kind == "visible":
            request = {"cmd": "text", "name": step["name"]}
            reply = run.call(request, timeout=60.0)
            # QD-1 (GL-5g's rule 5): "not visible" is a question about a thing
            # that exists. A name that resolves to nothing is not hidden, it
            # is a typo -- or the thing is gone, which `wait` with `missing`
            # says.
            if not reply.get("ok") and not bool(step.get("value", True)):
                return _fail(
                    run, kind, label, request, reply,
                    f"{step['name']} names nothing; visible false needs a widget that is "
                    "there and hidden (write a wait with missing: true for one that is gone)",
                    started,
                )
            visible = bool(reply.get("ok")) and bool(reply["result"]["visible"])
            if visible != bool(step.get("value", True)):
                return _fail(
                    run,
                    kind,
                    label,
                    request,
                    reply,
                    f"{step['name']} visible={visible}, expected "
                    f"{bool(step.get('value', True))}",
                    started,
                )
            run.record(
                "expect",
                label,
                ok=True,
                request=request,
                response=reply.get("result"),
                detail=f"{step['name']} visible={visible}",
                started=started,
            )
            return True

        if kind == "list_count":
            request = {"cmd": "list", "pattern": step["pattern"]}
            reply = run.call(request, timeout=60.0)
            if not reply.get("ok"):
                error = reply.get("error") or {}
                return _fail(
                    run,
                    kind,
                    label,
                    request,
                    reply,
                    f"{error.get('code', 'error')}: {error.get('message', '')}",
                    started,
                )
            names = [entry["name"] for entry in reply["result"]["widgets"]]
            run.list_patterns[step["pattern"]] = max(
                run.list_patterns.get(step["pattern"], 0), len(names)
            )
            if len(names) != int(step["value"]):
                return _fail(
                    run,
                    kind,
                    label,
                    request,
                    reply,
                    f"\"{step['pattern']}\" matched {len(names)} "
                    f"({', '.join(names) or 'nothing'}), expected {step['value']}",
                    started,
                )
            run.record(
                "expect",
                label,
                ok=True,
                request=request,
                response={"matched": names},
                detail=f"\"{step['pattern']}\" matched {len(names)}",
                started=started,
            )
            return True

        if kind == "engine_windows":
            # EW-2: since Studio was launched, no process it started has shown
            # a top-level window, and the watch saw each engine's load (its
            # marked window still top-level) often enough to have been looking.
            watch = run.window_watch
            if watch is None:
                return _fail(
                    run, kind, label, step, None,
                    "the scenario does not set watch_engine_windows", started,
                )
            reading = watch.reading()
            minimum = int(step.get("min_hidden_samples", 3))
            watched = [
                engine for engine in reading["engines"]
                if engine["hidden_samples"] >= minimum
            ]
            shown = [
                f"pid {engine['pid']} ({engine['exe']}): \"{window['title']}\" "
                f"visible from {window['first_ms']} ms for {window['samples']} samples"
                for engine in reading["engines"]
                for window in engine["visible_windows"]
            ]
            reasons = []
            if shown:
                reasons.append("a window showed outside Studio: " + "; ".join(shown))
            if len(watched) < int(step["engines"]):
                reasons.append(
                    f"saw {len(watched)} engine load(s) with at least {minimum} "
                    f"samples each, expected {step['engines']}"
                )
            if reading["errors"]:
                reasons.append("the watch failed: " + "; ".join(reading["errors"]))
            # The window is hidden until Studio embeds it; this says Studio
            # did show it then, inside its own window, for the newest engine.
            if not reading["engines"] or not reading["engines"][-1]["shown_in_studio"]:
                reasons.append("the newest engine's window is not on screen inside Studio")
            if reasons:
                return _fail(run, kind, label, step, reading, " / ".join(reasons), started)
            run.record(
                "expect",
                label,
                ok=True,
                request=step,
                response=reading,
                detail=(
                    f"{reading['samples']} samples, no window outside Studio; loads seen: "
                    + ", ".join(
                        f"pid {engine['pid']} x{engine['hidden_samples']}"
                        for engine in watched
                    )
                ),
                started=started,
            )
            return True

        if kind == "screenshot":
            path = Path(step.get("path") or f"{step.get('name', 'window')}.png")
            if not path.is_absolute():
                path = run.artifacts / path
            request: dict[str, Any] = {"cmd": "screenshot", "path": str(path)}
            if step.get("name"):
                request["name"] = step["name"]
            if step.get("mode"):
                request["mode"] = step["mode"]
            reply = run.call(request, timeout=60.0)
            if not reply.get("ok"):
                error = reply.get("error") or {}
                return _fail(
                    run,
                    kind,
                    label,
                    request,
                    reply,
                    f"{error.get('code', 'error')}: {error.get('message', '')}",
                    started,
                )
            run.screenshots.append(reply["result"]["path"])
            run.record(
                "expect",
                label,
                ok=True,
                request=request,
                response=reply["result"],
                detail=f"saved {reply['result']['path']}",
                started=started,
            )
            return True
    except AutomationError as error:
        return _fail(run, kind, label, step, None, str(error), started)
    except KeyError as error:
        return _fail(run, kind, label, step, None, f"the step is missing {error}", started)

    return _fail(run, kind, label, step, None, f"unknown expectation \"{kind}\"", started)


def _fail(
    run: Run,
    kind: str,
    label: str,
    request: Any,
    response: Any,
    detail: str,
    started: float,
) -> bool:
    run.record(
        "expect",
        label,
        ok=False,
        request=request,
        response=response,
        detail=detail,
        started=started,
        screenshot=run.capture(f"step{len(run.steps) + 1}_failure"),
    )
    return False


def fix_viewport(run: Run, scenario: dict[str, Any]) -> bool:
    """Hold the viewport at the scenario's size before its first step."""

    wanted = scenario.get("viewport", list(DEFAULT_VIEWPORT))
    if wanted is None:
        return True
    started = time.perf_counter()
    if not (
        isinstance(wanted, list)
        and len(wanted) == 2
        and all(isinstance(value, int) and value > 0 for value in wanted)
    ):
        run.record(
            "viewport",
            "the viewport's size",
            ok=False,
            request={"viewport": wanted},
            detail='"viewport" is [width, height] in picture pixels, or null',
            started=started,
        )
        return False
    request = {"cmd": "viewport_size", "width": wanted[0], "height": wanted[1]}
    reply = run.call(request, timeout=30.0)
    result = reply.get("result") or {}
    got = [result.get("width"), result.get("height")]
    ok = bool(reply.get("ok"))
    if ok:
        detail = f"{got[0]}x{got[1]} picture pixels (window {result.get('window')})"
        # At a scale that does not divide the size, the picture is a pixel off.
        if not all(isinstance(g, int) for g in got) or any(
            abs(g - w) > 1 for g, w in zip(got, wanted)
        ):
            ok = False
            detail = f"asked for {wanted[0]}x{wanted[1]}, the engine is given {got[0]}x{got[1]}"
    else:
        error = reply.get("error") or {}
        detail = f"{error.get('code')}: {error.get('message')}"
    run.record(
        "viewport",
        f"the viewport is held at {wanted[0]}x{wanted[1]}",
        ok=ok,
        request=request,
        response=reply,
        detail=detail,
        started=started,
    )
    return ok


def finish_warnings(run: Run) -> None:
    """QD-1: what passed but says little.

    A name remembered and never compared (GL-5g's rule 2), and a list_count of
    0 whose pattern matched nothing in any list_count of the run (rule 7):
    both pass whatever happened.
    """

    for remembered, where in sorted(run.remember_names.items()):
        if remembered not in run.used_names:
            run.warnings.append(
                f'"{where}" remembered {remembered}, and no later step compared it'
            )
    for pattern, most in sorted(run.list_patterns.items()):
        if most == 0:
            run.warnings.append(
                f'list_count "{pattern}" matched nothing anywhere in the run: '
                "a count of 0 would pass for a misspelt name too"
            )
    for warning in run.warnings:
        print(f"warning: {warning}", flush=True)


def probe_no_automation(
    run: Run, studio: Path, pipe: str, artifacts: Path, env: dict[str, str]
) -> bool:
    """Acceptance 1, first half: without the flag there is no channel at all."""

    started = time.perf_counter()
    control = Studio(
        [str(studio)], artifacts / "studio_no_automation.log", env
    )
    try:
        control.wait_for_line("starting Pelican Studio", 60)
        # Give a server that was going to be opened every chance to appear.
        time.sleep(2.0)
        present = local_socket_exists(pipe)
    finally:
        control.stop()
    if present:
        run.record(
            "control",
            "a Studio launched without --automation opens no channel",
            ok=False,
            request={"command": control.command, "socket": local_socket_path(pipe)},
            detail=f"{local_socket_path(pipe)} exists without --automation",
            started=started,
        )
        return False
    run.record(
        "control",
        "a Studio launched without --automation opens no channel",
        ok=True,
        request={"command": control.command, "socket": local_socket_path(pipe)},
        detail=f"{local_socket_path(pipe)} is absent",
        started=started,
    )
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--studio", required=True, type=Path)
    parser.add_argument("--scenario", required=True, type=Path)
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--artifacts", type=Path, default=None)
    parser.add_argument("--pipe", default=None)
    parser.add_argument("--player", type=Path, default=None)
    parser.add_argument(
        "--var",
        action="append",
        default=[],
        metavar="NAME=VALUE",
        help="a value a step can name as {NAME}; `project` and `artifacts` are set already",
    )
    parser.add_argument(
        "--launch-timeout", type=float, default=120.0, help="seconds to wait for the channel"
    )
    arguments = parser.parse_args()
    # A step's detail quotes what a panel says, and a panel may say it with a
    # character the console's code page has no byte for (a "·"). The line is
    # for a reader; it must not be what ends the run.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="backslashreplace")

    scenario = json.loads(arguments.scenario.read_text(encoding="utf-8"))
    name = scenario.get("name") or arguments.scenario.stem
    artifacts = arguments.artifacts or arguments.report.parent
    artifacts.mkdir(parents=True, exist_ok=True)
    pipe = arguments.pipe or f"pelican_studio_{name}_{os.getpid()}"

    env = dict(os.environ)
    if arguments.player is not None:
        env["PELICAN_STUDIO_PLAYER"] = str(arguments.player)
    # Q-11: a scenario is silent. Studio's embedded player inherits this
    # environment and, told `null`, never opens the audio device (AU-3). A
    # caller that already chose a backend keeps it; an empty value is unset to
    # the player too, so it is replaced like an absent one.
    if not env.get(AUDIO_BACKEND_VARIABLE):
        env[AUDIO_BACKEND_VARIABLE] = "null"
    # Studio keeps its dock layouts per project in the user's config
    # directory and restores them on the next start, so one scenario (or a
    # person using Studio) would hand the next its windows. Each run starts
    # from the default layout in a directory of its own.
    studio_config = artifacts / "studio_config"
    shutil.rmtree(studio_config, ignore_errors=True)
    studio_config.mkdir(parents=True, exist_ok=True)
    env["PELICAN_STUDIO_CONFIG_DIR"] = str(studio_config.resolve())

    variables = {
        "project": str(arguments.project.resolve()),
        "artifacts": str(artifacts.resolve()),
    }
    for assignment in arguments.var:
        key, _, value = assignment.partition("=")
        variables[key.strip()] = value

    run = Run(scenario, artifacts)
    epilogue = studio_invariants.Epilogue(
        run, scenario, variables, repository=Path(__file__).resolve().parents[1]
    )
    invariant_outcomes: list[dict[str, Any]] = []
    started_at = time.time()
    started = time.perf_counter()
    studio: Studio | None = None
    launch_error = ""
    watchdog = Watchdog()
    watchdog.start()

    if scenario.get("probe_no_automation"):
        probe_no_automation(run, arguments.studio, pipe, artifacts, env)

    command = [
        str(arguments.studio),
        "--automation",
        pipe,
        "--project",
        str(arguments.project),
    ]
    if scenario.get("tab"):
        command += ["--tab", str(scenario["tab"])]

    try:
        studio = Studio(command, artifacts / "studio.log", env)
        watchdog.on_trip = studio.stop
        if scenario.get("watch_engine_windows"):
            if sys.platform != "win32":
                raise AutomationError("watch_engine_windows is Windows only")
            # From the launch on: the first engine starts with Studio, before
            # the automation channel is even open.
            run.window_watch = EngineWindowWatch(studio.process.pid)
            run.window_watch.start()
        listening = studio.wait_for_line("automation listening:", arguments.launch_timeout)
        if listening is None:
            raise AutomationError(
                "Studio never reported an automation channel "
                f"(see {artifacts / 'studio.log'})"
            )
        run.channel = Channel.connect(
            pipe, time.monotonic() + arguments.launch_timeout, watchdog
        )
        run.record(
            "launch",
            " ".join(command),
            ok=True,
            request={"command": command, "socket": local_socket_path(pipe)},
            detail=listening.strip(),
            started=started,
        )
        viewport_held = fix_viewport(run, scenario)
        # QD-1: the documents as they were when the engine first answered --
        # after the scenario's first wait_engine, or, for a scenario that has
        # none, the files as they are before its first step (waiting for the
        # engine here would change what such a scenario sees first).
        if viewport_held and scenario.get("steps") and not any(
            isinstance(raw, dict) and raw.get("cmd") == "wait_engine"
            for raw in scenario.get("steps", [])
        ):
            epilogue.capture_start(0, need_engine=False)
        steps_ok = viewport_held
        for index, raw_step in enumerate(scenario.get("steps", []), start=1):
            if not viewport_held:
                break
            step = expand(raw_step, variables)
            label = step_label(step, index)
            if "expect" in step:
                ok = run_expect_step(run, step, label)
                # QD-1 (GL-5g's rule 8): which steps judged something.
                reads_only = step["expect"] == "screenshot" or (
                    step["expect"] == "result"
                    and not any(key in step for key in ("value", "value_of", "value_format"))
                )
                if reads_only:
                    run.read_steps += 1
                else:
                    run.compared_steps += 1
            elif "file" in step:
                ok = run_file_step(
                    run, step, label, [arguments.project, artifacts]
                )
            elif "cmd" in step:
                ok = run_command_step(run, step, label)
            else:
                run.record(
                    "step", label, ok=False, request=step, detail="no cmd and no expect"
                )
                ok = False
            epilogue.after_step(step, index, ok)
            if not ok:
                steps_ok = False
                break
        # A scenario of no steps (a launch check) has nothing to undo or keep.
        if steps_ok and scenario.get("steps"):
            invariant_outcomes = studio_invariants.run_invariants(run, epilogue)
    except AutomationError as error:
        launch_error = str(error)
        run.record("launch", " ".join(command), ok=False, detail=launch_error, started=started)

    if run.window_watch is not None:
        run.window_watch.stop()
    exit_code: int | None = None
    forced_shutdown = False
    if studio is not None:
        shutdown_started = time.perf_counter()
        if run.channel is not None:
            try:
                run.channel.send({"cmd": "quit"}, timeout=30.0)
            except AutomationError:
                pass
            run.channel.close()
        exit_code = studio.wait_for_exit(30)
        forced_shutdown = watchdog.tripped is not None
        if exit_code is None:
            forced_shutdown = True
            exit_code = studio.stop()
        if watchdog.tripped is not None:
            shutdown_detail = (
                f"watchdog forced Studio to stop: {watchdog.tripped} never answered "
                f"(exit code {exit_code})"
            )
        elif forced_shutdown:
            shutdown_detail = (
                f"Studio did not exit within 30 seconds; forced shutdown "
                f"(exit code {exit_code})"
            )
        else:
            shutdown_detail = f"Studio exited with code {exit_code}"
        run.record(
            "shutdown",
            "Studio exits cleanly",
            ok=exit_code == 0 and not forced_shutdown,
            response={"exit_code": exit_code, "forced": forced_shutdown},
            detail=shutdown_detail,
            started=shutdown_started,
        )

    finish_warnings(run)
    failed = len(run.failures)
    report = {
        "schema": REPORT_SCHEMA,
        "version": REPORT_VERSION,
        "scenario": name,
        "scenario_path": str(arguments.scenario.resolve()),
        "description": scenario.get("description", ""),
        "project": str(arguments.project.resolve()),
        "studio": str(arguments.studio.resolve()),
        "automation_socket": local_socket_path(pipe),
        "started": time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(started_at)),
        "duration_ms": round((time.perf_counter() - started) * 1000),
        "ok": failed == 0 and not launch_error,
        "verdict": "green" if (failed == 0 and not launch_error) else "red",
        "steps_run": len(run.steps),
        "steps_planned": len(scenario.get("steps", []))
        + (1 if scenario.get("probe_no_automation") else 0)
        + (0 if scenario.get("viewport", True) is None else 1)
        + 2,
        "failed_steps": failed,
        # QD-1 (GL-5g's rule 8): steps that compared something, and readings
        # that only wrote a value down (screenshots, record, remember).
        "compared_steps": run.compared_steps,
        "read_steps": run.read_steps,
        "warnings": run.warnings,
        "summary": run.summary,
        "failures": run.failures,
        "screenshots": run.screenshots,
        "studio_exit_code": exit_code,
        "studio_shutdown_forced": forced_shutdown,
        "studio_log": str((artifacts / "studio.log").resolve()),
        # QD-1: what the driver checked after the last step, by invariant.
        "invariants": invariant_outcomes,
        "known_red": [
            {"invariant": outcome["invariant"], "ticket": outcome.get("ticket")}
            for outcome in invariant_outcomes
            if outcome.get("status") == "known_red"
        ],
        "invariant_timings_s": {key: round(value, 2) for key, value in epilogue.timings.items()},
        "steps": run.steps,
    }
    arguments.report.parent.mkdir(parents=True, exist_ok=True)
    arguments.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    for known in report["known_red"]:
        print(f"{name}: KNOWN RED invariant {known['invariant']} ({known['ticket']})")
    if report["ok"]:
        print(f"{name}: green ({len(run.steps)} steps)")
        return 0
    print(f"{name}: RED", file=sys.stderr)
    for failure in run.failures:
        print(
            f"  step {failure['step']} ({failure['label']}): {failure['reason']}",
            file=sys.stderr,
        )
        if failure.get("screenshot"):
            print(f"    screenshot: {failure['screenshot']}", file=sys.stderr)
    if launch_error:
        print(f"  launch: {launch_error}", file=sys.stderr)
    print(f"  report: {arguments.report.resolve()}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
