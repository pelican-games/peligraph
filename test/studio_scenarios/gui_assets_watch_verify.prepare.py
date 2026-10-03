"""HW-2s / AV-1s: the texture gui_assets_watch_verify drops into the project.

What a person does outside Studio: a tool of their own bakes a texture, and
it is saved into assets/textures of the project after its contents were
tracked -- a file the manifest does not list ("extra"). A 4 x 4 RGBA float
image baked by `pelican_cli image ktx2 --raw`, so the engine that watches the
folder reads a real texture.

    python gui_assets_watch_verify.prepare.py --out <folder> --cli <pelican_cli>
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
from pathlib import Path

SIZE = 4


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--cli", required=True, type=Path)
    arguments = parser.parse_args()
    arguments.out.mkdir(parents=True, exist_ok=True)
    values: list[float] = []
    for row in range(SIZE):
        for column in range(SIZE):
            values += [column / (SIZE - 1), row / (SIZE - 1), 0.5, 1.0]
    raw = arguments.out / "stray.raw"
    raw.write_bytes(struct.pack(f"<{len(values)}f", *values))
    completed = subprocess.run(
        [
            str(arguments.cli),
            "image",
            "ktx2",
            "--out",
            str(arguments.out / "stray.ktx2"),
            "--format",
            "R32G32B32A32_SFLOAT",
            "--raw",
            str(raw),
            "--width",
            str(SIZE),
            "--height",
            str(SIZE),
        ],
        capture_output=True,
        text=True,
    )
    sys.stdout.write(completed.stdout)
    if completed.returncode != 0:
        sys.stderr.write(completed.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
