from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))

import studio_drive  # noqa: E402


class StudioDriveShutdownTest(unittest.TestCase):
    def run_scenario(
        self, exit_code: int | None, *, stopped_code: int = 0,
        watchdog_trip: str | None = None,
    ) -> tuple[int, dict, mock.Mock]:
        # Exercise the CLI verdict and saved report, replacing only the native
        # process/pipe so these regressions also run without Qt or a desktop.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            scenario = root / "scenario.json"
            scenario.write_text('{"name":"shutdown","steps":[]}', encoding="utf-8")
            report_path = root / "report.json"
            studio = mock.Mock(spec=studio_drive.Studio)
            studio.wait_for_line.return_value = "automation listening: fixture"
            studio.wait_for_exit.return_value = exit_code
            studio.stop.return_value = stopped_code
            channel = mock.Mock(spec=studio_drive.Channel)
            channel.send.return_value = {"ok": True}
            watchdog = mock.Mock(spec=studio_drive.Watchdog)
            watchdog.tripped = watchdog_trip
            argv = [
                "studio_drive.py", "--studio", str(root / "studio.exe"),
                "--scenario", str(scenario), "--project", str(root),
                "--report", str(report_path),
            ]
            with (
                mock.patch.object(sys, "argv", argv),
                mock.patch.object(
                    studio_drive, "Studio", return_value=studio
                ) as studio_class,
                mock.patch.object(studio_drive, "Watchdog", return_value=watchdog),
                mock.patch.object(studio_drive.Channel, "connect", return_value=channel),
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                result = studio_drive.main()
            # The environment Studio was started in (Studio(command, log, env)).
            self.launch_env = studio_class.call_args.args[2]
            channel.close.assert_called_once()
            return result, json.loads(report_path.read_text(encoding="utf-8")), studio

    def test_clean_exit_keeps_the_report_green(self) -> None:
        result, report, studio = self.run_scenario(0)
        self.assertEqual(result, 0)
        self.assertTrue(report["ok"])
        self.assertEqual(report["verdict"], "green")
        self.assertEqual(report["failures"], [])
        self.assertEqual(report["studio_exit_code"], 0)
        self.assertFalse(report["studio_shutdown_forced"])
        self.assertEqual(report["steps_run"], report["steps_planned"])
        self.assertEqual(report["steps"][-1]["kind"], "shutdown")
        studio.stop.assert_not_called()

    def test_nonzero_exit_fails_even_when_scenario_steps_pass(self) -> None:
        for code in (1, 0xC0000005):
            with self.subTest(code=code):
                result, report, studio = self.run_scenario(code)
                self.assert_shutdown_failure(result, report, code, forced=False)
                self.assertIn(str(code), report["failures"][0]["reason"])
                studio.stop.assert_not_called()

    def test_exit_timeout_fails_even_if_forced_stop_returns_zero(self) -> None:
        result, report, studio = self.run_scenario(None, stopped_code=0)
        self.assert_shutdown_failure(result, report, 0, forced=True)
        self.assertIn("did not exit within 30 seconds", report["failures"][0]["reason"])
        studio.stop.assert_called_once()

    def test_watchdog_termination_cannot_report_a_clean_shutdown(self) -> None:
        result, report, _ = self.run_scenario(0, watchdog_trip="quit (id 1)")
        self.assert_shutdown_failure(result, report, 0, forced=True)
        self.assertIn("watchdog forced", report["failures"][0]["reason"])
        self.assertIn("quit (id 1)", report["failures"][0]["reason"])

    def assert_shutdown_failure(
        self, result: int, report: dict, code: int, *, forced: bool,
    ) -> None:
        self.assertEqual(result, 1)
        self.assertFalse(report["ok"])
        self.assertEqual(report["verdict"], "red")
        self.assertEqual(report["failed_steps"], 1)
        self.assertEqual(report["failures"][0]["kind"], "shutdown")
        self.assertEqual(report["studio_exit_code"], code)
        self.assertEqual(report["studio_shutdown_forced"], forced)
        self.assertNotIn("studio_exit_code_note", report)



class StudioDriveAudioBackendTest(unittest.TestCase):
    """Q-11: a scenario runs silent unless its caller chose a backend."""

    run_scenario = StudioDriveShutdownTest.run_scenario

    def launch_env_with(self, value: str | None) -> dict[str, str]:
        with mock.patch.dict(studio_drive.os.environ):
            studio_drive.os.environ.pop("PELICAN_AUDIO_BACKEND", None)
            if value is not None:
                studio_drive.os.environ["PELICAN_AUDIO_BACKEND"] = value
            result, _, _ = self.run_scenario(0)
        self.assertEqual(result, 0)
        return self.launch_env

    def test_studio_starts_with_the_null_backend(self) -> None:
        self.assertEqual(self.launch_env_with(None)["PELICAN_AUDIO_BACKEND"], "null")

    def test_an_empty_value_is_unset_and_gets_null(self) -> None:
        self.assertEqual(self.launch_env_with("")["PELICAN_AUDIO_BACKEND"], "null")

    def test_a_backend_the_caller_chose_is_kept(self) -> None:
        for chosen in ("device", "offline"):
            with self.subTest(chosen=chosen):
                self.assertEqual(
                    self.launch_env_with(chosen)["PELICAN_AUDIO_BACKEND"], chosen
                )


class ResultRetryTest(unittest.TestCase):
    """F-5: a `result` step with `retry_ms` asks again until satisfied."""

    class FakeRun:
        def __init__(self, readings: list[int]) -> None:
            self.readings = list(readings)
            self.calls = 0
            self.remembered: dict = {}
            self.steps: list = []
            self.records: list[dict] = []

        def call(self, request: dict, timeout: float) -> dict:
            self.calls += 1
            value = self.readings.pop(0) if len(self.readings) > 1 else self.readings[0]
            return {"ok": True, "result": {"distinct_colors": value}}

        def record(self, *args, **kwargs) -> None:
            self.records.append(kwargs)

        def capture(self, name: str) -> None:
            return None

    step = {
        "expect": "result",
        "command": {"cmd": "screenshot", "mode": "screen"},
        "path": "distinct_colors",
        "op": "gt",
        "value": 100,
    }

    def test_without_retry_one_reading_decides(self) -> None:
        run = self.FakeRun([1, 1, 500])
        with mock.patch.object(studio_drive.time, "sleep"):
            self.assertFalse(studio_drive.run_expect_step(run, dict(self.step), "x"))
        self.assertEqual(run.calls, 1)

    def test_retry_reads_until_the_answer_moves(self) -> None:
        run = self.FakeRun([1, 1, 500])
        with mock.patch.object(studio_drive.time, "sleep"):
            self.assertTrue(studio_drive.run_expect_step(
                run, dict(self.step, retry_ms=60000), "x"))
        self.assertEqual(run.calls, 3)

    def test_retry_gives_up_when_the_time_is_up(self) -> None:
        run = self.FakeRun([1])
        clock = iter([0.0, 0.0, 0.5, 1.0, 2.0, 3.0, 4.0])
        with (
            mock.patch.object(studio_drive.time, "sleep"),
            mock.patch.object(studio_drive.time, "monotonic", lambda: next(clock)),
        ):
            self.assertFalse(studio_drive.run_expect_step(
                run, dict(self.step, retry_ms=1000), "x"))
        self.assertGreater(run.calls, 1)


class StudioDriveFileStepTest(unittest.TestCase):
    # F-4: a project made by File > New Project lives under the run's
    # artifacts, not in the project copy, and a file step has to reach it.
    # Anything outside the directories the run owns is still refused.
    def test_file_steps_reach_the_project_copy_and_the_artifacts_only(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = root / "run" / "project"
            artifacts = root / "run" / "artifacts"
            outside = root / "elsewhere"
            for folder in (project, artifacts, outside):
                folder.mkdir(parents=True)
            run = studio_drive.Run({"name": "files"}, artifacts)
            roots = [project, artifacts]
            with contextlib.redirect_stdout(io.StringIO()):
                for target, allowed in (
                    (project / "a.txt", True),
                    (artifacts / "new_project" / "shaders" / "b.frag", True),
                    (outside / "c.txt", False),
                    (artifacts / ".." / "elsewhere_too.txt", False),
                ):
                    with self.subTest(target=str(target)):
                        ok = studio_drive.run_file_step(
                            run,
                            {"file": "write", "path": str(target), "text": "x"},
                            "write",
                            roots,
                        )
                        self.assertEqual(ok, allowed)
                        self.assertEqual(target.resolve().is_file(), allowed)


class StudioDriveCompareTest(unittest.TestCase):
    # F-7: "this message never appeared" is a question about a text, and
    # `excludes` is the answer `contains` gives turned round.
    def test_excludes_is_contains_turned_round(self) -> None:
        log = "Opened x.\n[Studio gizmo] Modal transform polling failed: gone\n"
        self.assertTrue(studio_drive.compare("contains", log, "polling failed"))
        self.assertFalse(studio_drive.compare("excludes", log, "polling failed"))
        self.assertTrue(studio_drive.compare("excludes", "Opened x.\n", "polling failed"))


if __name__ == "__main__":
    unittest.main()
