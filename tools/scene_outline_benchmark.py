"""Compare scene_tree and scene_outline in the same Release player.

Run under pelican_gpu_lease_run. Requires engine_survey.py alongside this file.
Only fresh private project copies are edited. Pipe/engine time and client JSON
decode time are reported separately; these are not GUI interaction latencies.
"""
from __future__ import annotations

import argparse
import hashlib
import statistics
import subprocess
from pathlib import Path

from engine_survey import ROOT, Rpc, measure, prepare, save


def run_case(player: Path, directory: Path, count: int, repeat: int):
    directory.mkdir()
    project = directory / "project"
    prepare(ROOT / "test/fixtures/studio_automation_graph", project, count, False)
    client = Rpc(player, project, directory, "640x360")
    result = {"objects": count, "repeat": repeat, "operations": {}}
    try:
        save(directory / "status.json", client.call("get_status")[0])
        for _ in range(8):
            client.call("step_frame")
        values = {}
        methods = ["scene_tree", "scene_outline"]
        if repeat % 2:
            methods.reverse()
        for method in methods:
            result["operations"][method], values[method] = measure(client, method)
        tree, outline = values["scene_tree"], values["scene_outline"]
        assert len(tree["objects"]) == len(outline["objects"]) == count
        names = {row["name"]: row["authoring_object_id"] for row in tree["objects"] if "name" in row}
        for original, row in zip(tree["objects"], outline["objects"]):
            assert row["authoring_object_id"] == original["authoring_object_id"] != 0
            assert row["declaration_index"] == original["declaration_index"]
            assert ("name" in row) == ("name" in original)
            assert row.get("name") == original.get("name")
            assert row["parent_authoring_object_id"] == names.get(original.get("parent"))
        params = {"scene_id": outline["scene_id"], "if_scene_revision": outline["scene_revision"]}
        result["operations"]["scene_outline_unchanged"], unchanged = measure(client, "scene_outline", params)
        assert unchanged["not_modified"] and "objects" not in unchanged

        # Verify a real committed edit invalidates the conditional response,
        # while IDs remain compatible with detail queries.
        actor = client.call("open_editor_session", {"display_name": "Outline benchmark"})[0]["actor_id"]
        object_id = outline["objects"][0]["authoring_object_id"]
        accepted = client.call("edit", {
            "actor_id": actor, "base_revision": outline["scene_revision"],
            "operations": [{"op": "set_component_value", "object_id": object_id,
                            "component_slot": "transform", "field_path": "/pos", "value": [2, 3, 4]}],
        })[0]
        assert accepted["status"] == "accepted", accepted
        client.call("step_frame")
        committed = client.call("get_edit_result", {"ticket": accepted["ticket"]})[0]
        assert committed["status"] == "committed", committed
        updated = client.call("scene_outline", params)[0]
        assert not updated["not_modified"] and updated["scene_revision"] != outline["scene_revision"]
        assert updated["objects"] == outline["objects"]
        detail = client.call("get_components", {"scene_id": outline["scene_id"], "authoring_object_id": object_id})[0]
        assert detail["authoring_object_id"] == object_id
        result["edit_invalidates_poll"] = True
    finally:
        result["exit_code"] = client.close()
        save(directory / "result.json", result)
    assert result["exit_code"] == 0, result
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    player, out = args.player.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    binary_hash = hashlib.sha256(player.read_bytes()).hexdigest()
    save(out / "environment.json", {
        "player": str(player), "player_sha256": binary_hash,
        "source_base": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "samples": 15, "warmup": 3, "repeats": 3, "order": ["tree-outline", "outline-tree", "tree-outline"],
        "gpu_lease": "caller must hold the shared GPU lease",
    })
    rows = []
    for repeat in range(3):
        for count in [1000, 10000]:
            assert hashlib.sha256(player.read_bytes()).hexdigest() == binary_hash
            rows.append(run_case(player, out / f"{repeat}-{count}", count, repeat))
            save(out / "results.json", rows)
            print(f"Completed repeat={repeat} objects={count}", flush=True)
    summary = {}
    for count in [1000, 10000]:
        summary[count] = {}
        for method in ["scene_tree", "scene_outline", "scene_outline_unchanged"]:
            values = [r["operations"][method] for r in rows if r["objects"] == count]
            medians = [v["median_ms"] for v in values]
            summary[count][method] = {
                "median_ms": statistics.median(medians), "run_medians_ms": medians,
                "response_bytes": statistics.median(v["response_bytes_median"] for v in values),
                "client_parse_median_ms": statistics.median(v["client_parse"]["median_ms"] for v in values),
            }
    save(out / "summary.json", summary)


if __name__ == "__main__":
    main()
