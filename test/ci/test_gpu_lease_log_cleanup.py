#!/usr/bin/env python3
"""FX-6: startup log retention, with the real GPU lease explicitly disabled."""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--launcher", required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()
    args.work_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="lease_logs_", dir=args.work_dir) as directory:
        root = Path(directory)
        now = time.time()

        def place(name: str, age_hours: float) -> Path:
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture\n", encoding="utf-8")
            stamp = now - age_hours * 3600
            os.utime(path, (stamp, stamp))
            return path

        old = place("pelican_gpu_lease_v1.old.log", 48)
        retained = [
            place("pelican_gpu_lease_v1.fresh.log", 23),
            place("pelican_gpu_lease_v1.future.log", -24),
            place("pelican_gpu_lease_test.old.log", 48),
            place("pelican_gpu_lease_v1.old.txt", 48),
            place("nested/pelican_gpu_lease_v1.old.log", 48),
        ]
        locked = place("pelican_gpu_lease_v1.locked.log", 48) if os.name == "nt" else None
        handle = None
        if locked:
            import ctypes
            from ctypes import wintypes
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                          wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD,
                                          wintypes.HANDLE]
            kernel.CreateFileW.restype = wintypes.HANDLE
            kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            # Read/write sharing is allowed, deletion sharing is denied.
            handle = kernel.CreateFileW(str(locked), 0x80000000, 3, None, 3, 0x80, None)
            assert handle != wintypes.HANDLE(-1).value, ctypes.get_last_error()
        directory_log = root / "pelican_gpu_lease_v1.directory.log"
        directory_log.mkdir()
        os.utime(directory_log, (now - 48 * 3600, now - 48 * 3600))
        env = dict(os.environ)
        # Exercise the normal TEMP fallback, without touching the machine's
        # real log directory, shader cache, or GPU mutex.
        env.pop("PELICAN_GPU_LEASE_LOG_DIR", None)
        env["TEMP" if os.name == "nt" else "XDG_RUNTIME_DIR"] = str(root)
        env["PELICAN_GPU_LEASE"] = "off"
        env["PELICAN_SHADER_CACHE_DIR"] = str(root / "cache")
        try:
            result = subprocess.run(
                [args.launcher, "--", sys.executable, "-B", "-c", "print('wrapped command ran')"],
                env=env, capture_output=True, text=True, timeout=30)
            if locked:
                assert locked.exists(), "an open log denying deletion was removed"
        finally:
            if handle is not None:
                kernel.CloseHandle(handle)
        assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
        assert "wrapped command ran" in result.stdout, result.stdout
        assert result.stderr == "", f"cleanup failure was not silent: {result.stderr}"
        assert not old.exists(), "a log older than 24 hours survived startup"
        assert directory_log.is_dir(), "a matching directory was removed"
        for path in retained:
            assert path.read_text(encoding="utf-8") == "fixture\n", f"retained file changed: {path}"
        # The explicit log directory uses the same retention rule.
        chosen = root / "chosen"
        chosen.mkdir()
        chosen_old = chosen / "pelican_gpu_lease_v1.old.log"
        chosen_old.write_text("old\n", encoding="utf-8")
        os.utime(chosen_old, (now - 48 * 3600, now - 48 * 3600))
        env["PELICAN_GPU_LEASE_LOG_DIR"] = str(chosen)
        result = subprocess.run(
            [args.launcher, "--", sys.executable, "-B", "-c", "pass"],
            env=env, capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, (result.returncode, result.stderr)
        assert result.stderr == "", result.stderr
        assert not chosen_old.exists(), "explicit log directory was not swept"
    print("FX-6 GPU lease log cleanup: PASS (lease off)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
