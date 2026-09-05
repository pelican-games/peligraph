#include "../src/project/assetdataformat.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

namespace Pelican {

TEST_CASE("WP386a asset_data v2 owns one fixed AABB per procedural asset",
          "[WP386a][asset-data][procedural]") {
    const auto parsed = parseAssetDataFormatJson(nlohmann::json::parse(R"json({
      "schema":"pelican.asset_data","version":2,
      "models":[],"materials":[],"textures":[],
      "procedural_geometry":[{
        "name":"sphere","kind":"aabb_v1",
        "aabb":{"min":[-1.0,-1.0,-1.0],"max":[1.0,1.0,1.0]},
        "record":{"radius":1.0,"tag":7}
      }]
    })json"));
    REQUIRE(parsed.version == 2);
    REQUIRE(parsed.procedural_geometry.size() == 1);
    CHECK(parsed.procedural_geometry[0].name == "sphere");
    CHECK(parsed.procedural_geometry[0].aabb_min[0] == -1.0F);
    CHECK(parsed.procedural_geometry[0].aabb_max[2] == 1.0F);
    CHECK(parsed.procedural_geometry[0].record.at("tag") == 7);

    auto v1 = nlohmann::json::parse(R"json({
      "schema":"pelican.asset_data","version":1,
      "models":[],"materials":[],"textures":[],
      "procedural_geometry":[]
    })json");
    CHECK_THROWS_WITH(parseAssetDataFormatJson(v1),
                      Catch::Matchers::ContainsSubstring(
                          "pelican.asset_data_procedural_requires_v2@1"));

    auto entries = nlohmann::json::parse(R"json({
      "schema":"pelican.asset_data","version":2,
      "models":[],"materials":[],"textures":[],
      "procedural_geometry":[{"name":"sphere","kind":"aabb_v1",
        "aabb":{"min":[-1,-1,-1],"max":[1,1,1]},
        "record":{},"entries":[]}]
    })json");
    CHECK_THROWS_WITH(parseAssetDataFormatJson(entries),
                      Catch::Matchers::ContainsSubstring(
                          "pelican.procedural_geometry_entries_unsupported@1"));

    auto degenerate = nlohmann::json::parse(R"json({
      "schema":"pelican.asset_data","version":2,
      "models":[],"materials":[],"textures":[],
      "procedural_geometry":[{"name":"thin","kind":"aabb_v1",
        "aabb":{"min":[0,0,0],"max":[0.001,1,1]},"record":{}}]
    })json");
    CHECK_THROWS_WITH(parseAssetDataFormatJson(degenerate),
                      Catch::Matchers::ContainsSubstring(
                          "pelican.procedural_aabb_degenerate@1"));
}

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
    CHECK(parseAssetDataFormatJson(
              nlohmann::json{
                  {"schema", "pelican.asset_data"},
                  {"version", 2},
                  {"models", nlohmann::json::array()},
              })
              .version == 2);
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
