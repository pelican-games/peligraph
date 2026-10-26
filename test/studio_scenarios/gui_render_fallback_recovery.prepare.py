"""Prepare Blank roots refused at compose and strict IR entry for RC-3e2.

Both valid JSON roots are repaired through Studio's whole-document dialog.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path("passes") / "main_rendering_config.json"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--cli", required=True, type=Path)
    arguments = parser.parse_args()
    arguments.out.mkdir(parents=True, exist_ok=True)
    project = arguments.out / "compose"
    completed = subprocess.run(
        [str(arguments.cli), "project", "init", str(project), "--template", "blank"],
        capture_output=True,
        text=True,
    )
    sys.stdout.write(completed.stdout)
    if completed.returncode != 0:
        sys.stderr.write(completed.stderr)
        return 1
    text = (project / ROOT).read_text(encoding="utf-8")
    shutil.copytree(project, arguments.out / "ir")
    # RC-3e2: failures after JSON decoding, repaired through Studio's whole
    # document dialog. Each recovery has its own visible Frame Plan node.
    for mode in ("compose", "ir"):
        destination = arguments.out / mode
        document = json.loads(text)
        rendering = document["rendering"]
        rendering["rendering_passes"][0]["passes"][0]["name"] = f"recovered_{mode}"
        if mode == "compose":
            rendering["snapshots"] = [{"name": "capture", "after": "absent_node"}]
        else:
            rendering["render_targets"] = [
                {"name": "post", "extent_scale": 1, "format": "R8G8B8A8_UNORM",
                 "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
                {"name": "depth", "extent_scale": 1, "format": "D32_SFLOAT",
                 "usage": ["DEPTH_STENCIL_ATTACHMENT", "SAMPLED"]},
            ]
            rendering["rendering_passes"][0]["passes"].insert(0, {
                "name": "tone", "type": "material", "material_contract": "forward_transparent_v1",
                "screen_inputs": {"opaque_color": "post"},
                "output": {"color": "post", "depth": "depth"},
            })
        (destination / ROOT).write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
