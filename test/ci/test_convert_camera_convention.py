from __future__ import annotations

import contextlib
import io
import json
import math
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))

import convert_camera_convention as ccc  # noqa: E402


def axis_angle(axis, degrees):
    axis = ccc.normalized(axis)
    half = math.radians(degrees) / 2.0
    return (axis[0] * math.sin(half), axis[1] * math.sin(half), axis[2] * math.sin(half), math.cos(half))


def distance(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


class CameraRotationTest(unittest.TestCase):
    def test_the_identity_looked_along_plus_z(self) -> None:
        self.assertEqual(ccc.convert_camera_rotation([0, 0, 0, 1]), ((0.0, 1.0, 0.0, 0.0), False))

    def test_a_converted_camera_looks_the_same_way_upright(self) -> None:
        for rotation in (
            axis_angle((0, 1, 0), 30),
            axis_angle((0, 1, 0), 180),
            ccc.multiply(axis_angle((0, 1, 0), -120), axis_angle((1, 0, 0), 25)),
            ccc.multiply(axis_angle((0, 1, 0), 75), axis_angle((1, 0, 0), -60)),
            (0.0799042, 0.3852203, -0.0335018, 0.9187483),  # projects/example's camera
        ):
            with self.subTest(rotation=rotation):
                converted, roll = ccc.convert_camera_rotation(rotation)
                # Old forward R * +Z is new forward R' * -Z, to the rounding.
                self.assertLess(distance(ccc.rotate(converted, (0, 0, -1)), ccc.rotate(rotation, (0, 0, 1))), 1e-6)
                # Upright: its right is horizontal and its up points up.
                self.assertLess(abs(ccc.rotate(converted, (1, 0, 0))[1]), 1e-6)
                self.assertGreater(ccc.rotate(converted, (0, 1, 0))[1], 0.0)
                self.assertGreaterEqual(converted[3], 0.0)
                self.assertFalse(roll)

    def test_a_roll_is_dropped_and_reported(self) -> None:
        for rolled in (ccc.multiply(axis_angle((0, 1, 0), 30), axis_angle((0, 0, 1), 20)),
                       axis_angle((0, 0, 1), 180)):
            with self.subTest(rotation=rolled):
                converted, roll = ccc.convert_camera_rotation(rolled)
                self.assertTrue(roll)
                self.assertLess(distance(ccc.rotate(converted, (0, 0, -1)), ccc.rotate(rolled, (0, 0, 1))), 1e-6)
                self.assertLess(abs(ccc.rotate(converted, (1, 0, 0))[1]), 1e-6)

    def test_looking_straight_up_or_down_keeps_the_rotations_own_up(self) -> None:
        for vertical in (axis_angle((1, 0, 0), -90), axis_angle((1, 0, 0), 90),
                         ccc.multiply(axis_angle((0, 1, 0), 40), axis_angle((1, 0, 0), -90))):
            with self.subTest(rotation=vertical):
                converted, roll = ccc.convert_camera_rotation(vertical)
                self.assertFalse(roll)
                self.assertLess(distance(ccc.rotate(converted, (0, 0, -1)), ccc.rotate(vertical, (0, 0, 1))), 1e-6)
                self.assertLess(distance(ccc.rotate(converted, (0, 1, 0)), ccc.rotate(vertical, (0, 1, 0))), 1e-6)

    def test_a_bake_rotation_turns_half_about_its_own_y(self) -> None:
        rotation = ccc.multiply(axis_angle((0, 1, 0), 35), axis_angle((1, 0, 0), 10))
        converted = ccc.convert_seq_rotation(rotation)
        for axis in ((0, 0, -1), (0, 1, 0)):
            old = (0, 0, 1) if axis == (0, 0, -1) else axis
            self.assertLess(distance(ccc.rotate(converted, axis), ccc.rotate(rotation, old)), 1e-6)


class ControllerAnglesTest(unittest.TestCase):
    def test_orbit_angles_offset_the_camera_the_same_way(self) -> None:
        oblique = ccc.normalized((0.3, 0.9, 0.1))
        for up in ((0.0, 1.0, 0.0), (0.0, -1.0, 0.0), oblique):
            for yaw, pitch in ((0.0, 0.0), (0.6, 0.25), (-2.1, -0.7), (3.0, 1.2)):
                with self.subTest(up=up, yaw=yaw, pitch=pitch):
                    after = ccc.convert_controller_angles(yaw, pitch, up)
                    self.assertLess(distance(ccc.controller_direction(yaw, pitch, up),
                                             ccc.controller_direction(*after)), 1e-9)

    def test_the_closed_forms_for_the_shipped_ups(self) -> None:
        self.assertEqual(ccc.convert_controller_angles(0.6, 0.25, (0, 1, 0)), (-0.6, 0.25))
        self.assertEqual(ccc.convert_controller_angles(0.6, 0.25, (0, -1, 0)), (0.6, -0.25))

    def test_the_frames_are_the_engines(self) -> None:
        # cameracontrollersystem.cpp: yaw 0 / pitch 0 is +Z; yaw turns it
        # toward right = cross(forward, up) = -X now, cross(up, forward) = +X
        # before with up +Y.
        self.assertEqual(ccc.yaw_pitch_frame(None), ((0.0, 1.0, 0.0), (0.0, 0.0, 1.0), (-1.0, 0.0, 0.0)))
        self.assertEqual(ccc.yaw_pitch_frame((0.0, 1.0, 0.0))[2], (1.0, 0.0, 0.0))
        self.assertEqual(ccc.yaw_pitch_frame((0.0, -1.0, 0.0))[2], (-1.0, 0.0, 0.0))


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.encode("utf-8"))


