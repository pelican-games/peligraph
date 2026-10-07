"""DL-1: reproduce the reported anchored loop in private run copies only.

DL1_EXAMPLE_CONFIG optionally names a captured user document. Using a snapshot
keeps all four runs on the same input, even if the live preview is later edited.
Without it, reproduce the reported empty loop in the repository fixture copy.
"""
import argparse
import json
import os
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--out", type=Path, required=True)
parser.add_argument("--cli", required=True)
args = parser.parse_args()
source = os.environ.get("DL1_EXAMPLE_CONFIG")
for name in ("project", "project_control"):
    path = args.out.parent / name / "passes" / "main_rendering_config.json"
    document = json.loads(Path(source or path).read_text(encoding="utf-8"))
    if source:
        assert any(node["id"] == "loop" and node["op"] == "graph.loop"
                   for node in document["nodes"]), "the captured document needs loop"
        assert "loop" in document["layout"]["nodes"], "loop must be anchored"
    else:
        document["definitions"]["loop_body"] = {"nodes": [], "outputs": {}}
        document["nodes"] = [node for node in document["nodes"] if node["id"] != "loop"]
        document["nodes"].append({"id": "loop", "op": "graph.loop", "outputs": [],
            "parameters": {"branches": [{"body": "loop_body"}], "over": {"range": {"count": 1}}}})
        document.setdefault("layout", {}).setdefault("nodes", {})["loop"] = {
            "x": 582.415999198651, "y": 504.9668027630039}
    # The invariant compares bytes. Graph edits replace nodes/layout with
    # standalone dump(2) values, so start those members in that saved format.
    # Explicit LF also avoids Windows text-mode CRLF conversion in this fixture.
    text = json.dumps(document, indent=2, ensure_ascii=False)
    for key in ("nodes", "layout"):
        value = json.dumps(document[key], indent=2, ensure_ascii=False)
        indented = "\n  ".join(value.split("\n"))
        saved = json.dumps(document[key], indent=2, ensure_ascii=False, sort_keys=True)
        text = text.replace(f'  "{key}": {indented}', f'  "{key}": {saved}', 1)
    path.write_bytes((text + "\n").encode("utf-8"))
