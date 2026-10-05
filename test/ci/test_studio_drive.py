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


class StudioDriveTimingTest(unittest.TestCase):
    def test_short_command_keeps_submillisecond_timing_on_performance_clock(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            run = studio_drive.Run({"name": "timing"}, Path(directory))
            run.channel = mock.Mock(spec=studio_drive.Channel)
            run.channel.send.return_value = {"ok": True, "result": {}}
            with (
                mock.patch.object(studio_drive.time, "perf_counter", side_effect=[10.0, 10.00025]),
                mock.patch.object(studio_drive.time, "monotonic", return_value=100000.0),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertTrue(studio_drive.run_command_step(run, {"cmd": "text", "name": "x"}, "short"))
            measured = run.steps[0]
            self.assertEqual(measured["duration_ms"], 0)
            self.assertAlmostEqual(measured["duration_precise_ms"], 0.25)
            self.assertAlmostEqual(
                (measured["finished_perf_s"] - measured["started_perf_s"]) * 1000, 0.25)


def answer_viewport_size(request: dict, timeout: float) -> dict:
    """What a Studio answers: the size it was asked for, anything else ok."""

    if request.get("cmd") == "viewport_size":
        return {"ok": True, "result": {"width": request["width"], "height": request["height"]}}
    return {"ok": True}


class StudioDriveViewportTest(unittest.TestCase):
    """PX-2: every run holds the viewport at one size before its first step."""

    def fix(self, scenario: dict, answer=answer_viewport_size) -> tuple[bool, mock.Mock, list]:
        with tempfile.TemporaryDirectory() as directory:
            run = studio_drive.Run(scenario, Path(directory))
            run.channel = mock.Mock(spec=studio_drive.Channel)
            run.channel.send.side_effect = answer
            with contextlib.redirect_stdout(io.StringIO()):
                held = studio_drive.fix_viewport(run, scenario)
            return held, run.channel, run.failures

    def test_the_default_size_is_asked_for(self) -> None:
        held, channel, failures = self.fix({"name": "default"})
        self.assertTrue(held)
        self.assertEqual(failures, [])
        request = channel.send.call_args.args[0]
        self.assertEqual(request["cmd"], "viewport_size")
        self.assertEqual((request["width"], request["height"]), studio_drive.DEFAULT_VIEWPORT)

    def test_a_scenario_names_its_own_size_or_none(self) -> None:
        held, channel, _ = self.fix({"name": "own", "viewport": [640, 400]})
        self.assertTrue(held)
        self.assertEqual(channel.send.call_args.args[0]["width"], 640)
        held, channel, _ = self.fix({"name": "free", "viewport": None})
        self.assertTrue(held)
        channel.send.assert_not_called()

    def test_a_size_that_does_not_take_stops_the_run(self) -> None:
        def short(request: dict, timeout: float) -> dict:
            return {"ok": True, "result": {"width": request["width"], "height": request["height"] - 40}}

        held, _, failures = self.fix({"name": "short"}, short)
        self.assertFalse(held)
        self.assertEqual(failures[0]["kind"], "viewport")
        held, _, failures = self.fix({"name": "bad", "viewport": [720]})
        self.assertFalse(held)
        held, _, failures = self.fix(
            {"name": "refused"},
            lambda request, timeout: {"ok": False, "error": {"code": "automation.wait_timeout@1", "message": "no"}},
        )
        self.assertFalse(held)
        self.assertIn("wait_timeout", failures[0]["reason"])


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
            channel.send.side_effect = answer_viewport_size
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


import studio_invariants  # noqa: E402


class ScriptedRun:
    """A Run whose channel answers from a function of the request."""

    def __init__(self, answer, scenario: dict | None = None) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.run = studio_drive.Run(scenario or {"name": "rules"}, Path(self.directory.name))
        self.run.channel = mock.Mock(spec=studio_drive.Channel)
        self.run.channel.send.side_effect = answer
        self.run.capture = lambda stem: None

    def expect(self, step: dict) -> bool:
        with contextlib.redirect_stdout(io.StringIO()):
            return studio_drive.run_expect_step(self.run, step, step.get("label", "x"))

    def close(self) -> None:
        self.directory.cleanup()


def answering(value):
    return lambda request, timeout: {"ok": True, "result": {"value": value}}


class QD1CompareRulesTest(unittest.TestCase):
    """QD-1, GL-5g's rules 1 and 3: the comparisons that could not fail now do."""

    def test_contains_and_excludes_on_an_object_are_refused(self) -> None:
        for op in ("contains", "excludes"):
            with self.subTest(op=op), self.assertRaises(studio_drive.StepMisuse):
                studio_drive.compare(op, {"fragment": "x"}, "PELICAN_PASSTHROUGH_INPUT")

    def test_has_key_and_lacks_key_ask_about_a_key(self) -> None:
        self.assertTrue(studio_drive.compare("has_key", {"defines": []}, "defines"))
        self.assertTrue(studio_drive.compare("lacks_key", {"fragment": "x"}, "defines"))
        self.assertFalse(studio_drive.compare("lacks_key", {"defines": []}, "defines"))
        with self.assertRaises(studio_drive.StepMisuse):
            studio_drive.compare("has_key", ["defines"], "defines")

    def test_excludes_from_a_list_needs_a_value_of_its_kind(self) -> None:
        with self.assertRaises(studio_drive.StepMisuse):
            studio_drive.compare("excludes", [1, 2], "1")
        self.assertTrue(studio_drive.compare("excludes", ["a", "b"], "c"))
        self.assertFalse(studio_drive.compare("contains", [1, 2], True))
        with self.assertRaises(studio_drive.StepMisuse):
            studio_drive.compare("contains", "anything", "")

    def test_a_bool_is_not_a_number(self) -> None:
        self.assertFalse(studio_drive.equal(True, 1))
        self.assertFalse(studio_drive.equal([True], [1]))
        self.assertTrue(studio_drive.equal(1, 1.0))
        self.assertTrue(studio_drive.equal({"a": [1]}, {"a": [1.0]}))

    def test_ne_between_kinds_is_refused_but_null_is_a_value(self) -> None:
        with self.assertRaises(studio_drive.StepMisuse):
            studio_drive.compare("ne", "156", 156)
        self.assertTrue(studio_drive.compare("ne", None, 156))
        self.assertTrue(studio_drive.compare("ne", 156, 288.9))

    def test_a_refused_comparison_fails_the_step_by_name(self) -> None:
        scripted = ScriptedRun(answering({"fragment": "x"}))
        try:
            ok = scripted.expect({"expect": "result", "command": {"cmd": "text"},
                                  "path": "value", "op": "excludes", "value": "y"})
            self.assertFalse(ok)
            self.assertIn("has_key", scripted.run.failures[0]["reason"])
        finally:
            scripted.close()


class QD1ReadingRulesTest(unittest.TestCase):
    """QD-1, GL-5g's rules 2, 4, 5, 7 and 8."""

    def test_a_reading_that_neither_compares_nor_remembers_fails(self) -> None:
        scripted = ScriptedRun(answering(3))
        try:
            self.assertFalse(scripted.expect({"expect": "result", "command": {"cmd": "text"}, "path": "value"}))
            self.assertIn("record: true", scripted.run.failures[0]["reason"])
            self.assertTrue(scripted.expect({"expect": "result", "command": {"cmd": "text"},
                                             "path": "value", "record": True}))
        finally:
            scripted.close()

    def test_a_name_remembered_and_never_compared_is_a_warning(self) -> None:
        scripted = ScriptedRun(answering(3))
        try:
            run = scripted.run
            self.assertTrue(scripted.expect({"expect": "result", "command": {"cmd": "text"},
                                             "path": "value", "remember": "kept", "label": "keep"}))
            self.assertTrue(scripted.expect({"expect": "result", "command": {"cmd": "text"},
                                             "path": "value", "remember": "used"}))
            self.assertTrue(scripted.expect({"expect": "result", "command": {"cmd": "text"},
                                             "path": "value", "value_of": "used"}))
            with contextlib.redirect_stdout(io.StringIO()):
                studio_drive.finish_warnings(run)
            self.assertEqual(len(run.warnings), 1)
            self.assertIn("kept", run.warnings[0])
        finally:
            scripted.close()

    def test_stable_for_frames_reads_again_after_the_frames(self) -> None:
        readings = iter([188, 188, 188, 189])

        def answer(request, timeout):
            if request["cmd"] == "rpc":
                return {"ok": True, "result": {"result": {"render": {"published_frames": 400}}}}
            if request["cmd"] == "wait_engine":
                self.assertEqual(request["frames"], 430)
                return {"ok": True, "result": {}}
            return {"ok": True, "result": {"value": next(readings)}}

        scripted = ScriptedRun(answer)
        try:
            step = {"expect": "result", "command": {"cmd": "text"}, "path": "value",
                    "value": 188, "stable_for_frames": 30}
            self.assertTrue(scripted.expect(dict(step)))
            self.assertFalse(scripted.expect(dict(step)))
            self.assertIn("30 frames later", scripted.run.failures[0]["reason"])
        finally:
            scripted.close()

    def test_visible_false_needs_a_name_that_resolves(self) -> None:
        missing = ScriptedRun(lambda request, timeout: {
            "ok": False, "error": {"code": "automation.widget_missing@1", "message": "no"}})
        hidden = ScriptedRun(lambda request, timeout: {"ok": True, "result": {"visible": False}})
        try:
            self.assertFalse(missing.expect({"expect": "visible", "name": "pelican.typo", "value": False}))
            self.assertIn("missing: true", missing.run.failures[0]["reason"])
            self.assertTrue(hidden.expect({"expect": "visible", "name": "pelican.there", "value": False}))
        finally:
            missing.close()
            hidden.close()

    def test_a_count_of_zero_for_a_pattern_never_seen_is_a_warning(self) -> None:
        scripted = ScriptedRun(lambda request, timeout: {"ok": True, "result": {"widgets": []}})
        try:
            self.assertTrue(scripted.expect({"expect": "list_count", "pattern": "pill#lop/", "value": 0}))
            with contextlib.redirect_stdout(io.StringIO()):
                studio_drive.finish_warnings(scripted.run)
            self.assertIn("pill#lop/", scripted.run.warnings[0])
        finally:
            scripted.close()


class QD1FileRulesTest(unittest.TestCase):
    """QD-1, GL-5g's rule 6: `was` for an absence, `list` for a directory."""

    def test_was_needs_the_presence_seen_first(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run = studio_drive.Run({"name": "files"}, root)
            run.capture = lambda stem: None
            target = root / "shader.frag"
            target.write_text("void main() {}", encoding="utf-8")
            step = lambda **fields: dict({"file": "exists", "path": str(target)}, **fields)  # noqa: E731
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertFalse(studio_drive.run_file_step(
                    run, step(path=str(root / "never.frag"), value=False, was=True), "never", [root]))
                self.assertTrue(studio_drive.run_file_step(run, step(), "there", [root]))
                target.unlink()
                self.assertTrue(studio_drive.run_file_step(run, step(value=False, was=True), "gone", [root]))
                self.assertTrue(studio_drive.run_file_step(
                    run, step(path=str(root / "other.frag"), value=False), "absent", [root]))
            self.assertIn("other.frag", run.warnings[0])

    def test_list_names_what_a_directory_holds(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "shaders").mkdir()
            (root / "shaders" / "a.frag").write_text("", encoding="utf-8")
            run = studio_drive.Run({"name": "files"}, root)
            run.capture = lambda stem: None
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertTrue(studio_drive.run_file_step(
                    run, {"file": "list", "path": str(root / "shaders"), "value": ["a.frag"]}, "l", [root]))
                self.assertFalse(studio_drive.run_file_step(
                    run, {"file": "list", "path": str(root / "shaders"), "count": 0}, "l", [root]))


def project_state(files: dict[str, str], render: dict[str, str], pid: int = 7,
                  directory: str = "") -> studio_invariants.ProjectState:
    return studio_invariants.ProjectState({
        "dir": directory, "engine_ready": True, "engine_process_id": pid,
        "files": [{"path": path, "size": 1, "sha256": sha} for path, sha in files.items()],
        "rpc": {"get_render_documents": {"documents": [
            {"reference": reference, "content": content} for reference, content in render.items()]}},
    })


class QD1InvariantTest(unittest.TestCase):
    """QD-1, stage B: the checks after the last step."""

    def test_missing_or_malformed_pixels_cannot_pass_reopen(self) -> None:
        valid = [[10, 20, 30] for _ in studio_invariants.PIXEL_GRID]
        self.assertTrue(studio_invariants.pixels_close(valid, valid))
        for bad in (None, [], [[10, 20]], [[10, 20, 30]] * 8,
                    [[10, 20, True]] * 9, [[10, 20, 300]] * 9):
            with self.subTest(bad=bad):
                self.assertFalse(studio_invariants.pixels_close(valid, bad))
                self.assertFalse(studio_invariants.pixels_close(bad, bad))

    def test_unsettled_documents_are_not_returned_as_saved(self) -> None:
        epilogue = mock.Mock()
        epilogue.state.side_effect = [project_state({"a": "1"}, {}),
                                     project_state({"a": "2"}, {})]
        with (mock.patch.object(studio_invariants.time, "monotonic", side_effect=[0, 0, 1, 3]),
              mock.patch.object(studio_invariants.time, "sleep")):
            self.assertIsNone(studio_invariants.settled_state(epilogue, within_s=2))

    def test_save_failure_cannot_be_hidden_by_a_successful_reopen(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "other").mkdir()
            epilogue = studio_invariants.Epilogue(mock.Mock(), {"name": "save"})
            epilogue.variables = {"other_project": str(root / "other")}
            epilogue.state = lambda: project_state({}, {}, directory=str(root))
            epilogue.call = mock.Mock(return_value={"ok": False, "error": {"message": "save refused"}})
            outcome = studio_invariants.check_reopen(epilogue)
            self.assertEqual(outcome["status"], "red")
            self.assertIn("save refused", outcome["reason"])
            epilogue.call.assert_called_once()

    def test_plain_error_lines_are_health_failures(self) -> None:
        self.assertIsNotNone(studio_invariants.LOG_PATTERN.search("[ERROR] failed to save"))

    def test_an_invariant_is_off_only_with_a_reason_and_known_red_only_with_a_ticket(self) -> None:
        mode = studio_invariants.invariant_mode
        self.assertEqual(mode({}, "undo"), ("on", ""))
        self.assertEqual(mode({"invariants": {"undo": False, "undo_reason": "r"}}, "undo"), ("off", "r"))
        self.assertEqual(mode({"invariants": {"undo": "known_red", "ticket": "QD1-1"}}, "undo"),
                         ("known_red", "QD1-1"))
        for bad in ({"undo": False}, {"undo": "known_red"}, {"undo": "maybe"}):
            with self.subTest(bad=bad), self.assertRaises(studio_invariants.InvariantConfigError):
                mode({"invariants": bad}, "undo")

    def test_a_document_that_differs_is_named_with_its_diff(self) -> None:
        start = project_state({"a.json": "1"}, {"passes/main.json": '{"nodes": []}'})
        end = project_state({"a.json": "2", "b.frag": "3"}, {"passes/main.json": '{"nodes": [1]}'})
        differing, skipped = studio_invariants.diff_states(start, end)
        self.assertEqual([entry["key"] for entry in differing],
                         ["file:a.json", "file:b.frag", "render:passes/main.json"])
        self.assertEqual(differing[1]["change"], "added")
        self.assertTrue(any("+" in line for line in differing[2]["diff"]))
        self.assertEqual(skipped, [])

    def test_a_subtree_left_out_by_design_is_said_not_compared(self) -> None:
        start = project_state({}, {"passes/main.json": '{"nodes": [], "layout": {"frame_plan": {"x": 1}}}'})
        moved = project_state({}, {"passes/main.json": '{"nodes": [], "layout": {"frame_plan": {"x": 2}}}'})
        differing, skipped = studio_invariants.diff_states(
            start, moved, subtrees=list(studio_invariants.DEFAULT_UNDO_EXEMPT))
        self.assertEqual(differing, [])
        self.assertIn("set_layout", skipped[0]["reason"])
        edited = project_state({}, {"passes/main.json": '{"nodes": [1], "layout": {"frame_plan": {"x": 2}}}'})
        differing, _ = studio_invariants.diff_states(
            start, edited, subtrees=list(studio_invariants.DEFAULT_UNDO_EXEMPT))
        self.assertEqual(len(differing), 1)

    def test_the_journal_says_what_each_session_can_still_undo(self) -> None:
        records = [
            {"actor_id": 1, "actor_display_name": "Pelican Studio Inspector", "operation_kind": "edit"},
            {"actor_id": 1, "actor_display_name": "Pelican Studio Inspector", "operation_kind": "edit"},
            {"actor_id": 1, "actor_display_name": "Pelican Studio Inspector", "operation_kind": "undo"},
        ]
        depths = studio_invariants.journal_depths({"records": records})
        self.assertEqual(depths, {"Pelican Studio Inspector (actor 1)": {"undo": 1, "redo": 1}})

    def test_redo_goes_back_the_way_undo_came(self) -> None:
        epilogue = studio_invariants.Epilogue(mock.Mock(), {"name": "x"})
        pressed: list[tuple[str, str]] = []

        def press_once(kind, step, log, notes):
            pressed.append((kind, step))
            return "moved" if len(pressed) <= 3 else "off"

        epilogue.press_once = press_once
        undone = {"sequence": ["graph", "graph", "render", "materials"]}
        studio_invariants.time.monotonic()
        result = epilogue.redo_in_reverse(undone, studio_invariants.time.monotonic() + 60)
        self.assertEqual(pressed[:3], [("render", "redo"), ("graph", "redo"), ("graph", "redo")])
        self.assertEqual(result["sequence"], ["render", "graph", "graph"])

    def test_scene_redo_uses_the_journal_not_an_always_enabled_button(self) -> None:
        kinds = {"scene": {"redo": {"found": True, "enabled": True}}}
        self.assertEqual(studio_invariants.available_redo(kinds, {"records": []}), [])
        records = [
            {"actor_id": 1, "actor_display_name": "Pelican Studio Inspector",
             "operation_kind": "edit"},
            {"actor_id": 1, "actor_display_name": "Pelican Studio Inspector",
             "operation_kind": "undo"},
        ]
        self.assertEqual(studio_invariants.available_redo(kinds, {"records": records}), ["scene"])
        # When the journal could not be read, preserve potentially older Redo entries.
        self.assertEqual(studio_invariants.available_redo(kinds, None), ["scene"])
        kinds["graph"] = {"redo": {"found": True, "enabled": True}}
        self.assertEqual(studio_invariants.available_redo(kinds, {"records": []}), ["graph"])

    def test_hidden_parent_does_not_hide_an_enabled_material_undo(self) -> None:
        epilogue = studio_invariants.Epilogue(mock.Mock(), {"name": "hidden-undo"})
        control = {"found": True, "enabled": True, "visible": False,
                   "tooltip": "Undo material on every Cube"}
        epilogue.history = lambda: {"material_row": {"undo": control}}
        epilogue.call = mock.Mock(return_value={"ok": True})
        epilogue.settled = ("history", "before", 1)
        epilogue.settle = mock.Mock(return_value=("history", "after", 1))
        log = []
        self.assertEqual(epilogue.press_once("material_row", "undo", log, []), "moved")
        epilogue.call.assert_called_once_with(
            {"cmd": "history", "kind": "material_row", "step": "undo"}, timeout=60.0)
        self.assertTrue(log[0]["documents_moved"])
        # Empty histories disable their control, regardless of the parent's visibility.
        control["enabled"] = False
        epilogue.call.reset_mock()
        self.assertEqual(epilogue.press_once("material_row", "undo", [], []), "off")
        epilogue.call.assert_not_called()

    def test_a_refused_history_is_retried_after_another_changes_documents(self) -> None:
        epilogue = studio_invariants.Epilogue(mock.Mock(), {"name": "dependency"})
        state = {"reference_removed": False, "node_removed": False}

        def press(kind, step, log, notes):
            if kind == "graph" and not state["reference_removed"]:
                log.append({"kind": kind, "moved": False, "documents_moved": False})
                return "still"
            key = {"render": "reference_removed", "graph": "node_removed"}.get(kind)
            if key and not state[key]:
                state[key] = True
                log.append({"kind": kind, "moved": True, "documents_moved": True})
                return "moved"
            return "off"

        epilogue.press_once = press
        result = epilogue.press_all("undo", studio_invariants.time.monotonic() + 10)
        self.assertTrue(state["node_removed"])
        self.assertEqual(result["sequence"], ["render", "graph"])

    def test_a_control_only_change_does_not_retry_a_refused_history(self) -> None:
        epilogue = studio_invariants.Epilogue(mock.Mock(), {"name": "control-only"})
        calls = {"graph": 0, "render": 0}

        def press(kind, step, log, notes):
            if kind == "graph":
                calls[kind] += 1
                log.append({"kind": kind, "moved": False, "documents_moved": False})
                return "still"
            if kind == "render" and calls[kind] == 0:
                calls[kind] += 1
                log.append({"kind": kind, "moved": True, "documents_moved": False})
                return "moved"
            return "off"

        epilogue.press_once = press
        epilogue.press_all("undo", studio_invariants.time.monotonic() + 10)
        self.assertEqual(calls["graph"], 1)

    def test_preexisting_redo_is_not_drained_past_the_scenario_endpoint(self) -> None:
        epilogue = studio_invariants.Epilogue(mock.Mock(), {"name": "already-undone"})
        epilogue.press_once = mock.Mock(return_value="moved")
        epilogue.press_all = mock.Mock(side_effect=AssertionError("would consume old Redo"))
        result = epilogue.redo_in_reverse(
            {"sequence": ["graph", "graph"]}, studio_invariants.time.monotonic() + 10,
            drain_remaining=False)
        self.assertEqual(epilogue.press_once.call_count, 2)
        epilogue.press_all.assert_not_called()
        self.assertEqual(result["sequence"], ["graph", "graph"])
        self.assertIn("already had Redo entries", result["notes"][0])

    def test_incomplete_history_walk_cannot_pass_matching_snapshots(self) -> None:
        for phase in ("undo", "redo"):
            with self.subTest(phase=phase):
                epilogue = studio_invariants.Epilogue(mock.Mock(), {"name": "incomplete"})
                epilogue.start = project_state({}, {})
                epilogue.state = mock.Mock(return_value=epilogue.start)
                epilogue.history = lambda: {}
                walks = {name: {"presses": {}, "sequence": [], "log": [], "notes": []}
                         for name in ("undo", "redo")}
                walks[phase]["notes"].append(f"{phase}: stopped at the time budget (240 s)")
                epilogue.press_all = mock.Mock(return_value=walks["undo"])
                epilogue.redo_in_reverse = mock.Mock(return_value=walks["redo"])
                result = studio_invariants.check_undo(epilogue)
                self.assertEqual(result["status"], "red")
                self.assertTrue(result["check_error"])
                self.assertIn("time budget", result["reason"])

    def test_preexisting_redo_replay_reports_an_expired_budget(self) -> None:
        epilogue = studio_invariants.Epilogue(mock.Mock(), {"name": "incomplete-redo"})
        epilogue.press_once = mock.Mock()
        result = epilogue.redo_in_reverse({"sequence": ["graph"]}, -1,
                                         drain_remaining=False)
        epilogue.press_once.assert_not_called()
        self.assertTrue(any("stopped at the time budget" in n for n in result["notes"]))

    def test_known_red_keeps_the_run_green_and_says_so(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            run = studio_drive.Run({"name": "k"}, Path(directory))
            scenario = {"name": "k", "invariants": {"undo": "known_red", "undo_ticket": "QD1-1",
                                                    "reopen": False, "reopen_reason": "r",
                                                    "health": False, "health_reason": "r"}}
            epilogue = studio_invariants.Epilogue(run, scenario)
            epilogue.call = lambda request, timeout=0: {"ok": True, "result": {"result": {"problems": []}}}
            red = {"invariant": "undo", "status": "red", "reason": "after Undo, not the start"}
            with (mock.patch.object(studio_invariants, "check_undo", return_value=dict(red)),
                  contextlib.redirect_stdout(io.StringIO())):
                outcomes = studio_invariants.run_invariants(run, epilogue)
            self.assertEqual(outcomes[0]["status"], "known_red")
            self.assertEqual(outcomes[0]["ticket"], "QD1-1")
            self.assertEqual(run.failures, [])
            self.assertIn("KNOWN RED (QD1-1)", run.steps[0]["detail"])
            self.assertEqual([outcome["status"] for outcome in outcomes[1:]], ["off", "off"])
            # Unmarked, the same finding fails the run.
            run = studio_drive.Run({"name": "k"}, Path(directory))
            epilogue = studio_invariants.Epilogue(run, {"name": "k", "invariants": {
                "reopen": False, "reopen_reason": "r", "health": False, "health_reason": "r"}})
            epilogue.call = lambda request, timeout=0: {"ok": True, "result": {"result": {"problems": []}}}
            with (mock.patch.object(studio_invariants, "check_undo", return_value=dict(red)),
                  contextlib.redirect_stdout(io.StringIO())):
                studio_invariants.run_invariants(run, epilogue)
            self.assertEqual(len(run.failures), 1)

    def test_known_red_cannot_hide_a_broken_checker(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            run = studio_drive.Run({"name": "broken-checker"}, Path(directory))
            scenario = {"invariants": {"undo": "known_red", "undo_ticket": "QD1-R1",
                                       "reopen": False, "health": False, "reason": "test isolation"}}
            epilogue = studio_invariants.Epilogue(run, scenario)
            epilogue.call = lambda request, timeout=0: {"ok": True, "result": {"result": {"problems": []}}}
            with (mock.patch.object(studio_invariants, "check_undo", side_effect=ValueError("bad reply")),
                  contextlib.redirect_stdout(io.StringIO())):
                outcomes = studio_invariants.run_invariants(run, epilogue)
            self.assertEqual(outcomes[0]["status"], "red")
            self.assertTrue(outcomes[0]["check_error"])
            self.assertEqual(len(run.failures), 1)



if __name__ == "__main__":
    unittest.main()
