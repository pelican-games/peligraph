#!/usr/bin/env python3
"""FG-2: verify_fold in a running engine, with its real children.

A windowed engine (hidden by --embedded) folds the material pass
decompose_material_pass put into the graph, with an isolate decision that
does not change the picture. verify_fold starts the engine's own executable
three times, headless (the open form twice, the decided form once), compares
the frames byte for byte and marks the group verified; the next composition
applies the decision (fold_decision_applied@1 in the frame plan). The same
request in a headless engine is refused without starting anything.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import tempfile
import time
from typing import Any

PASS = "forward_transparent"
GROUP = "fg2_post"


def make_window_project(root: Path, rendering_config: Path) -> Path:
    from pelican_rpc_smoke import make_project

    project = make_project(root, rendering_config)
    project_json = project / "project.json"
    document = json.loads(project_json.read_text(encoding="utf-8"))
    document["name"] = "fg2_verify_fold_windowed"
    document["basic_config"]["window_size"] = {"width": 160, "height": 90}
    document["basic_config"]["fullscreen"] = False
    project_json.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    return project


def source_digest(client: Any) -> str:
    return client.call("get_render_authoring_context")["source_digest"]["hex"]


def wait_result(client: Any, accepted: dict, seconds: float) -> dict:
    if accepted.get("status") != "accepted":
        raise AssertionError(f"not accepted: {accepted!r}")
    deadline = time.monotonic() + seconds
    while True:
        result = client.call("get_edit_result", {"ticket": accepted["ticket"]})
        if result.get("status") not in ("pending", "running"):
            return result
        if time.monotonic() > deadline:
            raise AssertionError(f"{accepted['ticket']} did not end: {result!r}")
        time.sleep(0.1)


def committed(client: Any, accepted: dict, seconds: float = 30.0) -> dict:
    result = wait_result(client, accepted, seconds)
    if result.get("status") != "committed":
        raise AssertionError(f"not committed: {result!r}")
    return result


def fold_decisions(plan: dict) -> list[dict]:
    physical = plan.get("physical_target_plan") or {}
    return [decision for decision in physical.get("decisions", []) if "from_fold" in decision]


def declare(player: Path, project: Path) -> None:
    """Decompose the pass and fold it; then the engine is started again."""
    from tools.pelican_rpc import PelicanRpc

    with PelicanRpc(project, exe_path=player, headless=False, extra_arguments=["--embedded"]) as client:
        committed(client, client.call("decompose_material_pass",
                                      {"base_source_digest": source_digest(client), "pass": PASS}))
        root = json.loads((project / "passes/main_rendering_config.json").read_text(encoding="utf-8"))
        ids = [node["id"] for node in root["nodes"]]
        nodes = [node for node in (PASS, f"{PASS}_draws") if node in ids]
        if PASS not in nodes:
            raise AssertionError(f"decompose_material_pass left no node {PASS}: {ids!r}")
        committed(client, client.call("fold_nodes", {
            "base_source_digest": source_digest(client),
            "group": GROUP,
            "nodes": nodes,
            "folded": {"kind": "decisions", "decisions": {"isolate": [{"node": PASS}]}},
        }))


def check(player: Path, project: Path) -> None:
    from tools.pelican_rpc import PelicanRpc

    declare(player, project)
    with PelicanRpc(project, exe_path=player, headless=False, extra_arguments=["--embedded"]) as client:
        groups = client.call("get_fold_groups")["fold_groups"]
        if [group["state"] for group in groups if group["group"] == GROUP] != ["unverified"]:
            raise AssertionError(f"get_fold_groups before verify_fold: {groups!r}")
        if fold_decisions(client.call("get_frame_plan")):
            raise AssertionError("an unverified group's decisions were applied")

        started = time.monotonic()
        accepted = client.call("verify_fold", {
            "base_source_digest": source_digest(client),
            "group": GROUP,
            "frames": 2,
            "warmup_seconds": 0.5,
        })
        # The frames go on while the children draw.
        first = client.get_status()["frame"]
        time.sleep(0.5)
        if client.get_status()["frame"] <= first:
            raise AssertionError("the engine stopped drawing while verify_fold ran")
        result = wait_result(client, accepted, 100.0)
        elapsed = time.monotonic() - started
        if result.get("status") != "committed":
            raise AssertionError(f"verify_fold: {result!r}")
        if result.get("operation") != "verify_fold" or result.get("group") != GROUP:
            raise AssertionError(f"verify_fold result: {result!r}")
        groups = {group["group"]: group for group in client.call("get_fold_groups")["fold_groups"]}
        group = groups[GROUP]
        if group["state"] != "verified" or group["verified"] != result["verified"] or \
                group["verified"]["digest"] != group["digest"] or group["verified"]["frames"] != 2:
            raise AssertionError(f"get_fold_groups after verify_fold: {group!r} / {result!r}")
        deadline = time.monotonic() + 10.0
        applied = fold_decisions(client.call("get_frame_plan"))
        while not applied and time.monotonic() < deadline:
            time.sleep(0.1)
            applied = fold_decisions(client.call("get_frame_plan"))
        if not applied or any(decision.get("id") != "pelican.plan.fold_decision_applied@1" or
                              decision.get("from_fold") != GROUP for decision in applied):
            raise AssertionError(f"the frame plan's fold decisions: {applied!r}")
    print(f"fg2 verify_fold: committed in {elapsed:.1f} s, {len(applied)} decision(s) applied")

    # A headless engine does not start the runs.
    with PelicanRpc(project, exe_path=player, headless=True) as client:
        refused = client.call("verify_fold", {"base_source_digest": source_digest(client), "group": GROUP})
        error = refused.get("error", {})
        if refused.get("status") != "rejected" or error.get("code") != "fold_verify_unavailable" or \
                error.get("reason") != "headless":
            raise AssertionError(f"headless verify_fold: {refused!r}")


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
    with tempfile.TemporaryDirectory(prefix="pelican_fg2_verify_fold_") as temporary:
        project = make_window_project(Path(temporary), rendering_config)
        check(player, project)
    print("fg2_verify_fold_windowed passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
