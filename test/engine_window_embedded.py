#!/usr/bin/env python3
"""EW-1: an --embedded player creates its render window hidden and marked.

A host (Studio) embeds the engine's window only after the project has loaded.
With --embedded the window must not appear outside the host in the meantime:
it is created hidden, unfocused and never fullscreen, and it carries the
PelicanEngineRenderWindow property the host finds it by. Without --embedded
the window is shown as before and carries the same mark.
"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time

# Keep in step with src/core/os/enginewindowproperty.hpp.
ENGINE_RENDER_WINDOW_PROPERTY = "PelicanEngineRenderWindow"
WINDOW_WIDTH = 320
WINDOW_HEIGHT = 200

GW_OWNER = 4
SW_SHOWNOACTIVATE = 4

user32 = ctypes.WinDLL("user32", use_last_error=True)
EnumWindowsProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user32.EnumWindows.argtypes = [EnumWindowsProc, wintypes.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetWindowThreadProcessId.restype = wintypes.DWORD
user32.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
user32.GetWindow.restype = wintypes.HWND
user32.GetPropW.argtypes = [wintypes.HWND, wintypes.LPCWSTR]
user32.GetPropW.restype = wintypes.HANDLE
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]


def use_physical_pixels() -> None:
    # Read the player's client rectangle in physical pixels, the unit GLFW
    # created it in, rather than in a DPI-virtualized unit.
    try:
        user32.SetProcessDpiAwarenessContext.argtypes = [wintypes.HANDLE]
        user32.SetProcessDpiAwarenessContext(wintypes.HANDLE(-4))
    except AttributeError:
        pass


def top_level_windows(pid: int) -> list[int]:
    """The owner-less top-level windows of process ``pid``."""
    found: list[int] = []

    def visit(hwnd: int, _: int) -> bool:
        owner_pid = wintypes.DWORD(0)
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner_pid))
        if owner_pid.value == pid and not user32.GetWindow(hwnd, GW_OWNER):
            found.append(hwnd)
        return True

    user32.EnumWindows(EnumWindowsProc(visit), 0)
    return found


def marked(hwnd: int) -> bool:
    return bool(user32.GetPropW(hwnd, ENGINE_RENDER_WINDOW_PROPERTY))


def visible(hwnd: int) -> bool:
    return bool(user32.IsWindowVisible(hwnd))


def client_extent(hwnd: int) -> tuple[int, int]:
    rect = wintypes.RECT()
    if not user32.GetClientRect(hwnd, ctypes.byref(rect)):
        raise AssertionError(f"GetClientRect failed: {ctypes.get_last_error()}")
    return rect.right - rect.left, rect.bottom - rect.top


def make_window_project(root: Path, rendering_config: Path, fullscreen: bool) -> Path:
    from pelican_rpc_smoke import make_project

    project = make_project(root, rendering_config)
    project_json = project / "project.json"
    document = json.loads(project_json.read_text(encoding="utf-8"))
    document["name"] = "engine_window_embedded"
    document["basic_config"]["window_size"] = {"width": WINDOW_WIDTH, "height": WINDOW_HEIGHT}
    document["basic_config"]["fullscreen"] = fullscreen
    project_json.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    return project


def frames_advance(client: object, label: str) -> tuple[int, int]:
    before = client.get_status()["frame"]
    deadline = time.monotonic() + 20.0
    while time.monotonic() < deadline:
        time.sleep(0.2)
        after = client.get_status()["frame"]
        if after > before:
            return before, after
    raise AssertionError(f"{label}: the frame did not advance from {before}")


def the_marked_window(pid: int, label: str) -> int:
    windows = top_level_windows(pid)
    marks = [hwnd for hwnd in windows if marked(hwnd)]
    if len(marks) != 1:
        raise AssertionError(
            f"{label}: expected exactly one marked top-level window, found {len(marks)} "
            f"among {len(windows)} owner-less top-level windows"
        )
    return marks[0]


def check_embedded(player: Path, project: Path, log_file: Path) -> None:
    from tools.pelican_rpc import PelicanRpc

    client = PelicanRpc(
        project,
        exe_path=player,
        headless=False,
        extra_arguments=["--embedded", "--log-file", str(log_file)],
    )
    with client:
        # The first answer comes after the project has loaded.
        client.get_status()
        pid = client._process.pid
        hwnd = the_marked_window(pid, "--embedded")
        if visible(hwnd):
            raise AssertionError("--embedded: the render window is visible before the host shows it")
        shown = [window for window in top_level_windows(pid) if visible(window)]
        if shown:
            raise AssertionError(f"--embedded: {len(shown)} top-level window(s) of the player are visible")
        # The project asks for fullscreen; an embedded window is created at
        # the project's window size instead.
        extent = client_extent(hwnd)
        if extent != (WINDOW_WIDTH, WINDOW_HEIGHT):
            raise AssertionError(
                f"--embedded: client extent {extent} != project window size "
                f"{(WINDOW_WIDTH, WINDOW_HEIGHT)}"
            )
        hidden_frames = frames_advance(client, "--embedded hidden")

        # What the host does once it has embedded the window: show it.
        user32.ShowWindow(hwnd, SW_SHOWNOACTIVATE)
        if not visible(hwnd):
            raise AssertionError("--embedded: ShowWindow did not show the render window")
        shown_frames = frames_advance(client, "--embedded shown")
        if the_marked_window(pid, "--embedded shown") != hwnd:
            raise AssertionError("--embedded: the marked window changed after ShowWindow")
    if client._process.poll() is None:
        raise AssertionError("--embedded: the player is still running")

    log = log_file.read_text(encoding="utf-8", errors="replace")
    if "--embedded: the project's fullscreen setting is ignored" not in log:
        raise AssertionError("--embedded: the log does not say the fullscreen setting was ignored")
    print(
        f"embedded: hidden frames {hidden_frames[0]}->{hidden_frames[1]}, "
        f"shown frames {shown_frames[0]}->{shown_frames[1]}, client {extent}"
    )


def check_standalone(player: Path, project: Path) -> None:
    from tools.pelican_rpc import PelicanRpc

    client = PelicanRpc(project, exe_path=player, headless=False)
    with client:
        client.get_status()
        hwnd = the_marked_window(client._process.pid, "standalone")
        if not visible(hwnd):
            raise AssertionError("standalone: the render window is not visible")
        extent = client_extent(hwnd)
        if extent != (WINDOW_WIDTH, WINDOW_HEIGHT):
            raise AssertionError(f"standalone: client extent {extent}")
        frames = frames_advance(client, "standalone")
    print(f"standalone: visible and marked, frames {frames[0]}->{frames[1]}")


def check_headless_refused(player: Path, project: Path) -> None:
    completed = subprocess.run(
        [str(player), "--embedded", "--headless", "--project", str(project)],
        cwd=player.parent,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=120,
    )
    if completed.returncode == 0:
        raise AssertionError("--embedded --headless was accepted")
    if "--embedded cannot be combined with --headless" not in completed.stderr:
        raise AssertionError(f"--embedded --headless: unexpected stderr: {completed.stderr!r}")
    print(f"--embedded --headless refused with exit code {completed.returncode}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--player", required=True, type=Path)
    args = parser.parse_args()

    if sys.platform != "win32":
        print("engine_window_embedded: Windows only")
        return 0

    repo_root = args.repo_root.resolve()
    player = args.player.resolve()
    sys.path.insert(0, str(repo_root))
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    use_physical_pixels()

    rendering_config = repo_root / "projects/example/passes/main_rendering_config.json"
    with tempfile.TemporaryDirectory(prefix="pelican_engine_window_") as temporary:
        root = Path(temporary)
        fullscreen_project = make_window_project(root / "fullscreen", rendering_config, True)
        windowed_project = make_window_project(root / "windowed", rendering_config, False)

        check_headless_refused(player, windowed_project)
        check_embedded(player, fullscreen_project, root / "embedded_player.log")
        check_standalone(player, windowed_project)

    print("engine_window_embedded passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