def make_project(root: Path, up) -> dict[str, str]:
    camera = '{"yfov": 0.8, "zfar": 100.0, "up": %s}' % json.dumps(up) if up is not None else '{"yfov": 0.8}'
    files = {
        "project.json": '{\n  "schema": "pelican.project",\n  "version": 1,\n'
                        '  "basic_config": {"camera": %s, "scene_data_json": "scene.json"}\n}\n' % camera,
        "scene.json": (
            '{\n  "schema": "pelican.scene",\n  "version": 2,\n  "scenes": {"main": {"objects": [\n'
            '    {"name": "Orbit", "components": [\n'
            '      {"name": "transform", "pos": [0, 0, -3]},\n'
            '      {"name": "camera", "controller": {"type": "orbit", "distance": 3, '
            '"yaw_degrees": 30, "pitch": 0.25, "up": [0, 1, 0]}}\n'
            '    ]},\n'
            '    {"name": "Rolled", "components": [\n'
            '      {"name": "transform", "pos": [1, 2, 3], "rotation": [0, 0, 1, 0]},\n'
            '      {"name": "camera"}\n'
            '    ]},\n'
            '    {"name": "Box", "components": [{"name": "transform", "rotation": [0, 0.6, 0, 0.8]}]}\n'
            '  ]}}\n}\n'),
        "props.scene.json": '{"schema": "pelican.scene", "version": 2, "scenes": {"p": {"objects": [\n'
                            '  {"components": [{"name": "transform", "rotation": [0, 0.6, 0, 0.8]}]}]}}}\n',
        "prefabs/eye.json": '{"schema": "pelican.prefab", "version": 1, "name": "eye", '
                            '"components": [{"name": "camera"}]}\n',
        "bake/camera.jsonl": '{"fps":30.0,"generator":"pelican2-camera-bake","objects":["Cam"],'
                             '"schema":"pelican.transform_seq","version":1}\n'
                             '{"t":0.0,"transforms":[{"pos":[0.0,1.0,2.0],"rot":[0.0,0.0,0.0,1.0],'
                             '"scale":[1.0,1.0,1.0]}]}\n',
        "bake/houdini.jsonl": '{"fps":30.0,"objects":["Rock"],"schema":"pelican.transform_seq","version":1}\n'
                              '{"t":0.0,"transforms":[{"pos":[0,0,0],"rot":[0,0,0,1],"scale":[1,1,1]}]}\n',
    }
    for name, text in files.items():
        write(root / name, text)
    return files


def run_main(argv) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = ccc.main(argv)
    return code, out.getvalue(), err.getvalue()


