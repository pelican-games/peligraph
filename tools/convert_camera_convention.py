#!/usr/bin/env -S uv run --quiet --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Convert a project written before the camera convention changed (CC-1b).

    uv run tools/convert_camera_convention.py [--dry-run] <project dir>...

Before the change a camera looked along +Z of its rotation, R * (0, 0, 1), and
took its up from project.json's `basic_config.camera.up`; an orbit
controller's yaw / pitch were measured in a frame built on that up. Now a
camera looks along -Z of its rotation with up R * (0, 1, 0), the world's up is
+Y, and the key is refused by name (`project.camera_up_removed@1`). See
docs/design_coordinate_convention.md.

What the script rewrites, in place, changing only the values it converts (the
rest of each file keeps its bytes):

- project.json: `basic_config.camera.up` is removed (and `camera`, if that
  was all it held).
- every `pelican.scene` document under the project: each object with a
  `camera` component gets the rotation R' that looks the same way, upright:
  f = R * (0, 0, 1), R' = the rotation looking along f with up +Y. When f is
  within |f . Y| > 0.999 of the vertical, R' = R * q(0, 1, 0, 0) (a half turn
  about its own Y) instead. A camera with no rotation had the identity. A
  rotation whose own up was not the upright one carried a roll the old camera
  never drew; it is dropped and reported as `camera_roll_dropped`.
  An orbit controller's angles: old up +Y -> (-yaw, pitch), -Y -> (yaw,
  -pitch), any other up through the direction they point at. A controller's
  `up` is removed.
- every `pelican.prefab` document: controllers as above. A prefab holds no
  transform, so the scene objects that place a prefab with a camera are
  reported as `camera_prefab_instances` for a hand fix.
- every `pelican.transform_seq` (JSON Lines) written by the camera bake
  (`generator: pelican2-camera-bake`): each rot becomes R * q(0, 1, 0, 0).
  A sequence from elsewhere is reported (`transform_seq_not_camera_bake`) and
  left as it is.

A project with no `basic_config.camera.up` is refused (it is already
converted, or was never written with the key); pass `--assume-up X,Y,Z` to
convert it as if it had that up (the old engine default was 0,-1,0).

The report is JSON on stdout: {projects: [{project, dry_run, old_up, changed,
camera_roll_dropped, camera_prefab_instances, transform_seq_not_camera_bake,
manual}]}. Exit 1 when a project cannot be converted (nothing is written).
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
from pathlib import Path

WORLD_UP = (0.0, 1.0, 0.0)
CAMERA_BAKE_GENERATOR = "pelican2-camera-bake"


# --- vectors and quaternions ([x, y, z, w], as the documents write them) ---

