#!/usr/bin/env python3
"""Write projects/pathtrace_demo/assets/checker_box.glb: a unit box with a
checkerboard base color texture, 32-bit indices, and one glTF PBR material.
Standard library only; the output is deterministic."""
from __future__ import annotations
import json, struct, sys, zlib
from pathlib import Path

def png_rgba(width: int, height: int, pixel) -> bytes:
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        for x in range(width):
            raw.extend(pixel(x, y))
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))

def checker(x: int, y: int):
    cell = ((x // 8) + (y // 8)) % 2
    return (235, 235, 235, 255) if cell else (40, 90, 200, 255)

def main() -> int:
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "projects/pathtrace_demo/assets/checker_box.glb")
    faces = [  # normal, tangent-ish u axis, v axis
        ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0)),
        ((1, 0, 0), (0, 0, -1), (0, 1, 0)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
        ((0, 1, 0), (1, 0, 0), (0, 0, -1)), ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
    ]
    positions, normals, uvs, indices = [], [], [], []
    for n, u, v in faces:
        base = len(positions)
        for (su, sv, tu, tv) in ((-1, -1, 0, 1), (1, -1, 1, 1), (1, 1, 1, 0), (-1, 1, 0, 0)):
            positions.append(tuple(0.5 * n[i] + 0.5 * su * u[i] + 0.5 * sv * v[i] for i in range(3)))
            normals.append(tuple(float(c) for c in n))
            uvs.append((float(tu), float(tv)))
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    def pack(fmt, rows):
        return b"".join(struct.pack("<" + fmt, *r) for r in rows)
    pos_bytes, nrm_bytes, uv_bytes = pack("fff", positions), pack("fff", normals), pack("ff", uvs)
    idx_bytes = struct.pack("<%dI" % len(indices), *indices)
    image = png_rgba(64, 64, checker)
    blobs = [pos_bytes, nrm_bytes, uv_bytes, idx_bytes, image]
    buffer = bytearray(); views = []
    for b in blobs:
        views.append({"buffer": 0, "byteOffset": len(buffer), "byteLength": len(b)})
        buffer += b + b"\0" * ((4 - len(b) % 4) % 4)
    mins = [min(p[i] for p in positions) for i in range(3)]; maxs = [max(p[i] for p in positions) for i in range(3)]
    gltf = {
        "asset": {"version": "2.0", "generator": "pelican tools/pt1b_make_checker_box.py"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": "checker_box"}],
        "meshes": [{"name": "checker_box", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}]}],
        "materials": [{"name": "checker", "pbrMetallicRoughness": {"baseColorFactor": [1.0, 1.0, 1.0, 1.0], "baseColorTexture": {"index": 0}, "metallicFactor": 0.0, "roughnessFactor": 1.0}}],
        "textures": [{"source": 0, "sampler": 0}], "samplers": [{"magFilter": 9728, "minFilter": 9728, "wrapS": 10497, "wrapT": 10497}],
        "images": [{"bufferView": 4, "mimeType": "image/png", "name": "checker"}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3", "min": mins, "max": maxs},
            {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": len(uvs), "type": "VEC2"},
            {"bufferView": 3, "componentType": 5125, "count": len(indices), "type": "SCALAR"},
        ],
        "bufferViews": views, "buffers": [{"byteLength": len(buffer)}],
    }
    js = json.dumps(gltf, separators=(",", ":")).encode(); js += b" " * ((4 - len(js) % 4) % 4)
    glb = b"glTF" + struct.pack("<II", 2, 12 + 8 + len(js) + 8 + len(buffer))
    glb += struct.pack("<I", len(js)) + b"JSON" + js + struct.pack("<I", len(buffer)) + b"BIN\0" + bytes(buffer)
    out.parent.mkdir(parents=True, exist_ok=True); out.write_bytes(glb)
    print(f"wrote {out} ({len(glb)} bytes, {len(indices)//3} triangles)")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
