#!/usr/bin/env -S uv run --quiet --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Self-test for tools/doclink.py.  Run: uv run tools/doclink_test.py"""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import doclink  # noqa: E402


class DeriveSymbol(unittest.TestCase):
    def test_accepts_identifiers_and_strips_decoration(self) -> None:
        self.assertEqual(doclink.derive_symbol("`ECSCore`"), doclink.Symbol("ECSCore", None))
        self.assertEqual(doclink.derive_symbol("`Renderer::render()`"), doclink.Symbol("render", "Renderer"))
        self.assertEqual(doclink.derive_symbol("`ComponentIdByType<T>`"), doclink.Symbol("ComponentIdByType", None))
        self.assertEqual(doclink.derive_symbol("`foo(int a, int b)`"), doclink.Symbol("foo", None))
        self.assertEqual(doclink.derive_symbol("`struct PassDefinition`"), doclink.Symbol("PassDefinition", None))

    def test_refuses_text_that_is_not_a_symbol(self) -> None:
        self.assertIsNone(doclink.derive_symbol("`src/core/loop.cpp`"), "a path is a file reference")
        self.assertIsNone(doclink.derive_symbol("`loop.cpp`"), "a file name is a file reference")
        self.assertIsNone(doclink.derive_symbol("同 L358"), "Japanese prose")
        self.assertIsNone(doclink.derive_symbol("#L124"), "a bare line reference carries no anchor")
        self.assertIsNone(doclink.derive_symbol("`teardown.cpp#L114`"))
        self.assertIsNone(doclink.derive_symbol(""))


class DefinitionScore(unittest.TestCase):
    def test_recognises_repo_declaration_idioms(self) -> None:
        ecs = doclink.Symbol("ECSCore", None)
        self.assertGreater(
            doclink.definition_score("DECLARE_MODULE(ECSCore) {", ecs), 0,
            "DECLARE_MODULE is how modules are declared here",
        )
        game_context = doclink.Symbol("GameContext", None)
        self.assertGreater(
            doclink.definition_score("class PELICAN_API GameContext {", game_context), 0,
            "an export macro sits between class and name",
        )
        handle = doclink.Symbol("ModelHandle", None)
        self.assertGreater(doclink.definition_score("PELICAN_DEFINE_HANDLE(ModelHandle)", handle), 0)

    def test_separates_definitions_from_mentions(self) -> None:
        symbol = doclink.Symbol("importSceneDocument", None)
        self.assertGreater(
            doclink.definition_score("SceneRevision ProjectBasicConfig::importSceneDocument(", symbol), 0,
            "a qualified out-of-class definition counts even when the link text is unqualified",
        )
        self.assertLessEqual(doclink.definition_score("    // importSceneDocument copies the tree", symbol), 0)
        self.assertLessEqual(doclink.definition_score("    return importSceneDocument(doc);", symbol), 0)
        self.assertLessEqual(doclink.definition_score('#include "importSceneDocument.hpp"', symbol), 0)

    def test_prefers_the_qualified_definition(self) -> None:
        symbol = doclink.Symbol("render", "Renderer")
        qualified = doclink.definition_score("void Renderer::render() {", symbol)
        bare = doclink.definition_score("void render() {", symbol)
        self.assertGreater(qualified, bare)


class RelocateSymbol(unittest.TestCase):
    def test_finds_a_unique_definition(self) -> None:
        lines = ["#pragma once", "", "namespace P {", "", "class Widget {", "};", "}"]
        result = doclink.relocate_symbol(lines, doclink.Symbol("Widget", None), 2)
        self.assertEqual(result["status"], "moved")
        self.assertEqual(result["line"], 5)

    def test_breaks_ties_by_proximity(self) -> None:
        lines = ["// filler"] * 100
        lines[9] = "void ping() {"   # line 10
        lines[79] = "void ping() {"  # line 80
        result = doclink.relocate_symbol(lines, doclink.Symbol("ping", None), 78)
        self.assertEqual(result["status"], "moved")
        self.assertEqual(result["line"], 80, "drift is local, so the nearer definition wins")

    def test_refuses_to_guess_between_equidistant_definitions(self) -> None:
        lines = ["// filler"] * 100
        lines[9] = "void ping() {"   # line 10
        lines[29] = "void ping() {"  # line 30
        result = doclink.relocate_symbol(lines, doclink.Symbol("ping", None), 20)
        self.assertEqual(result["status"], "unresolved")

    def test_reports_when_no_definition_exists(self) -> None:
        result = doclink.relocate_symbol(["// nothing here"], doclink.Symbol("Absent", None), 1)
        self.assertEqual(result["status"], "unresolved")


