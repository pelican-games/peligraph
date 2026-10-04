"""MI-1s: the model gui_model_from_file brings in, made outside Studio.

What a person has before opening Studio: a .glb somewhere that is not the
project. The scenario's is the small checker box the material pass scenario
already uses (test/fixtures/studio_material_pass_authoring/checker_box.glb),
copied into this run's incoming/ folder, so no new binary is added to the
repository. `--cli` is accepted as every prepare script's is, and not used.

    python gui_model_from_file.prepare.py --out <folder> --cli <pelican_cli>
"""

from __future__ import annotations

import argparse
import shutil
from pathlib import Path

FIXTURE = (
    Path(__file__).resolve().parent.parent
    / "fixtures"
    / "studio_material_pass_authoring"
    / "checker_box.glb"
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--cli", type=Path)
    arguments = parser.parse_args()
    arguments.out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(FIXTURE, arguments.out / FIXTURE.name)
    print(f"copied {FIXTURE.name} to {arguments.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
