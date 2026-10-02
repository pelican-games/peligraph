#!/usr/bin/env python3
"""CV-1b: a pelican.canvas document edited over RPC in a headless engine.

The blank project's root holds a canvas (a filled rectangle in a stack, read
by present as "out") that nothing lowered beforehand. The engine lowers it
where it reads it:

* edit_graph_document changes the rectangle's size; the edit commits and the
  next frame is a different picture. The CAS digest before and after is the
  digest of the authored bytes on disk, which still hold the canvas nodes.
* an edit the lowering refuses by name (a layer blend other than normal) is
  rejected with canvas.blend_unsupported@1, the document keeps its bytes and
  the picture does not change.
* the frame plan's pass "out" names the author's node "out" as its origin
  (GL-2a), and get_authoring_lowering reports the output's items, the nodes
  that drew them and its generated shaders.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from typing import Any

DOCUMENT = "passes/main_rendering_config.json"


def canvas_document() -> dict:
    return {
        "schema": "pelican.graph",
        "version": 1,
        "definitions": {},
        "nodes": [
            {"id": "card", "op": "shape.rect", "parameters": {"size": [120, 80], "center": [128, 128]},
             "inputs": {}, "outputs": ["geometry"]},
            {"id": "card_fill", "op": "shape.fill", "parameters": {"color": [0.2, 0.4, 0.9, 1.0]},
             "inputs": {"geometry": {"node": "card", "port": "geometry"}}, "outputs": ["layer"]},
            {"id": "root", "op": "canvas.stack",
             "parameters": {"size": [256, 256], "layers": [{"input": "card", "name": "card"}]},
             "inputs": {"card": {"node": "card_fill", "port": "layer"}}, "outputs": ["layer"]},
            {"id": "out", "op": "canvas.output", "parameters": {"fit": "contain"},
             "inputs": {"layer": {"node": "root", "port": "layer"}}, "outputs": ["image"]},
        ],
        "outputs": {},
        "rendering": {
            "rendering_passes": [{
                "name": "main_render",
                "passes": [{
                    "name": "present",
                    "type": "fullscreen",
                    "input": ["out"],
                    "output": {"color": ["display"], "depth": None},
                    "shader": {"vertex": "engine://fullscreen", "fragment": "engine://passthrough"},
                }],
            }],
        },
    }


def make_project(cli: Path, root: Path) -> Path:
    project = root / "canvas_live"
    subprocess.run([str(cli), "project", "init", str(project), "--template", "blank"], check=True,
                   capture_output=True, text=True)
    project_json = project / "project.json"
    document = json.loads(project_json.read_text(encoding="utf-8"))
    document["requires"] = ["canvas"]
    document["basic_config"]["window_size"] = {"width": 256, "height": 256}
    project_json.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    (project / DOCUMENT).write_text(json.dumps(canvas_document(), indent=2) + "\n", encoding="utf-8")
    return project


def source_digest(client: Any) -> str:
    return client.call("get_render_authoring_context")["source_digest"]["hex"]


def file_digest(project: Path) -> str:
    return hashlib.sha256((project / DOCUMENT).read_bytes()).hexdigest()


def wait_result(client: Any, accepted: dict, seconds: float = 60.0) -> dict:
    if accepted.get("status") != "accepted":
        return accepted
    deadline = time.monotonic() + seconds
    while True:
        result = client.call("get_edit_result", {"ticket": accepted["ticket"]})
        if result.get("status") not in ("pending", "running"):
            return result
        if time.monotonic() > deadline:
            raise AssertionError(f"{accepted['ticket']} did not end: {result!r}")
        # A headless engine commits an edit on the frame it is stepped to.
        client.call("step_frame", {})


def picture(client: Any, path: Path) -> bytes:
    # Two frames: the first after a publication may still be the old one.
    client.call("step_frame", {})
    client.call("step_frame", {})
    client.call("capture", {"path": str(path)})
    return path.read_bytes()


def edit(client: Any, ops: list[dict]) -> dict:
    return wait_result(client, client.call("edit_graph_document",
                                           {"base_source_digest": source_digest(client), "ops": ops}))


def check(player: Path, project: Path, frames: Path) -> None:
    from tools.pelican_rpc import PelicanRpc

    with PelicanRpc(project, exe_path=player, headless=True) as client:
        # The CAS digest is the authored document's.
        if source_digest(client) != file_digest(project):
            raise AssertionError("the source digest is not the authored document's")
        before = picture(client, frames / "before.png")

        # A value edit of the author's node commits and changes the picture.
        resized = edit(client, [{"op": "set_node_parameters", "node": "card",
                                 "parameters": {"size": [200, 40], "center": [128, 128]}}])
        if resized.get("status") != "committed":
            raise AssertionError(f"the size edit did not commit: {resized!r}")
        authored = json.loads((project / DOCUMENT).read_text(encoding="utf-8"))
        card = next(node for node in authored["nodes"] if node["id"] == "card")
        if card["op"] != "shape.rect" or card["parameters"]["size"] != [200, 40]:
            raise AssertionError(f"the saved document is not the author's: {card!r}")
        if any(node["id"].startswith("out$canvas_") for node in authored["nodes"]):
            raise AssertionError("the lowered nodes were saved into the authored document")
        if authored["rendering"]["rendering_passes"][0]["passes"][0]["input"] != ["out"]:
            raise AssertionError("the rendering section was saved lowered")
        if source_digest(client) != file_digest(project):
            raise AssertionError("after the edit, the source digest is not the authored document's")
        after = picture(client, frames / "after.png")
        if after == before:
            raise AssertionError("the size edit did not change the picture")

        # An edit the lowering refuses by name changes nothing.
        saved = (project / DOCUMENT).read_bytes()
        blended = edit(client, [{"op": "set_node_parameters", "node": "root", "parameters": {
            "size": [256, 256], "layers": [{"input": "card", "name": "card", "blend": "dissolve"}]}}])
        code = (blended.get("error") or {}).get("code")
        if blended.get("status") == "committed" or code != "canvas.blend_unsupported@1":
            raise AssertionError(f"the blend edit was not refused by name: {blended!r}")
        if (project / DOCUMENT).read_bytes() != saved:
            raise AssertionError("the refused edit changed the document")
        if picture(client, frames / "refused.png") != after:
            raise AssertionError("the refused edit changed the picture")

        # The pass the canvas made is the author's output node.
        plan = client.call("get_frame_plan", {})
        passes = [node for node in plan["nodes"] if node.get("name") == "out"]
        if len(passes) != 1 or passes[0].get("origin", {}).get("node") != "out":
            raise AssertionError(f"the frame plan's pass out: {passes!r}")

        lowering = client.call("get_authoring_lowering", {})
        if lowering["source_digest"]["hex"] != file_digest(project):
            raise AssertionError(f"get_authoring_lowering read another document: {lowering['source_digest']!r}")
        outputs = lowering["reports"]["canvas"]["outputs"]
        if [output["id"] for output in outputs] != ["out"]:
            raise AssertionError(f"get_authoring_lowering outputs: {outputs!r}")
        items = outputs[0]["items"]
        if items != [{"instance": 0, "kind": "rect_fill", "nodes": ["card_fill", "card", "root"]}]:
            raise AssertionError(f"get_authoring_lowering items: {items!r}")
        generated = outputs[0]["generated"]
        if [entry["stage"] for entry in generated] != ["vert", "frag"] or \
                any(not entry["text"] for entry in generated):
            raise AssertionError(f"get_authoring_lowering generated: {generated!r}")
        stem = generated[0]["stem"]
        for entry in generated:
            if not (project / entry["path"]).is_file() or \
                    entry["path"] != f".pelican/generated/canvas/{stem}/{stem}.{entry['stage']}":
                raise AssertionError(f"the generated shader is not where the pass reads it: {entry['path']!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--player", required=True, type=Path)
    parser.add_argument("--cli", required=True, type=Path)
    args = parser.parse_args()

    sys.path.insert(0, str(args.repo_root.resolve()))
    with tempfile.TemporaryDirectory(prefix="pelican_canvas_live_") as temporary:
        root = Path(temporary)
        frames = root / "frames"
        frames.mkdir()
        project = make_project(args.cli.resolve(), root)
        check(args.player.resolve(), project, frames)
    print("canvas_live_edit_rpc passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
