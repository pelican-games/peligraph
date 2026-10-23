#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Write the ground box glb that the repository tracks in three places:

    projects/vrm_xr_demo/assets/ground.glb
    test/fixtures/ground.glb
    test/fixtures/studio_lighting_pass_template/assets/ground.glb

The model is a box 2 wide, GROUND_HEIGHT tall and 2 deep, standing on y = 0,
with flat per-face normals, a planar (x, z) texture coordinate and one
double-sided red glTF PBR material. It has 24 vertices (three per corner, one
for each face that meets there) and 12 triangles. The geometry, the material
values and the vertex and index order are those the earlier hand-exported file
had, so every image and test that loads it is unchanged; only the names and
the generator string differ.

Standard library only; the output is deterministic.

    uv run tools/generate_ground_glb.py            # rewrite the three files
    uv run tools/generate_ground_glb.py --check    # exit 1 if one differs
    uv run tools/generate_ground_glb.py OUT.glb    # write one file elsewhere
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TRACKED = (
    "projects/vrm_xr_demo/assets/ground.glb",
    "test/fixtures/ground.glb",
    "test/fixtures/studio_lighting_pass_template/assets/ground.glb",
)

GROUND_HEIGHT = 0.6181437373161316  # a float32 value
# Linear base colour (red) and the material's scalar factors.
BASE_COLOR = [0.8001724481582642, 0.03560248762369156, 0.0, 1.0]  # float32 values
METALLIC = 0.0
ROUGHNESS = 0.5

# Corners, bottom face first: index = 4 * (top) + 2 * (z < 0) + (x > 0).
CORNERS = [
    (x, y, z)
    for y in (0.0, GROUND_HEIGHT)
    for z in (1.0, -1.0)
    for x in (-1.0, 1.0)
]
# Each corner has three vertices, in this order: the one of the face whose
# normal is along z, then along y, then along x.
SLOT_Z, SLOT_Y, SLOT_X = 0, 1, 2

# Faces as (normal slot, corner quad); a quad (a, b, c, d) is split into the
# triangles (a, b, c) and (a, c, d). Winding is counter-clockwise seen from
# outside.
FACES = [
    (SLOT_Y, (0, 2, 3, 1)),  # bottom, -y
    (SLOT_Y, (4, 5, 7, 6)),  # top, +y
    (SLOT_X, (1, 3, 7, 5)),  # +x
    (SLOT_X, (2, 0, 4, 6)),  # -x
    (SLOT_Z, (3, 2, 6, 7)),  # -z
    (SLOT_Z, (0, 1, 5, 4)),  # +z
]


def vertex_normal(corner: tuple[float, float, float], slot: int) -> tuple[float, float, float]:
    x, y, z = corner
    if slot == SLOT_Z:
        return (0.0, 0.0, 1.0 if z > 0.0 else -1.0)
    if slot == SLOT_Y:
        return (0.0, 1.0 if y > 0.0 else -1.0, 0.0)
    return (1.0 if x > 0.0 else -1.0, 0.0, 0.0)


def build() -> bytes:
    positions: list[float] = []
    normals: list[float] = []
    uvs: list[float] = []
    for corner in CORNERS:
        for slot in (SLOT_Z, SLOT_Y, SLOT_X):
            positions.extend(corner)
            normals.extend(vertex_normal(corner, slot))
            # u runs with x, v with z, both from 0 to 1 across the box.
            uvs.extend(((corner[0] + 1.0) / 2.0, (corner[2] + 1.0) / 2.0))
    indices: list[int] = []
    for slot, (a, b, c, d) in FACES:
        quad = [3 * corner + slot for corner in (a, b, c, d)]
        indices.extend((quad[0], quad[1], quad[2], quad[0], quad[2], quad[3]))

    vertex_count = len(CORNERS) * 3
    chunks = [
        struct.pack(f"<{len(positions)}f", *positions),
        struct.pack(f"<{len(normals)}f", *normals),
        struct.pack(f"<{len(uvs)}f", *uvs),
        struct.pack(f"<{len(indices)}H", *indices),
    ]
    targets = [34962, 34962, 34962, 34963]  # ARRAY_BUFFER / ELEMENT_ARRAY_BUFFER
    buffer_views = []
    offset = 0
    for chunk, target in zip(chunks, targets):
        buffer_views.append({"buffer": 0, "byteLength": len(chunk), "byteOffset": offset, "target": target})
        offset += len(chunk)
    binary = b"".join(chunks)
    binary += b"\0" * (-len(binary) % 4)

    document = {
        "asset": {"generator": "pelican2 tools/generate_ground_glb.py", "version": "2.0"},
        "scene": 0,
        "scenes": [{"name": "Ground", "nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "Ground"}],
        "materials": [{
            "doubleSided": True,
            "name": "Ground",
            "pbrMetallicRoughness": {
                "baseColorFactor": BASE_COLOR,
                "metallicFactor": METALLIC,
                "roughnessFactor": ROUGHNESS,
            },
        }],
        "meshes": [{
            "name": "Ground",
            "primitives": [{
                "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                "indices": 3,
                "material": 0,
            }],
        }],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": vertex_count,
             "max": [1.0, GROUND_HEIGHT, 1.0], "min": [-1.0, 0.0, -1.0], "type": "VEC3"},
            {"bufferView": 1, "componentType": 5126, "count": vertex_count, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": vertex_count, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
        ],
        "bufferViews": buffer_views,
        "buffers": [{"byteLength": len(binary)}],
    }
    text = json.dumps(document, separators=(",", ":")).encode("utf-8")
    text += b" " * (-len(text) % 4)
    total = 12 + 8 + len(text) + 8 + len(binary)
    return (struct.pack("<III", 0x46546C67, 2, total)
            + struct.pack("<I", len(text)) + b"JSON" + text
            + struct.pack("<I", len(binary)) + b"BIN\0" + binary)


def main(argv: list[str]) -> int:
    data = build()
    if argv == ["--check"]:
        stale = [p for p in TRACKED if (REPO / p).read_bytes() != data]
        for path in stale:
            print(f"{path} is not the output of tools/generate_ground_glb.py", file=sys.stderr)
        return 1 if stale else 0
    outputs = [Path(a) for a in argv] if argv else [REPO / p for p in TRACKED]
    for path in outputs:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        print(f"wrote {path} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
