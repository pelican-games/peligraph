from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from contract_boundary_gate import (  # noqa: E402
    OPENPBR_PIN,
    exact_set_issues,
    validate_engine_mvp,
    validate_openpbr,
    validate_projection,
)


class ContractBoundaryGateTest(unittest.TestCase):
    def test_exact_set_diagnostics_name_both_sides(self) -> None:
        self.assertEqual(
            exact_set_issues(
                "variants",
                "engine",
                {"opaque_single", "engine_intruder"},
                "importer",
                {"opaque_single", "importer_intruder"},
            ),
            [
                "variants: engine-only: engine_intruder",
                "variants: importer-only: importer_intruder",
            ],
        )

    def test_openpbr_gate_reports_engine_only_variant_and_pin_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            engine_root = root / "src/core/resources/surfaces/openpbr"
            importer_root = root / "test/fixtures/contract0"
            engine_root.mkdir(parents=True)
            importer_root.mkdir(parents=True)
            variants = []
            for alpha_mode in ("opaque", "mask", "blend"):
                for double_sided in (False, True):
                    side = "double" if double_sided else "single"
                    name = f"{alpha_mode}_{side}_sided"
                    surface = f"engine://surfaces/openpbr/{alpha_mode}_{side}.surface"
                    variants.append(
                        {
                            "name": name,
                            "surface": surface,
                            "alpha_mode": alpha_mode,
                            "double_sided": double_sided,
                        }
                    )
                    (engine_root / f"{alpha_mode}_{side}.surface").write_text(
                        "fixture\n", encoding="utf-8"
                    )
            engine = {
                "schema": "pelican.openpbr_surface",
                "version": 1,
                "openpbr": OPENPBR_PIN,
                "variants": variants,
            }
            importer = {
                "schema": "pelican.openpbr_importer_variants",
                "version": 1,
                "openpbr": OPENPBR_PIN,
                "defines": [
                    "OPENPBR_PIN_V1_1_1",
                    "OPENPBR_PIN_F8D6D947DFAE4C9B599965A86C22826EA7A8DBFB",
                ],
                "variants": variants[:-1],
            }
            (engine_root / "manifest.json").write_text(
                json.dumps(engine), encoding="utf-8"
            )
            (importer_root / "openpbr_importer_variants.json").write_text(
                json.dumps(importer), encoding="utf-8"
            )
            issues = validate_openpbr(root)
            self.assertIn(
                "OpenPBR variants: engine-only: blend_double_sided", issues
            )

            importer["openpbr"] = {**OPENPBR_PIN, "version": "1.1.2"}
            (importer_root / "openpbr_importer_variants.json").write_text(
                json.dumps(importer), encoding="utf-8"
            )
            issues = validate_openpbr(root)
            self.assertTrue(
                any("OpenPBR importer: exact pin mismatch" in issue for issue in issues),
                issues,
            )

    def test_projection_gate_rejects_unregistered_source_by_name(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source_root = root / "src/core/resources"
            fixture_root = root / "test/fixtures"
            source_root.mkdir(parents=True)
            fixture_root.mkdir(parents=True)
            (source_root / "known.vert").write_text(
                "gl_Position = pelicanFrame.projection * value;\n", encoding="utf-8"
            )
            inventory = {
                "schema": "pelican.projection_consumer_inventory",
                "version": 1,
                "scan": {
                    "roots": ["src/core"],
                    "extensions": [".vert"],
                    "patterns": [r"pelicanFrame\.projection"],
                },
                "consumers": [
                    {
                        "name": "known",
                        "matrix": "jittered",
                        "route": "fixture",
                        "taa_policy": "general formula",
                    }
                ],
                "sources": [
                    {
                        "source": "src/core/resources/known.vert",
                        "consumer": "known",
                        "role": "fixture",
                        "needles": ["pelicanFrame.projection"],
                    }
                ],
            }
            (fixture_root / "projection_jitter_consumers.json").write_text(
                json.dumps(inventory), encoding="utf-8"
            )
            self.assertEqual(validate_projection(root), [])

            intruder = "src/core/resources/intruder.vert"
            (root / intruder).write_text(
                "gl_Position = pelicanFrame.projection * value;\n", encoding="utf-8"
            )
            issues = validate_projection(root)
            self.assertIn(
                f"projection sources: discovered-unregistered-only: {intruder}", issues
            )

    def test_engine_mvp_gate_rejects_semantic_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fixture_root = root / "test/fixtures/contract0"
            fixture_root.mkdir(parents=True)
            (root / "src/core/resources").mkdir(parents=True)
            fixture = {
                "schema": "pelican.engine_mvp_contract",
                "version": 1,
                "contract": {"abi": "silently_changed"},
                "engine_mvp_shader_sources": [],
                "anchors": [],
            }
            (fixture_root / "engine_mvp_v1.json").write_text(
                json.dumps(fixture), encoding="utf-8"
            )
            issues = validate_engine_mvp(root)
            self.assertTrue(
                any("v1 semantics changed" in issue for issue in issues), issues
            )


if __name__ == "__main__":
    unittest.main()
