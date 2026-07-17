from __future__ import annotations

import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from golden_inventory import (  # noqa: E402
    MANIFEST_RELATIVE_PATH,
    validate_inventory,
    write_inventory,
)


class GoldenInventoryTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary_directory.cleanup)
        self.root = Path(self.temporary_directory.name)
        golden = self.root / "test/golden"
        fixtures = self.root / "test/fixtures"
        fixtures.mkdir(parents=True)
        golden.mkdir(parents=True)
        (golden / "README.md").write_text("fixture\n", encoding="utf-8")

        self._write_case("alpha", b"alpha", "clear")
        self._write_case("vat_playback", b"vat", "vat_playback")
        (fixtures / "wp73_rgba8_hashes.json").write_text(
            json.dumps({"alpha": "a", "vat_playback": "b"}), encoding="utf-8"
        )
        (fixtures / "renderer_execution_traces.json").write_text(
            json.dumps({"alpha": {}}), encoding="utf-8"
        )
        (fixtures / "canonical_frame_plan_trace.txt").write_text(
            "alpha: present[render]\n", encoding="utf-8"
        )
        write_inventory(self.root)

    def _write_case(self, name: str, expected: bytes, mode: str) -> None:
        case = self.root / "test/golden" / name
        case.mkdir(parents=True)
        (case / "case.json").write_text(json.dumps({"mode": mode}), encoding="utf-8")
        (case / "expected.png").write_bytes(expected)
        (case / "tolerance.json").write_text("{}\n", encoding="utf-8")

    def test_clean_inventory_passes_and_records_vat_conditions(self) -> None:
        self.assertEqual(validate_inventory(self.root), [])
        manifest = json.loads(
            (self.root / MANIFEST_RELATIVE_PATH).read_text(encoding="utf-8")
        )
        cases = {case["name"]: case for case in manifest["cases"]}
        self.assertEqual(cases["alpha"]["vat"], "on_and_off")
        self.assertEqual(cases["vat_playback"]["vat"], "on_only")

    def test_rejects_expected_png_tampering(self) -> None:
        (self.root / "test/golden/alpha/expected.png").write_bytes(b"tampered")
        issues = validate_inventory(self.root)
        self.assertTrue(
            any("case alpha: expected_png_sha256 mismatch" in issue for issue in issues),
            issues,
        )

    def test_rejects_missing_and_unknown_case_files_by_name(self) -> None:
        (self.root / "test/golden/alpha/tolerance.json").unlink()
        (self.root / "test/golden/alpha/notes.txt").write_text("unknown", encoding="utf-8")
        issues = validate_inventory(self.root)
        self.assertIn("case alpha: missing file: tolerance.json", issues)
        self.assertIn("case alpha: unknown file: notes.txt", issues)

    def test_rejects_an_unregistered_case_directory(self) -> None:
        self._write_case("intruder", b"new", "clear")
        issues = validate_inventory(self.root)
        self.assertIn("unknown golden case directory: intruder", issues)

    def test_rejects_a_missing_case_directory(self) -> None:
        shutil.rmtree(self.root / "test/golden/alpha")
        issues = validate_inventory(self.root)
        self.assertIn("missing golden case directory: alpha", issues)

    def test_rejects_trace_membership_tampering(self) -> None:
        (self.root / "test/fixtures/renderer_execution_traces.json").write_text(
            "{}\n", encoding="utf-8"
        )
        issues = validate_inventory(self.root)
        self.assertIn("case alpha: missing trace membership: renderer_execution", issues)

    def test_update_is_byte_deterministic(self) -> None:
        manifest_path = self.root / MANIFEST_RELATIVE_PATH
        first = manifest_path.read_bytes()
        write_inventory(self.root)
        second = manifest_path.read_bytes()
        self.assertEqual(first, second)


if __name__ == "__main__":
    unittest.main()
