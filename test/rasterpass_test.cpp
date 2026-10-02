#include "../src/project/rasterpass.hpp"
#include "../src/project/graphcomposition.hpp"
#include "../src/project/graphmodule.hpp"
#include "../src/project/passfieldownership.hpp"
#include "../src/project/rendergraphcomposition.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <array>

using namespace Pelican;

TEST_CASE("indexed raster contracts retain buffer and range", "[raster-index]") {
    nlohmann::json document{{"draw", {{"operation", "indexed"}, {"index_buffer", "mesh"},
        {"index_count", 6}, {"first_index", 3}, {"instance_count", 2}, {"first_instance", 1}}}};
    const auto contract = parseRasterPassContract(document, 1, false);
    const auto &draw = std::get<RasterIndexedDrawOperation>(contract.geometry.operation);
    REQUIRE(draw.index_buffer == "mesh");
    REQUIRE(draw.index_count == 6);
    REQUIRE(draw.first_index == 3);
    REQUIRE(draw.instance_count == 2);
    REQUIRE(draw.first_instance == 1);
    REQUIRE(parseRasterPassContract(rasterPassContractToJson(contract), 1, false) == contract);
    document["draw"]["vertex_count"] = 4;
    REQUIRE_THROWS(parseRasterPassContract(document, 1, false));
    document["draw"].erase("vertex_count");
    document["draw"]["index_count"] = 0;
    REQUIRE_THROWS(parseRasterPassContract(document, 1, false));
}

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
                     {"operation", "mesh"},
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

TEST_CASE(
    "public fixed-function parser is shared and distinguishes additive from explicit one-one",
    "[raster][fixed-state][wp353]") {
    const auto pass = nlohmann::json::parse(R"json({
      "draw": {"vertex_count": 3},
      "raster_state": {
        "topology": "triangle_strip",
        "cull": "back",
        "front_face": "clockwise",
        "color_attachments": [{"blend": "additive", "write_mask": "rg"}]
      }
    })json");
    const auto fixed =
        parseRasterFixedFunctionState(pass, 1, "shared fixed state");
    const auto contract =
        parseRasterPassContract(pass, 1, false, "shared fixed state");
    REQUIRE(fixed == contract.state);
    REQUIRE(fixed.color_attachments.size() == 1);
    const auto &additive = fixed.color_attachments.front().blend;
    CHECK(additive.enabled);
    CHECK(
        additive.color.source ==
        MaterialOutputBlendFactor::source_alpha);
    CHECK(
        additive.color.destination ==
        MaterialOutputBlendFactor::one);

    auto explicit_one_one = pass;
    explicit_one_one["raster_state"]["color_attachments"][0]["blend"] = {
        {"color", {{"src", "one"}, {"dst", "one"}, {"op", "add"}}},
        {"alpha", {{"src", "one"}, {"dst", "one"}, {"op", "add"}}},
    };
    const auto explicit_state =
        parseRasterFixedFunctionState(
            explicit_one_one, 1, "explicit one-one");
    const auto &one_one =
        explicit_state.color_attachments.front().blend;
    CHECK(one_one.enabled);
    CHECK(
        one_one.color.source ==
        MaterialOutputBlendFactor::one);
    CHECK(
        one_one.color.destination ==
        MaterialOutputBlendFactor::one);
    CHECK(additive != one_one);
}

TEST_CASE(
    "shared raster numeric-class validation rejects integer blend targets",
    "[raster][fixed-state][integer][wp353]") {
    const auto state = parseRasterFixedFunctionState(
        nlohmann::json{
            {"raster_state",
             {{"color_attachments",
               nlohmann::json::array(
                   {{{"blend", "additive"}}})}}}},
        1, "numeric-class fixture");
    const std::array floating{
        MaterialOutputNumericClass::floating};
    const std::array integer{
        MaterialOutputNumericClass::unsigned_integer};

    CHECK_NOTHROW(
        validateRasterColorAttachmentNumericClasses(
            state.color_attachments, floating,
            "Fullscreen pass 'floating'"));
    CHECK_THROWS_WITH(
        validateRasterColorAttachmentNumericClasses(
            state.color_attachments, integer,
            "Fullscreen pass 'picking_overlay'"),
        Catch::Matchers::ContainsSubstring(
            "Fullscreen pass 'picking_overlay'") &&
            Catch::Matchers::ContainsSubstring(
                "integer color output 0"));
}