def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def scale(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def normalized(a):
    length = math.sqrt(dot(a, a))
    if not length > 1e-12:
        return None
    return scale(a, 1.0 / length)


def normalized_quat(q):
    length = math.sqrt(sum(c * c for c in q))
    if not length > 1e-12:
        return (0.0, 0.0, 0.0, 1.0)
    return tuple(c / length for c in q)


def rotate(q, v):
    axis = (q[0], q[1], q[2])
    t = scale(cross(axis, v), 2.0)
    return add(add(v, scale(t, q[3])), cross(axis, t))


def multiply(a, b):
    """Hamilton product a * b (b is applied first)."""
    return (
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
    )


def quat_from_basis(right, up, back):
    """The rotation whose columns are right, up and back."""
    m00, m10, m20 = right
    m01, m11, m21 = up
    m02, m12, m22 = back
    trace = m00 + m11 + m22
    if trace > 0.0:
        s = math.sqrt(trace + 1.0) * 2.0
        return ((m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25 * s)
    if m00 > m11 and m00 > m22:
        s = math.sqrt(1.0 + m00 - m11 - m22) * 2.0
        return (0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s)
    if m11 > m22:
        s = math.sqrt(1.0 + m11 - m00 - m22) * 2.0
        return ((m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s)
    s = math.sqrt(1.0 + m22 - m00 - m11) * 2.0
    return ((m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s)


def rounded(value):
    result = round(value * 1e7) / 1e7
    return 0.0 if result == 0.0 else result


def canonical(q):
    """One of q and -q, rounded to 7 decimals: w >= 0, and on w == 0 the first
    non-zero component positive, so a rotation always prints the same."""
    negate = q[3] < 0.0
    if rounded(q[3]) == 0.0:
        negate = False
        for c in q[:3]:
            if rounded(c) != 0.0:
                negate = c < 0.0
                break
    if negate:
        q = tuple(-c for c in q)
    return tuple(rounded(c) for c in q)


HALF_TURN_Y = (0.0, 1.0, 0.0, 0.0)
ROLL_TOLERANCE = math.cos(math.radians(0.5))


def camera_rotation(forward, up):
    """clipspace.hpp cameraRotation: looks along `forward` with `up`."""
    back = scale(normalized(forward), -1.0)
    right = normalized(cross(up, back))
    true_up = cross(back, right)
    return normalized_quat(quat_from_basis(right, true_up, back))


def convert_camera_rotation(rotation):
    """(R', roll_dropped) for a camera rotation written before the change."""
    r = normalized_quat(rotation)
    forward = normalized(rotate(r, (0.0, 0.0, 1.0))) or (0.0, 0.0, 1.0)
    if abs(dot(forward, WORLD_UP)) > 0.999:
        return canonical(multiply(r, HALF_TURN_Y)), False
    migrated = camera_rotation(forward, WORLD_UP)
    old_up = rotate(r, (0.0, 1.0, 0.0))
    new_up = rotate(migrated, (0.0, 1.0, 0.0))
    return canonical(migrated), dot(old_up, new_up) < ROLL_TOLERANCE


def convert_seq_rotation(rotation):
    """A camera bake's rot: R * q(0, 1, 0, 0)."""
    return canonical(multiply(normalized_quat(rotation), HALF_TURN_Y))


def least_aligned_axis(axis):
    x, y, z = (abs(c) for c in axis)
    if z <= x and z <= y:
        return (0.0, 0.0, 1.0)
    if x <= y:
        return (1.0, 0.0, 0.0)
    return (0.0, 1.0, 0.0)


def yaw_pitch_frame(old_up=None):
    """cameracontrollersystem.cpp yawPitchBasis: before the change with the
    project's up and right = cross(up, forward); now with +Y and right =
    cross(forward, up)."""
    up = (normalized(old_up) or WORLD_UP) if old_up is not None else WORLD_UP
    forward = normalized(add((0.0, 0.0, 1.0), scale(up, -dot((0.0, 0.0, 1.0), up)))) or least_aligned_axis(up)
    if old_up is not None:
        right = normalized(cross(up, forward)) or (1.0, 0.0, 0.0)
    else:
        right = normalized(cross(forward, up)) or (-1.0, 0.0, 0.0)
    return up, forward, right


def controller_direction(yaw, pitch, old_up=None):
    """The direction (yaw, pitch) offsets an orbit camera from its target."""
    up, forward, right = yaw_pitch_frame(old_up)
    horizontal = add(scale(forward, math.cos(yaw)), scale(right, math.sin(yaw)))
    return normalized(add(scale(horizontal, math.cos(pitch)), scale(up, math.sin(pitch)))) or forward


def convert_controller_angles(yaw, pitch, old_up):
    up = normalized(old_up) or (0.0, -1.0, 0.0)
    if dot(up, WORLD_UP) > 1.0 - 1e-9:
        return -yaw, pitch
    if dot(up, WORLD_UP) < -1.0 + 1e-9:
        return yaw, -pitch
    direction = controller_direction(yaw, pitch, up)
    _, forward, right = yaw_pitch_frame(None)
    return (math.atan2(dot(direction, right), dot(direction, forward)),
            math.asin(max(-1.0, min(1.0, direction[1]))))


# --- documents ---------------------------------------------------------------

def is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def negated(value):
    if isinstance(value, int):
        return -value
    return 0.0 if value == 0.0 else -value


def convert_controller(controller, old_up):
    controller.pop("up", None)
    if controller.get("type") != "orbit":
        return
    up = normalized(old_up) or (0.0, -1.0, 0.0)
    keys = {}
    for radians, degrees in (("yaw", "yaw_degrees"), ("pitch", "pitch_degrees")):
        if is_number(controller.get(radians)):
            keys[radians] = (radians, float(controller[radians]))
        elif is_number(controller.get(degrees)):
            keys[radians] = (degrees, math.radians(controller[degrees]))
    if dot(up, WORLD_UP) > 1.0 - 1e-9:
        if "yaw" in keys:
            controller[keys["yaw"][0]] = negated(controller[keys["yaw"][0]])
        return
    if dot(up, WORLD_UP) < -1.0 + 1e-9:
        if "pitch" in keys:
            controller[keys["pitch"][0]] = negated(controller[keys["pitch"][0]])
        return
    yaw, pitch = convert_controller_angles(keys.get("yaw", (None, 0.0))[1],
                                           keys.get("pitch", (None, 0.0))[1], up)
    for name, value in (("yaw", yaw), ("pitch", pitch)):
        key = keys.get(name, (name, 0.0))[0]
        controller[key] = rounded(math.degrees(value) if key.endswith("_degrees") else value)


def convert_components(components, old_up, holds_transform, where, report):
    """Converts one component list in place when it holds a camera."""
    cameras = [c for c in components if isinstance(c, dict) and c.get("name") == "camera"]
    if not cameras:
        return
    for camera in cameras:
        if isinstance(camera.get("controller"), dict):
            convert_controller(camera["controller"], old_up)
    if not holds_transform:
        report["camera_prefab_instances"].append(where)
        return
    transforms = [i for i, c in enumerate(components) if isinstance(c, dict) and c.get("name") == "transform"]
    if not transforms:
        # No transform was the identity, which looked along +Z.
        components.insert(0, {"name": "transform", "rotation": [0, 1, 0, 0]})
        return
    transform = components[transforms[0]]
    rotation = transform.get("rotation", [0.0, 0.0, 0.0, 1.0])
    if not (isinstance(rotation, list) and len(rotation) == 4 and all(is_number(c) for c in rotation)):
        report["manual"].append(dict(where, message="the camera's rotation is not four numbers; "
                                     "multiply it by [0, 1, 0, 0] by hand"))
        return
    migrated, roll = convert_camera_rotation(rotation)
    transform["rotation"] = list(migrated)
    if roll:
        report["camera_roll_dropped"].append(where)


def convert_scene(document, old_up, path, report):
    for scene_id, scene in (document.get("scenes") or {}).items():
        if not isinstance(scene, dict):
            continue
        for index, obj in enumerate(scene.get("objects") or []):
            if isinstance(obj, dict) and isinstance(obj.get("components"), list):
                name = obj.get("name") if isinstance(obj.get("name"), str) else "#%d" % index
                convert_components(obj["components"], old_up, True,
                                   {"path": path, "scene": scene_id, "object": name}, report)


def convert_prefab(document, old_up, path, report):
    if isinstance(document.get("components"), list):
        convert_components(document["components"], old_up, False,
                           {"path": path, "prefab": document.get("name")}, report)


def convert_document(document, old_up, path, report):
    """Converts one parsed document in place by its schema; returns whether it
    is one the convention touches."""
    if not isinstance(document, dict):
        return False
    schema = document.get("schema")
    if schema == "pelican.scene":
        convert_scene(document, old_up, path, report)
        return True
    if schema == "pelican.prefab":
        convert_prefab(document, old_up, path, report)
        return True
    if schema == "pelican.project":
        camera = (document.get("basic_config") or {}).get("camera")
        if isinstance(camera, dict) and "up" in camera:
            del camera["up"]
            if not camera:
                del document["basic_config"]["camera"]
        return True
    return False


# --- writing back only what changed ------------------------------------------

class Node:
    __slots__ = ("kind", "start", "end", "items", "value")

    def __init__(self, kind, start):
        self.kind, self.start, self.end, self.items, self.value = kind, start, None, None, None


def parse_spans(text):
    """A JSON parser that keeps the span of every value."""
    n = len(text)

    def ws(i):
        while i < n and text[i] in " \t\r\n":
            i += 1
        return i

    def string_end(i):
        i += 1
        while text[i] != '"':
            i += 2 if text[i] == "\\" else 1
        return i + 1

    def value(i):
        i = ws(i)
        c = text[i]
        if c in "{[":
            node = Node("object" if c == "{" else "array", i)
            node.items = []
            close = "}" if c == "{" else "]"
            i = ws(i + 1)
            if text[i] == close:
                node.end = i + 1
                return node
            while True:
                i = ws(i)
                if c == "{":
                    key_end = string_end(i)
                    key = json.loads(text[i:key_end])
                    key_start = i
                    i = ws(key_end)
                    child = value(i + 1)
                    node.items.append((key, key_start, child))
                else:
                    child = value(i)
                    node.items.append(child)
                i = ws(child.end)
                if text[i] == ",":
                    i += 1
                    continue
                node.end = i + 1
                return node
        node = Node("leaf", i)
        if c == '"':
            node.end = string_end(i)
        else:
            j = i
            while j < n and text[j] not in ",]} \t\r\n":
                j += 1
            node.end = j
        node.value = json.loads(text[node.start:node.end])
        return node

    return value(1 if text.startswith("\ufeff") else 0)


def same(a, b):
    if isinstance(a, bool) or isinstance(b, bool):
        return type(a) is type(b) and a == b
    if is_number(a) and is_number(b):
        return a == b
    return type(a) is type(b) and a == b


def line_indent(text, pos):
    start = text.rfind("\n", 0, pos) + 1
    end = start
    while end < len(text) and text[end] in " \t":
        end += 1
    return text[start:end]


def render(value, text, pos, newline):
    return json.dumps(value, indent=2, ensure_ascii=False).replace("\n", newline + line_indent(text, pos))


def number_text(old_text, new):
    # 1.0 where the document wrote integers stays an integer.
    if isinstance(new, float) and new.is_integer() and not any(c in old_text for c in ".eE"):
        return str(int(new))
    return json.dumps(new)


def diff(node, new, text, edits, newline):
    if node.kind == "object" and isinstance(new, dict):
        old_keys = [key for key, _, _ in node.items]
        for index, (key, key_start, child) in enumerate(node.items):
            if key not in new:
                if index + 1 < len(node.items):
                    edits.append((key_start, node.items[index + 1][1], ""))
                elif index > 0:
                    edits.append((node.items[index - 1][2].end, child.end, ""))
                else:
                    edits.append((node.start + 1, node.end - 1, ""))
            else:
                diff(child, new[key], text, edits, newline)
        added = [key for key in new if key not in old_keys]
        if added:
            edits.append((node.start, node.end, render(new, text, node.start, newline)))
        return
    if node.kind == "array" and isinstance(new, list) and len(new) == len(node.items):
        for child, value in zip(node.items, new):
            diff(child, value, text, edits, newline)
        return
    if node.kind == "leaf" and same(node.value, new):
        return
    if node.kind == "leaf" and is_number(node.value) and is_number(new):
        edits.append((node.start, node.end, number_text(text[node.start:node.end], new)))
        return
    edits.append((node.start, node.end, render(new, text, node.start, newline)))


def patch_text(text, node, new):
    """`text` with the value at `node` rewritten to `new`, touching only the
    spans that differ."""
    newline = "\r\n" if "\r\n" in text else "\n"
    edits = []
    diff(node, new, text, edits, newline)
    for start, end, replacement in sorted(edits, reverse=True):
        text = text[:start] + replacement + text[end:]
    return text


def patch_document(text, new):
    patched = patch_text(text, parse_spans(text), new)
    if json.loads(patched) != new:
        raise RuntimeError("internal: the patched text does not read back as the converted document")
    return patched


# --- projects -----------------------------------------------------------------

class ConvertError(Exception):
    pass


def read_text(path):
    return path.read_bytes().decode("utf-8")


def project_up(project, assume_up):
    camera = (project.get("basic_config") or {}).get("camera") or {}
    up = camera.get("up")
    if up is None:
        if assume_up is None:
            raise ConvertError(
                "project.json has no basic_config.camera.up: it is already converted, or was "
                "never written with the key. Pass --assume-up X,Y,Z to convert it anyway "
                "(the old engine default was 0,-1,0)")
        up = assume_up
    if not (isinstance(up, list) and len(up) == 3 and all(is_number(c) for c in up)
            and normalized(tuple(float(c) for c in up))):
        raise ConvertError("basic_config.camera.up must be three numbers, not all zero: %r" % (up,))
    return tuple(float(c) for c in up)


def project_files(root):
    for directory, dirs, files in os.walk(root):
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        for name in sorted(files):
            if name.lower().endswith((".json", ".jsonl")):
                yield Path(directory) / name


def convert_transform_seq(text, rel, report):
    lines = text.splitlines(keepends=True)
    header = json.loads(lines[0])
    if header.get("generator") != CAMERA_BAKE_GENERATOR:
        report["transform_seq_not_camera_bake"].append(rel)
        return text
    out = [lines[0]]
    for line in lines[1:]:
        body = line.rstrip("\r\n")
        if not body.strip():
            out.append(line)
            continue
        frame = json.loads(body)
        for transform in frame.get("transforms", []):
            if isinstance(transform.get("rot"), list) and len(transform["rot"]) == 4:
                transform["rot"] = list(convert_seq_rotation(transform["rot"]))
        out.append(patch_text(body, parse_spans(body), frame) + line[len(body):])
    return "".join(out)


def convert_project(root, dry_run=False, assume_up=None):
    root = Path(root)
    project_path = root / "project.json"
    if not project_path.is_file():
        raise ConvertError("%s has no project.json" % root)
    project_text = read_text(project_path)
    project = json.loads(project_text)
    if not isinstance(project, dict) or project.get("schema") != "pelican.project":
        raise ConvertError("%s is not a pelican.project document" % project_path)
    old_up = project_up(project, assume_up)
    report = {"project": str(root), "dry_run": dry_run, "old_up": list(old_up), "changed": [],
              "camera_roll_dropped": [], "camera_prefab_instances": [],
              "transform_seq_not_camera_bake": [], "manual": []}
    writes = []
    for path in project_files(root):
        rel = path.relative_to(root).as_posix()
        text = read_text(path)
        if path.suffix.lower() == ".jsonl":
            first = text.split("\n", 1)[0].strip()
            try:
                header = json.loads(first) if first else None
            except ValueError:
                continue
            if not isinstance(header, dict) or header.get("schema") != "pelican.transform_seq":
                continue
            after = convert_transform_seq(text, rel, report)
        else:
            try:
                document = json.loads(text)
            except ValueError:
                continue
            if not convert_document(document, old_up, rel, report):
                continue
            after = patch_document(text, document)
        if after != text:
            writes.append((path, after))
            report["changed"].append(rel)
    if not dry_run:
        for path, after in writes:
            path.write_bytes(after.encode("utf-8"))
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("projects", nargs="+", help="project directories (each holds project.json)")
    parser.add_argument("--dry-run", action="store_true", help="report what would change, write nothing")
    parser.add_argument("--assume-up", help="X,Y,Z: the up of a project.json that has none")
    args = parser.parse_args(argv)
    assume_up = None
    if args.assume_up:
        assume_up = [float(c) for c in args.assume_up.split(",")]
    reports = []
    for project in args.projects:
        try:
            reports.append(convert_project(project, args.dry_run, assume_up))
        except ConvertError as error:
            print("camera convention: %s: %s" % (project, error), file=sys.stderr)
            return 1
    print(json.dumps({"projects": reports}, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
