"""Regression tests for what is published and how private repository links resolve."""
import unittest
from types import SimpleNamespace
import docs_site_hooks as hooks


class DocumentationSiteTest(unittest.TestCase):
    def setUp(self):
        hooks.PAGES = {"index.md", "manual/14_glossary.md"}
        hooks.REVISION = "test-revision"
        self.page = SimpleNamespace(file=SimpleNamespace(src_uri="manual/14_glossary.md"))
        self.config = {"repo_url": "https://github.com/example/private"}

    def render(self, source):
        return hooks.on_page_markdown(source, self.page, self.config, [])

    def test_only_explicit_pages_and_assets_are_published(self):
        files = [SimpleNamespace(src_uri=p, src_dir=str(hooks.ROOT / "docs")) for p in (
            "index.md", "manual/14_glossary.md", "site/styles.css",
            "implementation_plan.md", "new-internal-notes.md", "private.json", "site/secret.txt")]
        hooks.on_files(files, {"docs_dir": str(hooks.ROOT / "docs")})
        self.assertEqual([f.src_uri for f in files], ["index.md", "manual/14_glossary.md", "site/styles.css"])

    def test_theme_assets_remain_available(self):
        files = [SimpleNamespace(src_uri="assets/stylesheets/main.css", src_dir=str(hooks.ROOT / "theme"))]
        hooks.on_files(files, {"docs_dir": str(hooks.ROOT / "docs")})
        self.assertEqual(len(files), 1)

    def test_source_link_keeps_commit_and_line(self):
        output = self.render("[実装](../../src/core/loader/scene.cpp#L10)")
        self.assertIn("/blob/test-revision/src/core/loader/scene.cpp#L10", output)

    def test_unpublished_design_and_directory_links_stay_on_github(self):
        output = self.render("[設計](../design_scene_format.md) [コード](../../src/core/)")
        self.assertIn("/blob/test-revision/docs/design_scene_format.md", output)
        self.assertIn("/tree/test-revision/src/core", output)

    def test_local_and_external_links_are_preserved(self):
        source = "[用語](14_glossary.md#用語) [外部](https://example.com/a) [節](#用語)"
        self.assertEqual(self.render(source), source)

    def test_multiline_labels_are_rewritten(self):
        output = self.render("[型と\n関数](../../src/devstudio/view/nodegraphics.hpp#L122)")
        self.assertIn("/blob/test-revision/src/devstudio/view/nodegraphics.hpp#L122", output)

    def test_examples_are_not_rewritten(self):
        for fence in ("```", "~~~", "> ```"):
            source = f"{fence}markdown\n[例](../../src/core/loader/scene.cpp)\n{fence}\n"
            self.assertEqual(self.render(source), source)

    def test_bad_links_and_repository_escape_fail(self):
        for destination in ("../../missing-source.cpp", "../../../../../outside.txt"):
            with self.assertRaises(ValueError):
                self.render(f"[不正]({destination})")


if __name__ == "__main__":
    unittest.main()
