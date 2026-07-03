#include "../src/core/loader/importmanifest.hpp"

#include <catch2/catch_test_macros.hpp>
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

} // namespace Pelican
