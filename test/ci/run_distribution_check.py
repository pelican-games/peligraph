#!/usr/bin/env python3
"""Check a Runtime install the way a user meets it.

For every shipped sample under projects/ (every directory with a
project.json), Studio from the install opens a copy of the sample, captures a
frame, switches to a control project and back, captures again, and quits with
exit code 0. Then a project made with `pelican_cli project init` is edited
through the editor RPC, saved from the File menu, and read back after a full
Studio restart.

The install is used alone: PATH holds only its bin directory and System32, so
nothing from a developer build or the Qt SDK can stand in for a missing file.

Each Studio run starts its player with a per-project log
(<project>/.pelican/logs/player.log). The check asserts that the file exists,
and that the player still running when Studio quits did not keep re-creating
a surface for its destroyed window (once dozens of ErrorSurfaceLostKHR lines
until Studio killed it).

Run it under the GPU lease launcher when other GPU work may run at the same
time:

    <build>/test/Release/pelican_gpu_lease_run.exe -- \
        python -B test/ci/run_distribution_check.py \
        --stage <install prefix> --artifacts <dir>
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from studio_drive import Channel, Studio, Watchdog  # noqa: E402

PLAYER_LOG = Path(".pelican/logs/player.log")
SURFACE_LOST = "ErrorSurfaceLostKHR"
# A player may lose its surface once or twice between the host window going
# away and its main loop noticing. A loop that re-creates the surface every
# frame until it is killed shows up as dozens.
MAX_TEARDOWN_SURFACE_LOSSES = 3


def stage_environment(stage: Path) -> dict[str, str]:
    env = {key.upper(): value for key, value in os.environ.items()}
    env.pop("PELICAN_STUDIO_PLAYER", None)
    env["PATH"] = str(stage / "bin") + ";" + os.environ["SystemRoot"] + "/System32"
    return env


def shipped_samples(projects_dir: Path) -> list[str]:
    return sorted(
        entry.name
        for entry in projects_dir.iterdir()
        if entry.is_dir() and (entry / "project.json").is_file()
    )


class Session:
    """One Studio process driven over its automation pipe."""

    def __init__(self, stage: Path, project: Path, log: Path, pipe: str, env: dict[str, str]):
        self.studio = Studio(
            [str(stage / "bin/pelican_studio.exe"), "--automation", pipe, "--project", str(project)],
            log,
            env,
        )
        self.watchdog = Watchdog()
        self.watchdog.on_trip = self.studio.stop
        self.watchdog.start()
        self.channel = None
        assert self.studio.wait_for_line("automation listening:", 30), "no automation listener"
        self.channel = Channel.connect(pipe, time.monotonic() + 30, self.watchdog)

    def command(self, **request):
        reply = self.channel.send(request, timeout=75)
        assert reply.get("ok"), reply
        return reply["result"]

    def rpc(self, method: str, **params):
        reply = self.command(cmd="rpc", method=method, params=params)
        assert "error" not in reply, reply
        return reply["result"]

    def wait_first_frame(self) -> None:
        # The engine answers RPC before it publishes its first frame; capture
        # and the frame plan need a published one.
        self.command(cmd="wait_engine", timeout_ms=60000, frames=1)

    def capture(self, path: Path) -> None:
        # Studio lays its window out after a project opens, and each resize
        # gives the engine a new output whose first frame is still to come.
        # capture names those states; they mean "not yet", not failure.
        deadline = time.monotonic() + 20
        while True:
            try:
                self.rpc("capture", path=str(path))
                return
            except AssertionError as error:
                text = repr(error)
                if time.monotonic() > deadline or not (
                        "window output is unavailable" in text or "no rendered frame" in text):
                    raise
                time.sleep(0.1)

    def frame_plan_status(self) -> str:
        # Studio asks for the frame plan as soon as the engine answers RPC,
        # which for a ray-tracing project can be before its first generation
        # is published. The dock must settle on the plan, not on a failure.
        deadline = time.monotonic() + 20
        while True:
            text = self.command(cmd="text", name="pelican.framePlanStatus").get("text", "")
            settling = ("Requesting" in text or "Waiting for the engine" in text)
            if not settling or time.monotonic() > deadline:
                return text
            time.sleep(0.1)

    def quit(self) -> int:
        self.command(cmd="quit")
        self.channel.close()
        self.channel = None
        return self.studio.wait_for_exit(15)

    def close(self) -> None:
        if self.channel:
            self.channel.close()
        self.studio.stop()


def check_sample(stage: Path, sample: str, projects_dir: Path, control_fixture: Path,
                 out: Path, env: dict[str, str]) -> dict:
    out.mkdir(parents=True)
    project = out / "project"
    control = out / "control"
    shutil.copytree(projects_dir / sample, project,
                    ignore=shutil.ignore_patterns(".pelican", "build", "*.log"))
    shutil.copytree(control_fixture, control)
    result: dict = {"sample": sample, "artifact": str(out), "passed": False}
    session = None
    try:
        session = Session(stage, project, out / "studio.log",
                          f"pelican_distribution_{sample}_{os.getpid()}", env)
        session.wait_first_frame()
        tree = session.rpc("scene_tree")
        result["scene"] = tree["scene_id"]
        result["objects"] = len(tree["objects"])
        assert tree["objects"], tree
        plan = session.rpc("get_frame_plan")
        result["frame_plan_nodes"] = len(plan["nodes"])
        assert plan["nodes"], plan
        session.capture(out / "viewport.png")
        assert (out / "viewport.png").stat().st_size > 100, "no frame capture"
        # Switch away first: opening the current project is deliberately a no-op.
        session.command(cmd="open_project", dir=str(control))
        session.wait_first_frame()
        assert session.rpc("scene_tree")["scene_id"] == "default_scene"
        session.command(cmd="open_project", dir=str(project))
        session.wait_first_frame()
        assert session.rpc("scene_tree")["scene_id"] == tree["scene_id"]
        session.capture(out / "reloaded.png")
        status = session.frame_plan_status()
        result["frame_plan_status"] = status
        assert "failed" not in status and "unavailable" not in status, (
            f"Frame Plan dock settled on: {status}")
        code = session.quit()
        session.close()
        session = None
        result["exit_code"] = code
        assert code == 0, f"abnormal Studio exit: {code}"
        for name, directory in (("project", project), ("control", control)):
            log = directory / PLAYER_LOG
            assert log.is_file(), f"no per-project player log at {log}"
        # The reopened project's player was still running when Studio quit.
        losses = (project / PLAYER_LOG).read_text(encoding="utf-8", errors="replace").count(SURFACE_LOST)
        result["quit_player_surface_losses"] = losses
        assert losses <= MAX_TEARDOWN_SURFACE_LOSSES, (
            f"the player lost its surface {losses} times while Studio quit")
        result["passed"] = True
    except Exception as error:  # noqa: BLE001 - the report names every failure
        result["error"] = repr(error)
    finally:
        if session:
            session.close()
    return result


def check_new_project(stage: Path, out: Path, env: dict[str, str]) -> dict:
    out.mkdir(parents=True)
    project = out / "project"
    result: dict = {"sample": "<pelican_cli project init>", "artifact": str(out), "passed": False}
    try:
        subprocess.run([str(stage / "bin/pelican_cli.exe"), "project", "init", str(project)],
                       env=env, check=True)
        scene = project / "scenes/main.scene.json"
        for cycle in ("edit-save", "restart"):
            session = Session(stage, project, out / f"{cycle}.log",
                              f"pelican_distribution_new_{cycle}_{os.getpid()}", env)
            try:
                session.wait_first_frame()
                if cycle == "edit-save":
                    actor = session.rpc("open_editor_session", display_name="Distribution check")
                    revision = session.rpc("get_scene_revision")["scene_revision"]
                    edit = session.rpc(
                        "edit", actor_id=actor["actor_id"], base_revision=revision,
                        operations=[dict(op="set_component_value", object_id=1,
                                         component_slot="transform", field_path="/pos",
                                         value=[1, 2, -4])])
                    deadline = time.monotonic() + 15
                    while True:
                        outcome = session.rpc("get_edit_result", ticket=edit["ticket"])
                        if outcome.get("status") not in ("pending", "accepted"):
                            break
                        assert time.monotonic() < deadline, outcome
                        time.sleep(0.03)
                    assert outcome["status"] == "committed", outcome
                    session.command(cmd="action", name="pelican.file.saveScene")
                    deadline = time.monotonic() + 15
                    while True:
                        saved = json.loads(scene.read_text(encoding="utf-8"))
                        camera = saved["scenes"]["default_scene"]["objects"][0]
                        if camera["components"][0]["pos"] == [1, 2, -4]:
                            break
                        assert time.monotonic() < deadline, "Save Scene did not persist the edit"
                        time.sleep(0.05)
                    assert camera["authoring_object_id"] == 1
                else:
                    snapshot = session.rpc("get_components", scene_id="default_scene",
                                           authoring_object_id=1)
                    (out / "restarted-scene.json").write_text(json.dumps(snapshot, indent=2),
                                                              encoding="utf-8")
                    transform = next(c for c in snapshot["components"] if c["name"] == "transform")
                    assert transform["authored_json"]["pos"] == [1, 2, -4], transform
                    assert transform["runtime_json"]["world_trs"]["pos"] == [1, 2, -4], transform
                # The same-frame redraw path once duplicated GPU timing samples.
                for _ in range(5):
                    session.rpc("render_frame")
                session.capture(out / f"{cycle}.png")
                code = session.quit()
                assert code == 0, f"abnormal Studio exit after {cycle}: {code}"
            finally:
                session.close()
        result["passed"] = True
    except Exception as error:  # noqa: BLE001
        result["error"] = repr(error)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--stage", required=True, type=Path,
                        help="the Runtime install prefix (contains bin/pelican_studio.exe)")
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--projects-dir", type=Path, default=ROOT / "projects")
    parser.add_argument("--control-fixture", type=Path,
                        default=ROOT / "test/fixtures/studio_automation_graph")
    parser.add_argument("--sample", action="append", default=[],
                        help="check only these samples (repeatable); default: every shipped sample")
    parser.add_argument("--skip-new-project", action="store_true")
    arguments = parser.parse_args()

    stage = arguments.stage.resolve()
    if not (stage / "bin/pelican_studio.exe").is_file():
        print(f"error: {stage} is not a Runtime install with Studio", file=sys.stderr)
        return 2
    run_dir = arguments.artifacts.resolve() / f"run-{time.strftime('%Y%m%d-%H%M%S')}-{os.getpid()}"
    run_dir.mkdir(parents=True)
    env = stage_environment(stage)
    samples = arguments.sample or shipped_samples(arguments.projects_dir)
    results = []
    for sample in samples:
        result = check_sample(stage, sample, arguments.projects_dir, arguments.control_fixture,
                              run_dir / sample, env)
        results.append(result)
        print(("PASS: " if result["passed"] else "FAIL: ") + sample + " " + json.dumps(result),
              flush=True)
    if not arguments.skip_new_project:
        result = check_new_project(stage, run_dir / "new-project", env)
        results.append(result)
        print(("PASS: " if result["passed"] else "FAIL: ") + "new project " + json.dumps(result),
              flush=True)
    (run_dir / "report.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    passed = sum(1 for result in results if result["passed"])
    print(f"distribution check: {passed} / {len(results)} passed; report {run_dir / 'report.json'}",
          flush=True)
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
