from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import run_gpu_gate
from skip_policy import SkipPolicyError, validate_skip_policy


def write_junit(path: Path, testcases: list[tuple[str, bool]]) -> None:
    cases = []
    for name, skipped in testcases:
        child = '<skipped message="Not Run" />' if skipped else ""
        status = "notrun" if skipped else "run"
        cases.append(f'<testcase name="{name}" status="{status}">{child}</testcase>')
    path.write_text(
        f'<testsuite tests="{len(cases)}">{"".join(cases)}</testsuite>',
        encoding="utf-8",
    )


class SkipPolicyTest(unittest.TestCase):
    def test_allows_only_the_exact_optional_skip(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            junit = root / "report.xml"
            allowlist = root / "allowlist.txt"
            write_junit(junit, [("ordinary test", False), ("symlink test", True)])
            allowlist.write_text("symlink test\n", encoding="utf-8")

            self.assertEqual(validate_skip_policy(junit, allowlist), {"symlink test"})

    def test_rejects_an_artificial_unexpected_skip(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            junit = root / "report.xml"
            allowlist = root / "allowlist.txt"
            write_junit(junit, [("ordinary test", False), ("artificial skip", True)])
            allowlist.write_text("ordinary test\n", encoding="utf-8")

            with self.assertRaisesRegex(SkipPolicyError, "artificial skip"):
                validate_skip_policy(junit, allowlist)

    def test_rejects_a_stale_allowlist_entry(self) -> None:
        # A name that never appears means the list has rotted past the suite.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            junit = root / "report.xml"
            allowlist = root / "allowlist.txt"
            write_junit(junit, [("ordinary test", False)])
            allowlist.write_text("test that no longer exists\n", encoding="utf-8")

            with self.assertRaisesRegex(SkipPolicyError, "test that no longer exists"):
                validate_skip_policy(junit, allowlist)

    def test_names_the_gate_in_its_errors(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            junit = root / "report.xml"
            allowlist = root / "allowlist.txt"
            write_junit(junit, [("ordinary test", False)])
            allowlist.write_text("absent\n", encoding="utf-8")

            with self.assertRaisesRegex(SkipPolicyError, "GPU gate"):
                validate_skip_policy(junit, allowlist, gate_name="GPU")

    def test_bulk_skip_hint_only_fires_when_most_of_the_suite_skipped(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            junit = root / "report.xml"
            allowlist = root / "allowlist.txt"
            allowlist.write_text("", encoding="utf-8")

            write_junit(junit, [("a", True), ("b", True), ("c", True)])
            with self.assertRaisesRegex(SkipPolicyError, "no Vulkan device"):
                validate_skip_policy(junit, allowlist, "GPU", "no Vulkan device was available")

            write_junit(junit, [("a", True), ("b", False), ("c", False), ("d", False)])
            with self.assertRaises(SkipPolicyError) as caught:
                validate_skip_policy(junit, allowlist, "GPU", "no Vulkan device was available")
            self.assertNotIn("no Vulkan device", str(caught.exception))

    def test_rejects_wildcard_allowlist_entries(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            junit = root / "report.xml"
            allowlist = root / "allowlist.txt"
            write_junit(junit, [("symlink test", True)])
            allowlist.write_text("*symlink*\n", encoding="utf-8")

            with self.assertRaisesRegex(SkipPolicyError, "wildcard"):
                validate_skip_policy(junit, allowlist)

    def test_required_test_cannot_be_waived_by_the_skip_allowlist(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            junit = root / "report.xml"
            allowlist = root / "allowlist.txt"
            write_junit(junit, [("studio_scenario_edit", True)])
            allowlist.write_text("studio_scenario_edit\n", encoding="utf-8")
            with self.assertRaisesRegex(SkipPolicyError, "required.*did not run"):
                validate_skip_policy(
                    junit, allowlist, "GPU", required_tests={"studio_scenario_edit"}
                )


class GpuGateCoverageTest(unittest.TestCase):
    def run_gate(self, reported: set[str], *, engine_only: bool = False) -> int:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            allowlist = root / "allowlist.txt"
            allowlist.write_text("", encoding="utf-8")

            def successful_ctest(ctest, build_dir, config, labels, junit, log):
                write_junit(junit, [(name, False) for name in sorted(reported)])
                return 0

            with (
                mock.patch("skip_policy.shutil.which", return_value="ctest"),
                mock.patch("skip_policy.run_ctest", side_effect=successful_ctest),
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                return run_gpu_gate.main([
                    "--build-dir", str(root / "build"),
                    "--artifacts-dir", str(root / "artifacts"),
                    "--allowlist", str(allowlist),
                    *(["--engine-only"] if engine_only else []),
                ])

    def test_full_gate_requires_every_shipped_studio_scenario(self) -> None:
        required = run_gpu_gate.required_studio_tests(
            Path(__file__).resolve().parents[1] / "studio_scenarios"
        )
        self.assertEqual(self.run_gate(required | {"engine_test"}), 0)
        for absent in (required, {sorted(required)[0]}):
            with self.subTest(absent=sorted(absent)):
                self.assertEqual(self.run_gate((required - absent) | {"engine_test"}), 2)

    def test_engine_only_is_an_explicit_subset(self) -> None:
        self.assertEqual(self.run_gate({"engine_test"}, engine_only=True), 0)

    def test_missing_scenario_directory_cannot_lower_the_required_coverage(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(SkipPolicyError, "no required Studio scenarios"):
                run_gpu_gate.required_studio_tests(Path(directory) / "missing")


if __name__ == "__main__":
    unittest.main()
