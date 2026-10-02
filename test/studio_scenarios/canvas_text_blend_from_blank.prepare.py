"""SG-3: the font canvas_text_blend_from_blank imports, put where a person keeps one.

The repository holds no font file. The scenario imports the system's Arial,
the face CV-4's and CV-6a's headless tests draw text with
(test/run_canvas_text_headless.cmake), copied into this run's incoming/
folder as a person's download would be; Studio's Import font... copies it
into the project from there. Without it the scenario cannot run, and says so.

    python canvas_text_blend_from_blank.prepare.py --out <folder> --cli <pelican_cli>
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    # Every prepare script is handed the CLI; this one has no use for it.
    parser.add_argument("--cli", required=True, type=Path)
    arguments = parser.parse_args()
    font = Path(os.environ.get("SystemRoot", "C:/Windows")) / "Fonts" / "arial.ttf"
    if not font.is_file():
        sys.stderr.write(f"no {font}: the scenario imports the system's Arial\n")
        return 1
    arguments.out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(font, arguments.out / "arial.ttf")
    print(f"copied {font} to {arguments.out / 'arial.ttf'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
