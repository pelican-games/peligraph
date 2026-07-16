#include "../src/project/importmanifest.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {

namespace {

std::filesystem::path fixtureRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" / "import_manifest";
}

nlohmann::json readJson(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) {
        throw std::runtime_error("failed to open fixture: " + path.string());
    }
    return nlohmann::json::parse(file);
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

void requireErrorKind(std::string_view message, std::string_view error_kind) {
    if (error_kind == "absolute") {
        REQUIRE(contains(message, "relative"));
    } else if (error_kind == "escape") {
        REQUIRE(contains(message, "escape"));
    } else if (error_kind == "unknown_schema") {
        REQUIRE(contains(message, "output schema"));
    } else if (error_kind == "schema") {
        REQUIRE(contains(message, "schema"));
    } else if (error_kind == "material_version") {
        REQUIRE(contains(message, "pelican.material"));
        REQUIRE(contains(message, "version"));
    } else {
        FAIL("unknown import manifest error_kind: " << error_kind);
    }
}

} // namespace

TEST_CASE("import manifest fixtures parse and reject expected cases", "[import-manifest]") {
    const auto expectations = readJson(fixtureRoot() / "expectations.json");

    for (const auto &entry : expectations) {
        const auto file = entry.at("file").get<std::string>();
        DYNAMIC_SECTION(file) {
            const auto manifest_json = readJson(fixtureRoot() / file);
            const auto expected = entry.at("expect").get<std::string>();
            if (expected == "ok") {
                const auto manifest = parseImportManifestJson(manifest_json);
                REQUIRE(manifest.outputs.size() == entry.at("output_count").get<size_t>());
                REQUIRE(manifest.source.file == entry.at("source_file").get<std::string>());
            } else {
                std::string message;
                try {
                    (void)parseImportManifestJson(manifest_json);
                } catch (const std::exception &ex) {
                    message = ex.what();
                }
                REQUIRE_FALSE(message.empty());
                requireErrorKind(message, entry.at("error_kind").get<std::string>());
            }
        }
    }
}

TEST_CASE("import manifest accepts deterministic K3 deliveries", "[import-manifest]") {
    const auto manifest = parseImportManifestJson({
        {"schema", "pelican.import"},
        {"version", 1},
        {"tool", {{"name", "pelican-import-tools"}, {"version", "0.1.0"}}},
        {"source", {{"files", {"sprites/a.png", "sprites/b.png"}}}},
        {"outputs", {{{"file", "atlas.json"},
                      {"schema", "pelican.atlas"},
                      {"version", 1},
                      {"sha256", std::string(64, 'a')}},
                     {{"file", "atlas_0.png"},
                      {"schema", "png"},
                      {"sha256", std::string(64, 'b')}}}},
    });
    CHECK(manifest.created.empty());
    CHECK(manifest.source.file.empty());
    CHECK(manifest.outputs.size() == 2);
}

TEST_CASE("import manifest accepts khronos.ktx2 outputs from the ktx2 recipe",
          "[import-manifest]") {
    // pelican-import-tools の ktx2 レシピは ("khronos.ktx2", 2) を出力する。
    const auto manifest = parseImportManifestJson({
        {"schema", "pelican.import"},
        {"version", 1},
        {"tool", {{"name", "pelican-import-tools"}, {"version", "0.1.0"}}},
        {"source", {{"files", {"textures/albedo.png"}}}},
        {"outputs", {{{"file", "albedo.ktx2"},
                      {"schema", "khronos.ktx2"},
                      {"version", 2},
                      {"sha256", std::string(64, 'c')}}}},
    });
    CHECK(manifest.outputs.size() == 1);

    const auto ktx2Manifest = [](nlohmann::json output) {
        output["file"] = "albedo.ktx2";
        output["schema"] = "khronos.ktx2";
        output["sha256"] = std::string(64, 'c');
        return nlohmann::json{
            {"schema", "pelican.import"},
            {"version", 1},
            {"tool", {{"name", "pelican-import-tools"}, {"version", "0.1.0"}}},
            {"source", {{"files", {"textures/albedo.png"}}}},
            {"outputs", {std::move(output)}},
        };
    };
    // version 欠落 / 1 / 3 は reject、2 のみ accept(MANIFEST-KTX2-VERSION)。
    CHECK_THROWS_WITH(parseImportManifestJson(ktx2Manifest({})),
                      Catch::Matchers::ContainsSubstring("requires version 2"));
    CHECK_THROWS_WITH(parseImportManifestJson(ktx2Manifest({{"version", 1}})),
                      Catch::Matchers::ContainsSubstring("expected=2, actual=1"));
    CHECK_THROWS_WITH(parseImportManifestJson(ktx2Manifest({{"version", 3}})),
                      Catch::Matchers::ContainsSubstring("expected=2, actual=3"));
    CHECK(parseImportManifestJson(ktx2Manifest({{"version", 2}})).outputs.size() == 1);
}

TEST_CASE("import manifest accepts only pelican.material version 1 outputs",
          "[import-manifest][material][wp124]") {
    const auto materialManifest = [](nlohmann::json output) {
        output["file"] = "materials.json";
        output["schema"] = "pelican.material";
        output["sha256"] = std::string(64, 'd');
        return nlohmann::json{
            {"schema", "pelican.import"},
            {"version", 1},
            {"tool", {{"name", "pelican-import-tools"}, {"version", "0.1.0"}}},
            {"source", {{"file", "coat.usda"}}},
            {"outputs", {std::move(output)}},
        };
    };
    CHECK(parseImportManifestJson(materialManifest({{"version", 1}})).outputs.size() == 1);
    CHECK_THROWS_WITH(parseImportManifestJson(materialManifest({})),
                      Catch::Matchers::ContainsSubstring("requires version 1"));
    CHECK_THROWS_WITH(parseImportManifestJson(materialManifest({{"version", 2}})),
                      Catch::Matchers::ContainsSubstring("expected=1, actual=2"));
}

} // namespace Pelican
