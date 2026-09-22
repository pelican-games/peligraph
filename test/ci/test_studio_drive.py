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
                mock.patch.object(studio_drive, "Studio", return_value=studio),
                mock.patch.object(studio_drive, "Watchdog", return_value=watchdog),
                mock.patch.object(studio_drive.Channel, "connect", return_value=channel),
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                result = studio_drive.main()
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


if __name__ == "__main__":
    unittest.main()