// CC-2: clip_y is a raster pass's own field (down unless written), round
// trips through the canonical JSON, changes the pipeline's identity and an
// unknown value is refused as raster_state's are. A fullscreen pass does not
// own it. The graph's render.raster lists it in the catalog, refuses an
// unknown value by name, and lowers a node without it to a pass without it.
TEST_CASE("CC-2 a raster pass's clip_y is down unless written, and only render.raster takes it",
          "[raster][cc2]") {
    const nlohmann::json pass{{"draw", {{"vertex_count", 3}}}};
    const auto down = parseRasterPassContract(pass, 1, false);
    CHECK(down.clip_y == RasterClipY::down);
    auto written = pass;
    written["clip_y"] = "up";
    const auto up = parseRasterPassContract(written, 1, false);
    CHECK(up.clip_y == RasterClipY::up);
    CHECK(up.state == down.state);
    CHECK(rasterPassContractToJson(up).at("clip_y") == "up");
    CHECK(rasterPassContractToJson(down).at("clip_y") == "down");
    CHECK(parseRasterPassContract(rasterPassContractToJson(up), 1, false) == up);
    CHECK(rasterPassContractFingerprint(up) != rasterPassContractFingerprint(down));
    written["clip_y"] = "sideways";
    CHECK_THROWS_WITH(parseRasterPassContract(written, 1, false, "raster pass 'flip'"),
                      "raster pass 'flip' field 'clip_y' has unknown value 'sideways'");
    written["clip_y"] = true;
    CHECK_THROWS_WITH(parseRasterPassContract(written, 1, false, "raster pass 'flip'"),
                      "raster pass 'flip' field 'clip_y' must be a string");
    // Inside raster_state it is an unknown field, as before.
    CHECK_THROWS_WITH(parseRasterPassContract(nlohmann::json{{"draw", nlohmann::json::object()},
                                                             {"raster_state", {{"clip_y", "up"}}}},
                                              1, false, "raster pass 'flip'"),
                      Catch::Matchers::ContainsSubstring("unknown field 'clip_y'"));
    CHECK_THROWS_WITH(validatePassFieldOwnership(nlohmann::json{{"name", "present"}, {"type", "fullscreen"},
                                                                {"clip_y", "up"}},
                                                 PassFieldOwnershipCapabilities{}),
                      "Pass 'present' type 'fullscreen' does not own field 'clip_y'");
    CHECK(validatePassFieldOwnership(nlohmann::json{{"name", "flip"}, {"type", "raster"}, {"clip_y", "up"}},
                                     PassFieldOwnershipCapabilities{}) == RenderPassType::raster);

    using Json = nlohmann::json;
    // The catalog: an enum, down by default; render.fullscreen has none.
    const auto catalog = Json(builtinGraphModuleRegistry().catalog());
    const auto parameters = [&](std::string_view op) {
        for (const auto &module : catalog.at("modules"))
            for (const auto &entry : module.at("operations"))
                if (entry.at("op").get<std::string>() == std::string{op}) return entry.at("parameters");
        FAIL("the catalog lists no " << op);
        return Json{};
    };
    const auto find = [](const Json &listed, std::string_view name) {
        for (const auto &entry : listed)
            if (entry.at("name").get<std::string>() == std::string{name}) return entry;
        return Json{};
    };
    const auto clip_y = find(parameters("render.raster"), "clip_y");
    CHECK(clip_y.at("kind") == "enum");
    CHECK(clip_y.at("enum") == Json::array({"down", "up"}));
    CHECK(clip_y.at("default") == "down");
    CHECK(find(parameters("render.fullscreen"), "clip_y").is_null());

    const Json type{{"semantic", "pelican.render.color_signal@1"},
                    {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}};
    Json document{{"schema", "pelican.graph"}, {"version", 1}, {"definitions", Json::object()},
        {"nodes", Json::array({
            {{"id", "canvas"}, {"op", "render.image"},
             {"parameters", {{"type", type}, {"format", "R8G8B8A8_UNORM"}, {"extent_scale", 1.0},
                             {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}}},
             {"outputs", {"resource"}}},
            {{"id", "flip"}, {"op", "render.raster"},
             {"parameters", {{"shader", {{"vertex", "project://shaders/flip"}, {"fragment", "project://shaders/flip"}}},
                             {"draw", {{"vertex_count", 3}}},
                             {"ports", {{"color", {{"kind", "image"}, {"type", type}, {"access", "write"},
                                                   {"attachment", "color"}, {"location", 0}}}}}}},
             {"inputs", {{"color", {{"node", "canvas"}, {"port", "resource"}}}}},
             {"outputs", {"color"}}}})},
        {"outputs", Json::object()}};
    const auto lowered = [&](const Json &graph) {
        const auto config = lowerRenderGraphDocument(graph);
        for (const auto &family : config.at("rendering_passes"))
            for (const auto &lowered_pass : family.at("passes"))
                if (lowered_pass.at("name") == "flip") return lowered_pass;
        FAIL("no pass flip");
        return Json{};
    };
    // Not written: the pass is what it was, with no clip_y.
    const auto plain = lowered(document);
    CHECK_FALSE(plain.contains("clip_y"));
    CHECK(parseRasterPassContract(plain, 1, false).clip_y == RasterClipY::down);
    document["nodes"][1]["parameters"]["clip_y"] = "up";
    const auto flipped = lowered(document);
    CHECK(flipped.at("clip_y") == "up");
    auto without = flipped;
    without.erase("clip_y");
    CHECK(without == plain);
    CHECK(parseRasterPassContract(flipped, 1, false).clip_y == RasterClipY::up);
    document["nodes"][1]["parameters"]["clip_y"] = "sideways";
    try {
        (void)builtinGraphModuleRegistry().resolveOperation(document["nodes"][1]);
        FAIL("clip_y sideways was taken");
    } catch (const GraphRegistryError &error) {
        CHECK(error.code() == "graph.render_node_parameter_invalid@1");
        CHECK(error.detail() == Json{{"node", "flip"}, {"op", "render.raster"}, {"parameter", "clip_y"},
                                     {"expected", "enum"}, {"reason", "enum"},
                                     {"allowed", Json::array({"down", "up"})}});
    }
}
