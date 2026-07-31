#include "../src/project/assetdataformat.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

namespace Pelican {

TEST_CASE(
    "pelican.asset_data v1 is strict and indexes material documents by path",
    "[asset-data][wp240c]") {
    const auto parsed =
        parseAssetDataFormatJson(
            nlohmann::json::parse(R"json({
              "schema":"pelican.asset_data",
              "version":1,
              "models":[{
                "name":"helmet",
                "path":"assets/helmet.glb",
                "material_bindings":
                  "materials/helmet.bindings.json"
              }],
              "materials":[{
                "path":"materials/metal.material.json"
              }],
              "textures":[]
            })json"));
    REQUIRE(parsed.models.size() == 1);
    REQUIRE(parsed.models.front().name ==
            "helmet");
    REQUIRE(
        parsed.models.front().material_bindings ==
        "materials/helmet.bindings.json");
    REQUIRE(parsed.materials.size() == 1);
    REQUIRE(parsed.materials.front().path ==
            "materials/metal.material.json");

    REQUIRE_THROWS_WITH(
        parseAssetDataFormatJson(
            nlohmann::json{
                {"version", 1},
                {"models",
                 nlohmann::json::array()},
            }),
        Catch::Matchers::ContainsSubstring(
            "pelican.asset_data"));
    REQUIRE_THROWS_WITH(
        parseAssetDataFormatJson(
            nlohmann::json{
                {"schema",
                 "pelican.asset_data"},
                {"models",
                 nlohmann::json::array()},
            }),
        Catch::Matchers::ContainsSubstring(
            "version must be exactly 1"));
    REQUIRE_THROWS_WITH(
        parseAssetDataFormatJson(
            nlohmann::json{
                {"schema",
                 "pelican.asset_data"},
                {"version", 2},
                {"models",
                 nlohmann::json::array()},
            }),
        Catch::Matchers::ContainsSubstring(
            "version must be exactly 1"));
    REQUIRE_THROWS_WITH(
        parseAssetDataFormatJson(
            nlohmann::json{
                {"schema",
                 "pelican.asset_data"},
                {"version", 1},
                {"models",
                 nlohmann::json::array()},
                {"materials",
                 nlohmann::json::array(
                     {{{"path", "a.material.json"},
                       {"name", "duplicated"}}})},
            }),
        Catch::Matchers::ContainsSubstring(
            "unknown key 'name'"));
    REQUIRE_THROWS_WITH(
        parseAssetDataFormatJson(
            nlohmann::json{
                {"schema",
                 "pelican.asset_data"},
                {"version", 1},
                {"models",
                 nlohmann::json::array()},
                {"legacy", true},
            }),
        Catch::Matchers::ContainsSubstring(
            "unknown key 'legacy'"));
}

} // namespace Pelican
