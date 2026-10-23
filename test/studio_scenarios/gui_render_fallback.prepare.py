"""BS-1s: the project gui_render_fallback opens, made outside Studio.

What a person has before opening Studio: a Blank project (`pelican_cli project
init --template blank`) whose root rendering document they edited by hand and
got wrong -- a render.compute node whose buffer port says no element (CT-1's
case, the one BS-1's engine test uses). The engine refuses that root at start
with graph.port_field_missing@1. The node is written on one line so the
scenario can take exactly that text out again (a `file` `replace` step), the
way a person fixes the file in an editor of their own.

    python gui_render_fallback.prepare.py --out <folder> --cli <pelican_cli>
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path("passes") / "main_rendering_config.json"
NODE = (
    '{"id": "sim", "op": "render.compute", "parameters": {"ports": {"data": '
    '{"kind": "buffer", "access": "read_write"}}, "shader": "shaders/sim.comp"}, '
    '"outputs": []}'
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--cli", required=True, type=Path)
    arguments = parser.parse_args()
    arguments.out.mkdir(parents=True, exist_ok=True)
    project = arguments.out / "broken"
    completed = subprocess.run(
        [str(arguments.cli), "project", "init", str(project), "--template", "blank"],
        capture_output=True,
        text=True,
    )
    sys.stdout.write(completed.stdout)
    if completed.returncode != 0:
        sys.stderr.write(completed.stderr)
        return 1
    root = project / ROOT
    text = root.read_bytes().decode("utf-8")
    if text.count('"nodes": []') != 1:
        sys.stderr.write(f"{root}: expected one empty node list\n")
        return 1
    root.write_bytes(text.replace('"nodes": []', '"nodes": [' + NODE + "]").encode("utf-8"))

    # BS-1b: a second Blank project whose root lost its last } -- a file
    # saved half way, or a brace deleted by hand. It is not JSON at all.
    unclosed = arguments.out / "unclosed"
    completed = subprocess.run(
        [str(arguments.cli), "project", "init", str(unclosed), "--template", "blank"],
        capture_output=True,
        text=True,
    )
    sys.stdout.write(completed.stdout)
    if completed.returncode != 0:
        sys.stderr.write(completed.stderr)
        return 1
    root = unclosed / ROOT
    text = root.read_bytes().decode("utf-8").rstrip()
    if not text.endswith("}"):
        sys.stderr.write(f"{root}: expected the root to end with }}\n")
        return 1
    root.write_bytes((text[:-1].rstrip() + "\n").encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
