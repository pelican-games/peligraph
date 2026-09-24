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
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any

REPORT_SCHEMA = "pelican.studio_scenario_report"
REPORT_VERSION = 1


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
    if isinstance(left, bool) or isinstance(right, bool):
        return left is right or left == right
    if isinstance(left, (int, float)) and isinstance(right, (int, float)):
        return abs(float(left) - float(right)) <= 1e-9
    return left == right


_MISSING = object()


def compare(op: str, actual: Any, expected: Any) -> bool:
    """The comparisons a `result` step can ask for.

    `eq` is the one every other expectation uses.  The rest exist because a
    gesture is judged by which way a number moved, not by where it landed.
    """

    if op == "eq":
        return equal(actual, expected)
    if op == "ne":
        return not equal(actual, expected)
    if op == "contains":
        return expected in actual
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
    ) -> dict[str, Any]:
        index = len(self.steps) + 1
        entry = {
            "index": index,
            "kind": kind,
            "label": label,
            "ok": ok,
            "duration_ms": round((time.monotonic() - started) * 1000) if started else 0,
            "request": request,
            "response": response,
            "detail": detail,
        }
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


def run_command_step(run: Run, step: dict[str, Any], label: str) -> bool:
    started = time.monotonic()
    request = command_of(step)
    expected_error = step.get("expect_error")
    try:
        reply = run.call(request, timeout=float(step.get("wait_s", 180)))
    except AutomationError as error:
        run.record(
            "cmd", label, ok=False, request=request, detail=str(error), started=started
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
        run.record(
            "cmd",
            label,
            ok=False,
            request=request,
            response=reply,
            detail=f"{error.get('code', 'error')}: {error.get('message', '')}",
            started=started,
            screenshot=run.capture(f"step{len(run.steps) + 1}_failure"),
        )
        return False
    if request.get("cmd") == "screenshot":
        run.screenshots.append(reply["result"]["path"])
    run.record("cmd", label, ok=True, request=request, response=reply, started=started)
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
    """

    started = time.monotonic()
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
            detail = f"{path.name} {'holds' if found else 'does not hold'} the text"
        elif action == "exists":
            present = path.is_file()
            if present != bool(step.get("value", True)):
                raise ValueError(f"{path.name} {'exists' if present else 'is missing'}")
            detail = f"{path.name} {'exists' if present else 'is missing'}"
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
    started = time.monotonic()
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
            path = str(step.get("path", ""))
            try:
                actual = follow(reply["result"], path)
            except (PathError, ValueError) as error:
                return _fail(run, kind, label, request, reply, f"{path}: {error}", started)
            if step.get("remember"):
                run.remembered[str(step["remember"])] = actual
            has_expectation = "value" in step or "value_of" in step
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
            if "value" in step:
                expected = step["value"]
                shown = json.dumps(expected)
            else:
                expected = run.remembered.get(str(step["value_of"]), _MISSING)
                if expected is _MISSING:
                    return _fail(
                        run,
                        kind,
                        label,
                        request,
                        reply,
                        f"no earlier step remembered \"{step['value_of']}\"",
                        started,
                    )
                shown = f"{step['value_of']} ({json.dumps(expected)})"
            op = str(step.get("op", "eq"))
            try:
                satisfied = compare(op, actual, expected)
            except (ValueError, TypeError) as error:
                return _fail(run, kind, label, request, reply, str(error), started)
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
            run.record(
                "expect",
                label,
                ok=True,
                request=request,
                response={"path": path, "value": actual},
                detail=f"{request['cmd']} {path} {op} {shown} ({json.dumps(actual)})",
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


def probe_no_automation(
    run: Run, studio: Path, pipe: str, artifacts: Path, env: dict[str, str]
) -> bool:
    """Acceptance 1, first half: without the flag there is no channel at all."""

    started = time.monotonic()
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

    variables = {
        "project": str(arguments.project.resolve()),
        "artifacts": str(artifacts.resolve()),
    }
    for assignment in arguments.var:
        key, _, value = assignment.partition("=")
        variables[key.strip()] = value

    run = Run(scenario, artifacts)
    started_at = time.time()
    started = time.monotonic()
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
        for index, raw_step in enumerate(scenario.get("steps", []), start=1):
            step = expand(raw_step, variables)
            label = step_label(step, index)
            if "expect" in step:
                ok = run_expect_step(run, step, label)
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
            if not ok:
                break
    except AutomationError as error:
        launch_error = str(error)
        run.record("launch", " ".join(command), ok=False, detail=launch_error, started=started)

    exit_code: int | None = None
    forced_shutdown = False
    if studio is not None:
        shutdown_started = time.monotonic()
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
        "duration_ms": round((time.monotonic() - started) * 1000),
        "ok": failed == 0 and not launch_error,
        "verdict": "green" if (failed == 0 and not launch_error) else "red",
        "steps_run": len(run.steps),
        "steps_planned": len(scenario.get("steps", [])) + (1 if scenario.get("probe_no_automation") else 0) + 2,
        "failed_steps": failed,
        "summary": run.summary,
        "failures": run.failures,
        "screenshots": run.screenshots,
        "studio_exit_code": exit_code,
        "studio_shutdown_forced": forced_shutdown,
        "studio_log": str((artifacts / "studio.log").resolve()),
        "steps": run.steps,
    }
    arguments.report.parent.mkdir(parents=True, exist_ok=True)
    arguments.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

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