class RelocateLedger(unittest.TestCase):
    def test_follows_remembered_text_ignoring_indentation(self) -> None:
        lines = ["a", "b", "        const auto x = compute();"]
        result = doclink.relocate_ledger(lines, "const auto x = compute();", 1)
        self.assertEqual(result["status"], "moved")
        self.assertEqual(result["line"], 3)

    def test_reports_vanished_and_ambiguous_anchors(self) -> None:
        self.assertEqual(doclink.relocate_ledger(["a", "b"], "gone", 1)["status"], "unresolved")
        duplicated = ["x", "dup", "y", "dup", "z"]
        self.assertEqual(
            doclink.relocate_ledger(duplicated, "dup", 3)["status"], "unresolved",
            "equidistant duplicates are ambiguous",
        )


class Rewrite(unittest.TestCase):
    def test_replaces_only_the_line_number(self) -> None:
        text = "see [`A`](../../src/a.cpp#L10) and [`B`](../../src/b.cpp#L20) end"
        results = [
            doclink.Link("d", "`A`", "../../src/a.cpp", 10, text.index("[`A`]"), "[`A`](../../src/a.cpp#L10)", "ka", status="moved", line=11),
            doclink.Link("d", "`B`", "../../src/b.cpp", 20, text.index("[`B`]"), "[`B`](../../src/b.cpp#L20)", "kb", status="moved", line=999),
        ]
        self.assertEqual(
            doclink.rewrite(text, results),
            "see [`A`](../../src/a.cpp#L11) and [`B`](../../src/b.cpp#L999) end",
        )

    def test_leaves_untouched_links_alone(self) -> None:
        text = "[`A`](../../src/a.cpp#L10)"
        link = doclink.Link("d", "`A`", "../../src/a.cpp", 10, 0, text, "k", status="ok")
        self.assertEqual(doclink.rewrite(text, [link]), text)

    def test_ledger_key_distinguishes_repeated_links(self) -> None:
        first = doclink.ledger_key("docs/x.md", "../../src/a.cpp", "`Foo`", 0)
        second = doclink.ledger_key("docs/x.md", "../../src/a.cpp", "`Foo`", 1)
        self.assertNotEqual(first, second, "the second occurrence must get its own ledger entry")


