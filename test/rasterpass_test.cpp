#include "../src/project/rasterpass.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

using namespace Pelican;

TEST_CASE("generic raster contract canonicalizes portable draw and attachment state") {
    const auto contract = parseRasterPassContract(
        nlohmann::json{
            {"draw",
             {
                 {"implementation",
                  "project.sky_draw@3"},
                 {"operation", "direct"},
                 {"vertex_count", 36},
                 {"instance_count", 2},
                 {"first_vertex", 4},
                 {"first_instance", 7},
             }},
            {"raster_state",
             {
                 {"topology", "triangle_strip"},
                 {"cull", "back"},
                 {"front_face", "clockwise"},
                 {"depth_test", true},
                 {"depth_write", true},
                 {"depth_compare", "greater_equal"},
                 {"color_attachments",
                  nlohmann::json::array(
                      {
                          {
                              {"blend", "opaque"},
                              {"write_mask", "rgb"},
                          },
                          {
                              {"blend", "additive"},
                              {"write_mask", "ra"},
                          },
                      })},
             }},
        },
        2, true, "test raster");

    REQUIRE(
        contract.geometry.implementation ==
        "project.sky_draw@3");
    const auto &draw =
        std::get<RasterDirectDrawOperation>(
            contract.geometry.operation);
    CHECK(draw.vertex_count == 36);
    CHECK(draw.instance_count == 2);
    CHECK(draw.first_vertex == 4);
    CHECK(draw.first_instance == 7);
    CHECK(
        contract.state.topology ==
        RasterPrimitiveTopology::triangle_strip);
    CHECK(
        contract.state.cull ==
        RasterCullMode::back);
    CHECK(
        contract.state.front_face ==
        RasterFrontFace::clockwise);
    CHECK(contract.state.depth.test);
    CHECK(contract.state.depth.write);
    CHECK(
        contract.state.depth.compare ==
        RasterDepthCompare::greater_equal);
    REQUIRE(
        contract.state.color_attachments.size() == 2);
    CHECK(
        contract.state.color_attachments[0].write_mask ==
        (materialOutputWriteRed |
         materialOutputWriteGreen |
         materialOutputWriteBlue));
    CHECK_FALSE(
        contract.state.color_attachments[0]
            .blend.enabled);
    CHECK(
        contract.state.color_attachments[1]
            .blend.enabled);

    const auto encoded =
        rasterPassContractToJson(contract);
    CHECK(
        encoded.at("draw").at("operation") ==
        "direct");
    CHECK(
        encoded.at("raster_state")
            .at("color_attachments")
            .size() == 2);
    CHECK(
        rasterPassContractFingerprint(contract)
            .starts_with(
                "pelican.raster.pass@1:"));
}

TEST_CASE("generic raster contract has no authored attachment-count ceiling") {
    constexpr std::size_t attachment_count = 19;
    const auto contract = parseRasterPassContract(
        nlohmann::json{
            {"draw",
             {
                 {"operation", "direct"},
                 {"vertex_count", 3},
             }},
        },
        attachment_count, false);

    REQUIRE(
        contract.state.color_attachments.size() ==
        attachment_count);
    for (const auto &attachment :
         contract.state.color_attachments) {
        CHECK_FALSE(attachment.blend.enabled);
        CHECK(
            attachment.write_mask ==
            materialOutputWriteRgba);
    }
}

TEST_CASE("generic raster contract rejects malformed typed operations and state") {
    CHECK_THROWS_WITH(
        parseRasterPassContract(
            nlohmann::json{
                {"draw",
                 {
                     {"implementation", "unversioned"},
                 }},
            },
            1, false),
        Catch::Matchers::ContainsSubstring(
            "namespace.name@major"));

    CHECK_THROWS_WITH(
        parseRasterPassContract(
            nlohmann::json{
                {"draw",
                 {
                     {"vertex_count", 0},
                 }},
            },
            1, false),
        Catch::Matchers::ContainsSubstring(
            "vertex_count must be non-zero"));

    CHECK_THROWS_WITH(
        parseRasterPassContract(
            nlohmann::json{
                {"draw", nlohmann::json::object()},
                {"raster_state",
                 {
                     {"depth_test", true},
                 }},
            },
            1, false),
        Catch::Matchers::ContainsSubstring(
            "without a depth attachment"));

    CHECK_THROWS_WITH(
        parseRasterPassContract(
            nlohmann::json{
                {"draw", nlohmann::json::object()},
                {"raster_state",
                 {
                     {"color_attachments",
                      nlohmann::json::array(
                          {{{"blend", "opaque"}}})},
                 }},
            },
            2, false),
        Catch::Matchers::ContainsSubstring(
            "count must match"));

    CHECK_THROWS_WITH(
        parseRasterPassContract(
            nlohmann::json{
                {"draw",
                 {
                     {"operation", "indexed"},
                 }},
            },
            1, false),
        Catch::Matchers::ContainsSubstring(
            "unsupported typed operation"));
}

TEST_CASE("generic raster fingerprint changes with pipeline-relevant state") {
    const auto base = parseRasterPassContract(
        nlohmann::json{
            {"draw", nlohmann::json::object()},
        },
        1, false);
    auto changed = base;
    changed.state.cull = RasterCullMode::back;
    CHECK(
        rasterPassContractFingerprint(base) !=
        rasterPassContractFingerprint(changed));
    changed = base;
    std::get<RasterDirectDrawOperation>(
        changed.geometry.operation)
        .instance_count = 2;
    CHECK(
        rasterPassContractFingerprint(base) !=
        rasterPassContractFingerprint(changed));
}
