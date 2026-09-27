#!/usr/bin/env python3
"""PM-1a: playing <-> paused and advance_frames in a windowed engine.

A windowed engine draws every frame itself. Paused, it goes on drawing (the
evaluation index `frame` keeps rising) while the game frame and the game clock
stay where they stopped; advance_frames answers at once and the steps are taken
one per frame from the next frame boundary; step_frame, which would run a
nested frame here, is refused as step_frame_windowed.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import tempfile
import time
from typing import Any, Callable

STEP_SECONDS = 1.0 / 60.0  # the default --fps


def make_window_project(root: Path, rendering_config: Path) -> Path:
    from pelican_rpc_smoke import make_project

    project = make_project(root, rendering_config)
    project_json = project / "project.json"
    document = json.loads(project_json.read_text(encoding="utf-8"))
    document["name"] = "pm1a_play_state_windowed"
    document["basic_config"]["window_size"] = {"width": 320, "height": 200}
    document["basic_config"]["fullscreen"] = False
    project_json.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    return project


def wait_for(client: Any, label: str, predicate: Callable[[dict], bool]) -> dict:
    deadline = time.monotonic() + 20.0
    status = client.get_status()
    while time.monotonic() < deadline:
        if predicate(status):
            return status
        time.sleep(0.05)
        status = client.get_status()
    raise AssertionError(f"{label}: not reached; last status play={status.get('play')!r} frame={status.get('frame')!r}")


def refused(client: Any, method: str, params: dict, code: str) -> dict:
    from tools.pelican_rpc import PelicanRpcError

    try:
        answer = client.call(method, params)
    except PelicanRpcError as error:
        data = error.data if isinstance(error.data, dict) else {}
        if data.get("code") != code:
            raise AssertionError(f"{method}: expected {code}, got {error.code} {error.message!r} {error.data!r}")
        return data
    raise AssertionError(f"{method} {params!r} was accepted: {answer!r}")


def check(player: Path, project: Path) -> None:
    from tools.pelican_rpc import PelicanRpc

    client = PelicanRpc(project, exe_path=player, headless=False, extra_arguments=["--embedded"])
    with client:
        start = client.get_status()
        if start["play"]["state"] != "playing":
            raise AssertionError(f"the default launch is not playing: {start['play']!r}")

        # A windowed engine refuses step_frame by name, and advance_frames
        # while playing.
        refused(client, "step_frame", {}, "step_frame_windowed")
        refused(client, "advance_frames", {"count": 1}, "play_state_mismatch")
        edit = refused(client, "set_play_state", {"state": "edit"}, "play_state_mismatch")
        if edit.get("requested") != "edit":
            raise AssertionError(f"set_play_state edit: {edit!r}")

        answer = client.call("set_play_state", {"state": "paused"})
        if answer != {"state": "paused", "previous": "playing"}:
            raise AssertionError(f"set_play_state paused: {answer!r}")
        # The pause lands at the next frame boundary; from then on the game
        # stays put while the frames go on.
        stopped = wait_for(client, "paused", lambda s: s["play"]["state"] == "paused")
        stopped = wait_for(client, "a paused frame", lambda s: s["frame"] > stopped["frame"] + 1)
        held = wait_for(client, "frames go on while paused", lambda s: s["frame"] >= stopped["frame"] + 10)
        if held["play"]["game_frame"] != stopped["play"]["game_frame"] or \
                held["play"]["game_time"] != stopped["play"]["game_time"]:
            raise AssertionError(f"the game moved while paused: {stopped['play']!r} -> {held['play']!r}")

        scheduled = client.call("advance_frames", {"count": 3})
        until = scheduled.get("until_game_frame")
        if scheduled.get("scheduled") != 3 or until != held["play"]["game_frame"] + 3:
            raise AssertionError(f"advance_frames: {scheduled!r} after {held['play']!r}")
        stepped = wait_for(client, "three steps",
                           lambda s: s["play"]["game_frame"] == until and s["play"]["pending_steps"] == 0)
        later = wait_for(client, "frames after the steps", lambda s: s["frame"] >= stepped["frame"] + 10)
        if later["play"]["game_frame"] != until or later["play"]["state"] != "paused":
            raise AssertionError(f"the game went past the steps: {later['play']!r}")
        moved = later["play"]["game_time"] - held["play"]["game_time"]
        if abs(moved - 3 * STEP_SECONDS) > 1e-9:
            raise AssertionError(f"three steps moved the game clock by {moved}, not {3 * STEP_SECONDS}")

        answer = client.call("set_play_state", {"state": "playing"})
        if answer != {"state": "playing", "previous": "paused"}:
            raise AssertionError(f"set_play_state playing: {answer!r}")
        wait_for(client, "playing again", lambda s: s["play"]["game_frame"] > until + 5)
    print(f"pm1a windowed: paused {stopped['play']['game_frame']} over "
          f"{held['frame'] - stopped['frame']} frames, stepped to {until}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--player", required=True, type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    player = args.player.resolve()
    sys.path.insert(0, str(repo_root))
    sys.path.insert(0, str(Path(__file__).resolve().parent))

    rendering_config = repo_root / "projects/example/passes/main_rendering_config.json"
    with tempfile.TemporaryDirectory(prefix="pelican_pm1a_windowed_") as temporary:
        project = make_window_project(Path(temporary), rendering_config)
        check(player, project)
    print("pm1a_play_state_windowed passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
