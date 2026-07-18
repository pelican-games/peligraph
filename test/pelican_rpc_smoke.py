#!/usr/bin/env python3
"""Smoke test for the public thin Pelican RPC client."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import sys
import tempfile


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def make_project(root: Path, rendering_config: Path) -> Path:
    project = root / "project"
    write_json(
        project / "project.json",
        {
            "schema": "pelican.project",
            "version": 1,
            "name": "pelican_rpc_smoke",
            "engine_min_version": "0.1.0",
            "basic_config": {
                "window_title": "Pelican RPC Smoke",
                "window_size": {"width": 160, "height": 90},
                "fullscreen": False,
                "framerate": 30,
                "seed": 1234,
                "camera": {
                    "yfov": 0.7853981633974483,
                    "znear": 0.1,
                    "zfar": 1000.0,
                    "up": [0.0, 1.0, 0.0],
                },
                "default_scene_id": "default_scene",
                "scene_data_json": "scenes/main.scene.json",
                "asset_data_json": "assets/asset_data.json",
                "rendering_config_json": "passes/main_rendering_config.json",
                "default_rendering_pass": "main_render",
                "ui_config_json": "ui/ui_overlay.json",
            },
        },
    )
    write_json(
        project / "scenes/main.scene.json",
        {
            "schema": "pelican.scene",
            "version": 1,
            "scenes": {
                "default_scene": {
                    "objects": [
                        {
                            "name": "KeyLight",
                            "components": [
                                {
                                    "name": "light",
                                    "type": "directional",
                                    "direction": [-1.0, -0.25, 0.0],
                                    "intensity": 5.0,
                                    "color": [1.0, 1.0, 1.0],
                                }
                            ],
                        }
                    ]
                }
            },
        },
    )
    write_json(project / "assets/asset_data.json", {"models": []})
    write_json(
        project / "ui/ui_overlay.json",
        {
            "schema": "pelican.ui",
            "version": 1,
            "key": "empty",
            "root": {"id": "root", "type": "panel"},
        },
    )
    pass_path = project / "passes/main_rendering_config.json"
    pass_path.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(rendering_config, pass_path)
    return project


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--player", required=True, type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    sys.path.insert(0, str(repo_root))
    from tools.pelican_rpc import PelicanRpc

    rendering_config = repo_root / "projects/example/passes/main_rendering_config.json"
    with tempfile.TemporaryDirectory(prefix="pelican_rpc_smoke_") as temporary:
        project = make_project(Path(temporary), rendering_config)
        client = PelicanRpc(project, exe_path=args.player)
        with client:
            status = client.get_status()
            if status.get("scene") != "default_scene":
                raise AssertionError(f"unexpected get_status result: {status!r}")

            stepped = client.step_frame()
            if stepped.get("frame") != status.get("frame") + 1:
                raise AssertionError(f"unexpected step_frame result: {stepped!r}")

            tree = client.scene_tree()
            if tree.get("scene_id") != "default_scene":
                raise AssertionError(f"unexpected scene_tree result: {tree!r}")

        if client._process.poll() is None:
            raise AssertionError("PelicanRpc context manager left the player running")

    print("pelican_rpc smoke passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
