"""WP385 runtime acceptance for the GPU lease launcher.

These cases drive the real ``pelican_gpu_lease_run`` executable, but on a
test-scoped lease name (``PELICAN_GPU_LEASE_NAME``) so they never contend for
the one real GPU lease and therefore need no GPU themselves.

What is observed, and what has to be removed for each case to go red:

* ``mutual_exclusion`` -- two launchers started together hold the lease over
  disjoint monotonic intervals (read from the launcher's own acquire/release
  log).  Remove the mutex wait in ``gpu_lease_run.cpp`` and the intervals
  overlap.  The ``PELICAN_GPU_LEASE=off`` control shows the same two children
  overlapping, so a launcher that simply serialised by accident cannot pass.
* ``timeout`` -- a waiter blocked behind a holder fails with
  ``gpu_lease_timeout@1{holder_pid=...,waited_seconds=...}`` and exit 124, and
  its wrapped command never runs.  Turn the timeout into a skip (exit 0/4) or
  drop the deadline and this goes red.
* ``job_kill`` -- a grandchild started by the wrapped command dies when the
  launcher is killed.  Remove ``JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`` (or the
  job entirely) and the grandchild survives.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path

HOLD_SECONDS = 2.0
TIMEOUT_SECONDS = 5
HOLDER_HOLD_SECONDS = 12.0
GRANDCHILD_SECONDS = 120.0


# --------------------------------------------------------------------------
# helper modes (this file re-executes itself as the wrapped command)
# --------------------------------------------------------------------------
def _mode_child(args: argparse.Namespace) -> int:
    began = time.monotonic()
    time.sleep(args.seconds)
    Path(args.out).write_text(f"{began} {time.monotonic()}\n", encoding="utf-8")
    return 0


def _mode_parent(args: argparse.Namespace) -> int:
    creation = 0
    if os.name == "nt":
        creation = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
    subprocess.Popen(
        [sys.executable, "-B", str(Path(__file__).resolve()), "--mode", "grandchild",
         "--out", args.out, "--seconds", str(GRANDCHILD_SECONDS)],
        creationflags=creation, start_new_session=os.name != "nt")
    time.sleep(GRANDCHILD_SECONDS)
    return 0


def _mode_grandchild(args: argparse.Namespace) -> int:
    Path(args.out).write_text(str(os.getpid()), encoding="utf-8")
    time.sleep(args.seconds)
    return 0


# --------------------------------------------------------------------------
# observation helpers
# --------------------------------------------------------------------------
def _lease_intervals(log_dir: Path, lease: str) -> list[tuple[int, int]]:
    intervals: list[tuple[int, int]] = []
    for path in sorted(log_dir.glob(f"{lease}.*.log")):
        acquired: int | None = None
        for line in path.read_text(encoding="utf-8").splitlines():
            fields = line.split()
            if len(fields) != 3:
                continue
            _, event, stamp = fields
            if event == "acquired":
                acquired = int(stamp)
            elif event == "released" and acquired is not None:
                intervals.append((acquired, int(stamp)))
                acquired = None
    return sorted(intervals)


def _overlapping(intervals: list[tuple[float, float]]) -> list[str]:
    problems = []
    ordered = sorted(intervals)
    for left, right in zip(ordered, ordered[1:]):
        if right[0] < left[1]:
            problems.append(f"{left} overlaps {right}")
    return problems


def _child_intervals(paths: list[Path]) -> list[tuple[float, float]]:
    spans = []
    for path in paths:
        begin, end = path.read_text(encoding="utf-8").split()
        spans.append((float(begin), float(end)))
    return spans


def _alive(pid: int) -> bool:
    if os.name == "nt":
        import ctypes

        handle = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)
        if not handle:
            return False
        code = ctypes.c_ulong()
        ctypes.windll.kernel32.GetExitCodeProcess(handle, ctypes.byref(code))
        ctypes.windll.kernel32.CloseHandle(handle)
        return code.value == 259  # STILL_ACTIVE
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def _environment(log_dir: Path, lease: str, **extra: str) -> dict[str, str]:
    env = dict(os.environ)
    env["PELICAN_GPU_LEASE_NAME"] = lease
    env["PELICAN_GPU_LEASE_LOG_DIR"] = str(log_dir)
    env.pop("PELICAN_GPU_LEASE", None)
    env.update(extra)
    return env


def _self() -> list[str]:
    return [sys.executable, "-B", str(Path(__file__).resolve())]


# --------------------------------------------------------------------------
# cases
# --------------------------------------------------------------------------
def case_mutual_exclusion(launcher: str, root: Path, failures: list[str]) -> None:
    for lease_mode in ("on", "off"):
        work = root / f"exclusion_{lease_mode}"
        work.mkdir()
        lease = f"pelican_gpu_lease_test_{uuid.uuid4().hex}"
        extra = {"PELICAN_GPU_LEASE": "off"} if lease_mode == "off" else {}
        env = _environment(work, lease, **extra)
        outputs = [work / "a.txt", work / "b.txt"]
        running = [
            subprocess.Popen(
                [launcher, "--", *_self(), "--mode", "child",
                 "--out", str(out), "--seconds", str(HOLD_SECONDS)],
                env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            for out in outputs
        ]
        for process in running:
            process.wait(timeout=120)
            if process.returncode != 0:
                failures.append(
                    f"mutual_exclusion[{lease_mode}]: launcher exit "
                    f"{process.returncode}: {process.stderr.read()}")
        if any(not out.exists() for out in outputs):
            failures.append(f"mutual_exclusion[{lease_mode}]: a wrapped command did not run")
            continue
        children = _child_intervals(outputs)
        overlaps = _overlapping(children)
        if lease_mode == "on":
            if overlaps:
                failures.append(f"mutual_exclusion: wrapped commands overlapped: {overlaps}")
            leases = _lease_intervals(work, lease)
            if len(leases) != 2:
                failures.append(
                    f"mutual_exclusion: expected 2 logged lease intervals, got {leases}")
            elif _overlapping([(float(a), float(b)) for a, b in leases]):
                failures.append(f"mutual_exclusion: lease intervals overlapped: {leases}")
        elif not overlaps:
            failures.append(
                "mutual_exclusion[off]: the control did not overlap, so the "
                f"serialisation above proves nothing: {children}")


def case_timeout(launcher: str, root: Path, failures: list[str]) -> None:
    work = root / "timeout"
    work.mkdir()
    lease = f"pelican_gpu_lease_test_{uuid.uuid4().hex}"
    env = _environment(work, lease)
    holder = subprocess.Popen(
        [launcher, "--", *_self(), "--mode", "child",
         "--out", str(work / "holder.txt"), "--seconds", str(HOLDER_HOLD_SECONDS)],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if any("acquired" in path.read_text(encoding="utf-8")
                   for path in work.glob(f"{lease}.*.log")):
                break
            time.sleep(0.1)
        else:
            failures.append("timeout: the holder never took the lease")
        marker = work / "waiter.txt"
        waiter = subprocess.run(
            [launcher, "--", *_self(), "--mode", "child",
             "--out", str(marker), "--seconds", "0"],
            env=_environment(work, lease,
                             PELICAN_GPU_LEASE_TIMEOUT_SECONDS=str(TIMEOUT_SECONDS)),
            capture_output=True, text=True, timeout=120)
    finally:
        holder.wait(timeout=120)
    if waiter.returncode != 124:
        failures.append(
            f"timeout: waiter must fail with 124, got {waiter.returncode} "
            f"(stdout={waiter.stdout!r} stderr={waiter.stderr!r})")
    expected = f"gpu_lease_timeout@1{{holder_pid={holder.pid},waited_seconds={TIMEOUT_SECONDS}}}"
    if expected not in waiter.stderr:
        failures.append(f"timeout: expected {expected!r}, got {waiter.stderr!r}")
    if marker.exists():
        failures.append("timeout: the wrapped command ran even though the lease was never taken")


def case_job_kill(launcher: str, root: Path, failures: list[str]) -> None:
    work = root / "job"
    work.mkdir()
    lease = f"pelican_gpu_lease_test_{uuid.uuid4().hex}"
    pid_file = work / "grandchild.pid"
    process = subprocess.Popen(
        [launcher, "--", *_self(), "--mode", "parent", "--out", str(pid_file)],
        env=_environment(work, lease))
    deadline = time.monotonic() + 60
    while not pid_file.exists() and time.monotonic() < deadline:
        time.sleep(0.1)
    if not pid_file.exists():
        process.kill()
        failures.append("job_kill: the grandchild never reported its pid")
        return
    grandchild = int(pid_file.read_text(encoding="utf-8").strip())
    if not _alive(grandchild):
        failures.append(f"job_kill: grandchild {grandchild} was not running before the kill")
    process.kill()  # stands in for CTest terminating the launcher on timeout
    process.wait(timeout=60)
    deadline = time.monotonic() + 20
    while _alive(grandchild) and time.monotonic() < deadline:
        time.sleep(0.2)
    if _alive(grandchild):
        failures.append(
            f"job_kill: grandchild {grandchild} outlived the killed launcher; the "
            "lease would stay held by an orphan")


CASES = {
    "mutual_exclusion": case_mutual_exclusion,
    "timeout": case_timeout,
    "job_kill": case_job_kill,
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--launcher")
    parser.add_argument("--case", action="append", choices=sorted(CASES))
    parser.add_argument("--mode", choices=["child", "parent", "grandchild"])
    parser.add_argument("--out")
    parser.add_argument("--seconds", type=float, default=0.0)
    args = parser.parse_args()
    if args.mode:
        return {"child": _mode_child, "parent": _mode_parent,
                "grandchild": _mode_grandchild}[args.mode](args)
    if not args.launcher:
        parser.error("--launcher is required")
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="pelican_gpu_lease_") as directory:
        root = Path(directory)
        for name in args.case or sorted(CASES):
            CASES[name](args.launcher, root, failures)
    if failures:
        print("WP385 gpu lease runtime acceptance failed:", *failures, sep="\n", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