class ConvertProjectTest(unittest.TestCase):
    def test_a_project_is_converted_in_place_value_by_value(self) -> None:
        for up, yaw_after, pitch_after in (([0, 1, 0], -30, 0.25), ([0.0, -1.0, 0.0], 30, -0.25)):
            with self.subTest(up=up), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                before = make_project(root, up)

                code, out, _ = run_main(["--dry-run", str(root)])
                self.assertEqual(code, 0)
                for name, text in before.items():
                    self.assertEqual((root / name).read_text(encoding="utf-8"), text)
                dry = json.loads(out)["projects"][0]
                self.assertEqual(sorted(dry["changed"]),
                                 ["bake/camera.jsonl", "project.json", "scene.json"])

                code, out, _ = run_main([str(root)])
                self.assertEqual(code, 0)
                report = json.loads(out)["projects"][0]
                self.assertEqual(report["camera_roll_dropped"],
                                 [{"path": "scene.json", "scene": "main", "object": "Rolled"}])
                self.assertEqual(report["camera_prefab_instances"], [{"path": "prefabs/eye.json", "prefab": "eye"}])
                self.assertEqual(report["transform_seq_not_camera_bake"], ["bake/houdini.jsonl"])

                project = json.loads((root / "project.json").read_text(encoding="utf-8"))
                self.assertEqual(project["basic_config"]["camera"], {"yfov": 0.8, "zfar": 100.0})
                self.assertEqual(project["version"], 1)

                scene_text = (root / "scene.json").read_text(encoding="utf-8")
                scene = json.loads(scene_text)
                self.assertEqual(scene["version"], 2)
                objects = scene["scenes"]["main"]["objects"]
                orbit = objects[0]["components"]
                self.assertEqual(orbit[0], {"name": "transform", "pos": [0, 0, -3], "rotation": [0, 1, 0, 0]})
                self.assertEqual(orbit[1]["controller"],
                                 {"type": "orbit", "distance": 3, "yaw_degrees": yaw_after, "pitch": pitch_after})
                self.assertEqual(objects[1]["components"][0]["rotation"], [0, 1, 0, 0])
                # Only the converted values moved: the rest keeps its bytes.
                self.assertIn('{"name": "transform", "pos": [1, 2, 3], "rotation": [0, 1, 0, 0]}', scene_text)
                self.assertIn('{"name": "Box", "components": [{"name": "transform", "rotation": [0, 0.6, 0, 0.8]}]}',
                              scene_text)
                # A document without a camera is not touched at all.
                self.assertEqual((root / "props.scene.json").read_text(encoding="utf-8"), before["props.scene.json"])
                self.assertEqual((root / "bake/houdini.jsonl").read_text(encoding="utf-8"),
                                 before["bake/houdini.jsonl"])
                self.assertEqual((root / "prefabs/eye.json").read_text(encoding="utf-8"), before["prefabs/eye.json"])

                bake = (root / "bake/camera.jsonl").read_text(encoding="utf-8").splitlines()
                self.assertEqual(json.loads(bake[1])["transforms"][0]["rot"], [0.0, 1.0, 0.0, 0.0])

                # Converted once: a second run refuses, and writes nothing.
                converted = {name: (root / name).read_text(encoding="utf-8") for name in before}
                code, _, err = run_main([str(root)])
                self.assertEqual(code, 1)
                self.assertIn("no basic_config.camera.up", err)
                for name, text in converted.items():
                    self.assertEqual((root / name).read_text(encoding="utf-8"), text)

    def test_a_project_without_the_key_converts_with_an_assumed_up(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_project(root, None)
            code, out, _ = run_main(["--assume-up", "0,-1,0", str(root)])
            self.assertEqual(code, 0)
            self.assertEqual(json.loads(out)["projects"][0]["old_up"], [0.0, -1.0, 0.0])
            controller = json.loads((root / "scene.json").read_text(encoding="utf-8"))["scenes"]["main"][
                "objects"][0]["components"][1]["controller"]
            self.assertEqual(controller["pitch"], -0.25)


if __name__ == "__main__":
    unittest.main()
