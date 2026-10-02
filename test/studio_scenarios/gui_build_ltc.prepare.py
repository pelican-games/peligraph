"""IA-1s, decision 7: the two LTC tables gui_build_ltc imports, made outside Studio.

What a person does before opening Studio: a script of their own writes the
tables as raw floats and `pelican_cli image ktx2 --raw` bakes them into .ktx2
files the project can start an image from. No published LTC table is used; the
values come from a short closed form that stands in for Heitz et al. 2016's
fitted GGX tables (the scenario checks that the tables are read and change the
picture, not that they are right):

  column i: roughness r = i / 63; the lobe's alpha a = max(r^2, 0.02)
  row j:    t = j / 63 = sqrt(1 - cos(theta)), theta the view angle
  ltc_1 = (1, 0, s, a), s = min(tan(theta), 4) * (1 - a): the inverse matrix
          M^-1 = mat3((1, 0, 0), (0, 1, 0), (s, 0, a)) by columns, which turns
          the mirror direction onto the lobe's axis and narrows it by a
  ltc_2 = (m, f, 0, 1), m = 1 - a / 2 the lobe's magnitude and
          f = (1 - cos(theta))^5 / 4 the Fresnel term's share

Each table is 64 x 64 RGBA, 32-bit float, rows packed from the first row.

ltc_2_rebaked is ltc_2 baked again with f = 1/2 everywhere: the table a person
copies over the project's later (IA-1s, decision 5), which makes the
reflection brighter.

    python gui_build_ltc.prepare.py --out <folder> --cli <pelican_cli>
"""

from __future__ import annotations

import argparse
import math
import struct
import subprocess
import sys
from pathlib import Path

SIZE = 64


def tables() -> tuple[list[float], list[float], list[float]]:
    ltc_1: list[float] = []
    ltc_2: list[float] = []
    rebaked: list[float] = []
    for row in range(SIZE):
        t = row / (SIZE - 1)
        cos_theta = max(1.0 - t * t, 0.0)
        sin_theta = math.sqrt(max(1.0 - cos_theta * cos_theta, 0.0))
        tan_theta = sin_theta / max(cos_theta, 1.0e-3)
        for column in range(SIZE):
            roughness = column / (SIZE - 1)
            alpha = max(roughness * roughness, 0.02)
            skew = min(tan_theta, 4.0) * (1.0 - alpha)
            ltc_1 += [1.0, 0.0, skew, alpha]
            ltc_2 += [1.0 - alpha / 2.0, (1.0 - cos_theta) ** 5 / 4.0, 0.0, 1.0]
            rebaked += [1.0 - alpha / 2.0, 0.5, 0.0, 1.0]
    return ltc_1, ltc_2, rebaked


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--cli", required=True, type=Path)
    arguments = parser.parse_args()
    arguments.out.mkdir(parents=True, exist_ok=True)
    for name, values in zip(("ltc_1", "ltc_2", "ltc_2_rebaked"), tables()):
        raw = arguments.out / f"{name}.raw"
        raw.write_bytes(struct.pack(f"<{len(values)}f", *values))
        completed = subprocess.run(
            [
                str(arguments.cli),
                "image",
                "ktx2",
                "--out",
                str(arguments.out / f"{name}.ktx2"),
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
