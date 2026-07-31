#!/usr/bin/env python3
"""Smoke test for the public thin Pelican RPC client."""

from __future__ import annotations

import argparse
import hashlib
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
    write_json(
        project / "assets/asset_data.json",
        {
            "schema": "pelican.asset_data",
            "version": 1,
            "models": [],
        },
    )
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


def snapshot_source_document() -> dict[str, object]:
    return {
        "schema": "pelican.scene",
        "version": 1,
        "editor_envelope": {"round_trip": ["keep", 7, True]},
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
            },
            "non_current": {
                "objects": [
                    {
                        "components": [
                            {
                                "name": "unknown_read_only",
                                "raw_numeric": [3, 2.5, -0.0, 1e-06],
                                "array_order": ["z", "a", "m"],
                            }
                        ]
                    }
                ]
            },
        },
    }


def scratch_boot_document() -> dict[str, object]:
    return {
        "schema": "pelican.scene",
        "version": 1,
        "scenes": {
            "default_scene": {
                "objects": [
                    {"name": f"ScratchOnly{index}", "components": []}
                    for index in range(5)
                ]
            }
        },
    }


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

        human_project = make_project(Path(temporary) / "snapshot_shared", rendering_config)
        write_json(
            human_project / "scenes/main.scene.json",
            snapshot_source_document(),
        )
        scratch_scene_path = human_project / "scenes/main.scene.json"
        scratch_disk_before = scratch_scene_path.read_bytes()

        human = PelicanRpc(human_project, exe_path=args.player)
        with human:
            human.get_status()
            exported = human.export_scene_snapshot(
                {"schema_version": 1, "allow_pending": False}
            )
            payload = {
                "schema_version": exported["schema_version"],
                "semantic_scene_bytes": exported["semantic_scene_bytes"],
                "digest": exported["digest"],
                "current_scene_id": exported["current_scene_id"],
            }
            human_tree = human.scene_tree()
            scratch = PelicanRpc(human_project, exe_path=args.player)
            with scratch:
                if human._process.pid == scratch._process.pid:
                    raise AssertionError("snapshot E2E did not start distinct processes")

                # Consume a different scratch-local identity range before the
                # real transfer. Both processes still share the exact same
                # project root and disk scene read-only.
                padding_bytes = json.dumps(
                    scratch_boot_document(),
                    sort_keys=True,
                    separators=(",", ":"),
                )
                padding_payload = {
                    "schema_version": 1,
                    "semantic_scene_bytes": padding_bytes,
                    "digest": {
                        "algorithm": "sha256",
                        "hex": hashlib.sha256(padding_bytes.encode("utf-8")).hexdigest(),
                    },
                    "current_scene_id": "default_scene",
                }
                scratch.import_scene_snapshot(padding_payload)

                imported = scratch.import_scene_snapshot(payload)
                if imported.get("status") != "imported":
                    raise AssertionError(f"unexpected import result: {imported!r}")
                if imported.get("current_scene_id") != exported["current_scene_id"]:
                    raise AssertionError(f"imported wrong scene: {imported!r}")

                scratch_tree = scratch.scene_tree()
                if scratch_tree.get("scene_id") != "default_scene":
                    raise AssertionError(f"unexpected imported tree: {scratch_tree!r}")
                source_id = human_tree["objects"][0]["authoring_object_id"]
                imported_id = scratch_tree["objects"][0]["authoring_object_id"]
                if source_id == imported_id or imported_id <= 5:
                    raise AssertionError(
                        "snapshot import transplanted source AuthoringObjectId: "
                        f"source={source_id}, imported={imported_id}"
                    )

                round_tripped = scratch.export_scene_snapshot(
                    {"schema_version": 1, "allow_pending": False}
                )
                for field in ("current_scene_id", "semantic_scene_bytes", "digest"):
                    if round_tripped[field] != exported[field]:
                        raise AssertionError(
                            f"snapshot round trip changed {field}: "
                            f"{round_tripped[field]!r} != {exported[field]!r}"
                        )
                stepped = scratch.step_frame()
                if not isinstance(stepped.get("frame"), int):
                    raise AssertionError(f"imported runtime did not step: {stepped!r}")

        if scratch_scene_path.read_bytes() != scratch_disk_before:
            raise AssertionError("snapshot import overwrote the scratch scene file")
        if human._process.poll() is None or scratch._process.poll() is None:
            raise AssertionError("snapshot E2E left a player process running")
        print(
            "snapshot e2e: "
            f"human_pid={human._process.pid} scratch_pid={scratch._process.pid} "
            f"source_revision={exported['scene_revision']} "
            f"imported_revision={imported['scene_revision']} "
            f"source_id={source_id} imported_id={imported_id} "
            f"bytes={len(exported['semantic_scene_bytes'].encode('utf-8'))} "
            f"digest={exported['digest']['hex']} disk_unchanged=true"
        )

    print("pelican_rpc smoke passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