class EndToEnd(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        (self.root / "docs" / "manual").mkdir(parents=True)
        (self.root / "src").mkdir()
        self.addCleanup(self._tmp.cleanup)

    def _ledger(self) -> dict:
        return json.loads((self.root / "docs" / "link_anchors.json").read_text(encoding="utf-8"))

    def test_update_relocates_a_drifted_symbol_link(self) -> None:
        (self.root / "src" / "a.cpp").write_text(
            "// header comment\n// another\n\nvoid doTheThing() {\n}\n", encoding="utf-8"
        )
        doc = self.root / "docs" / "manual" / "01_x.md"
        doc.write_text("call [`doTheThing()`](../../src/a.cpp#L2) now.\n", encoding="utf-8")

        self.assertEqual(doclink.run("check", self.root), 1, "a drifted link must fail check")
        self.assertEqual(doclink.run("update", self.root), 0)
        self.assertIn("#L4)", doc.read_text(encoding="utf-8"))
        self.assertEqual(doclink.run("check", self.root), 0, "check is clean after update")

    def test_ledger_tier_tracks_a_link_with_no_symbol(self) -> None:
        source = self.root / "src" / "b.cpp"
        source.write_text("one();\nconst auto marker = 42;\nthree();\n", encoding="utf-8")
        doc = self.root / "docs" / "manual" / "02_y.md"
        doc.write_text("see [b.cpp](../../src/b.cpp#L2).\n", encoding="utf-8")

        self.assertEqual(doclink.run("update", self.root), 0, "first run adopts the anchor")
        ledger = self._ledger()
        self.assertEqual(ledger["schema"], "pelican.doc_link_anchors")
        self.assertIn("const auto marker = 42;", ledger["anchors"].values())

        # Someone inserts two lines above the anchor.
        source.write_text("zero();\nhalf();\none();\nconst auto marker = 42;\nthree();\n", encoding="utf-8")
        self.assertEqual(doclink.run("check", self.root), 1, "check notices the drift")
        self.assertEqual(doclink.run("update", self.root), 0)
        self.assertIn("#L4)", doc.read_text(encoding="utf-8"), "the ledger anchor followed the text")

    def test_blank_anchor_settles_instead_of_being_readopted(self) -> None:
        # Regression: the ledger stores "" for such links, and a truthiness check treated the
        # entry as absent, so every run adopted it again -- forever.
        (self.root / "src" / "d.cpp").write_text("one();\n\nthree();\n", encoding="utf-8")
        doc = self.root / "docs" / "manual" / "05_v.md"
        doc.write_text("see [d.cpp](../../src/d.cpp#L2).\n", encoding="utf-8")

        self.assertEqual(doclink.run("update", self.root), 0)
        first = (self.root / "docs" / "link_anchors.json").read_text(encoding="utf-8")
        self.assertEqual(doclink.run("update", self.root), 0, "a blank anchor must not fail the run")
        self.assertEqual(
            (self.root / "docs" / "link_anchors.json").read_text(encoding="utf-8"), first,
            "the ledger must reach a fixed point",
        )

    def test_broken_link_is_reported_not_rewritten(self) -> None:
        (self.root / "src" / "c.cpp").write_text("nothing relevant\n", encoding="utf-8")
        doc = self.root / "docs" / "manual" / "03_z.md"
        doc.write_text("gone [`vanishedSymbol()`](../../src/c.cpp#L900).\n", encoding="utf-8")
        before = doc.read_text(encoding="utf-8")
        self.assertEqual(doclink.run("update", self.root), 1, "unresolved links fail even in update mode")
        self.assertEqual(doc.read_text(encoding="utf-8"), before, "the document must not be touched")

    def test_missing_target_file_is_reported(self) -> None:
        doc = self.root / "docs" / "manual" / "04_w.md"
        doc.write_text("bad [`Foo`](../../src/does_not_exist.cpp#L1).\n", encoding="utf-8")
        self.assertEqual(doclink.run("check", self.root), 1)

    def test_only_restricts_the_pass(self) -> None:
        (self.root / "src" / "e.cpp").write_text("// x\nvoid keep() {\n}\n", encoding="utf-8")
        touched = self.root / "docs" / "manual" / "06_a.md"
        untouched = self.root / "docs" / "manual" / "07_b.md"
        touched.write_text("[`keep()`](../../src/e.cpp#L1)\n", encoding="utf-8")
        untouched.write_text("[`keep()`](../../src/e.cpp#L1)\n", encoding="utf-8")
        doclink.run("update", self.root, only={"docs/manual/06_a.md"})
        self.assertIn("#L2)", touched.read_text(encoding="utf-8"))
        self.assertIn("#L1)", untouched.read_text(encoding="utf-8"), "documents outside --only stay put")

    def test_corrupt_ledger_is_a_hard_error(self) -> None:
        (self.root / "docs" / "link_anchors.json").write_text("{ not json", encoding="utf-8")
        with self.assertRaises(doclink.DocLinkError):
            doclink.run("check", self.root)


if __name__ == "__main__":
    unittest.main(verbosity=2)
