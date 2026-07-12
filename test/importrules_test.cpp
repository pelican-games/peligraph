#include "../src/project/importrules.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace Pelican;

namespace {

nlohmann::json readJson(const std::filesystem::path &path) {
    std::ifstream file{path};
    return nlohmann::json::parse(file);
}

std::filesystem::path fixture(std::string_view name) {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" / "import_rules" / name;
}

} // namespace

TEST_CASE("import rules evaluate explicit, defaults, builtin, and unmatched precedence") {
    const auto rules = parseImportRulesJson(readJson(fixture("precedence.json")));

    const auto explicit_match = evaluateImportRules(rules, "ui/menu.psd");
    REQUIRE(explicit_match);
    CHECK(explicit_match->layer == ImportRuleLayer::rule);
    CHECK(explicit_match->recipe.name == "atlas_pack");
    CHECK(explicit_match->recipe.options.at("padding") == 0);

    const auto project_default = evaluateImportRules(rules, "characters/hero.psd");
    REQUIRE(project_default);
    CHECK(project_default->layer == ImportRuleLayer::defaults);
    CHECK(project_default->recipe.name == "psd_layers");

    const auto builtin = evaluateImportRules(rules, "levels/city.glb");
    REQUIRE(builtin);
    CHECK(builtin->layer == ImportRuleLayer::builtin);
    CHECK(builtin->recipe.name == "extract_scene");

    CHECK_FALSE(evaluateImportRules(rules, "raw/logo.png"));
    CHECK_FALSE(evaluateImportRules(rules, "notes/readme.txt"));
}

TEST_CASE("import globstar spans zero or more complete path segments") {
    CHECK(importGlobMatches("ui/**/*.psd", "ui/menu.psd"));
    CHECK(importGlobMatches("ui/**/*.psd", "ui/dialogs/pause.psd"));
    CHECK_FALSE(importGlobMatches("ui/**/*.psd", "other/menu.psd"));
    CHECK(importGlobMatches("levels/?.glb", "levels/a.glb"));
    CHECK_FALSE(importGlobMatches("levels/?.glb", "levels/ab.glb"));
}

TEST_CASE("import rules errors name unknown keys and invalid globs") {
    for (const auto &entry : readJson(fixture("invalid.json"))) {
        INFO(entry.at("name").get<std::string>());
        REQUIRE_THROWS_WITH(parseImportRulesJson(entry.at("document")),
                            Catch::Matchers::ContainsSubstring(entry.at("error").get<std::string>()));
    }
}
