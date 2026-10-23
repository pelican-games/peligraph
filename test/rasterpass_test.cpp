#include "../src/project/rasterpass.hpp"
#include "../src/project/graphcomposition.hpp"
#include "../src/project/graphmodule.hpp"
#include "../src/project/passfieldownership.hpp"
#include "../src/project/rendergraphcomposition.hpp"
#include "../src/project/renderpipeline.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

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

// PSN-1 A2: raster_state reads the whole pipeline-state vocabulary into the
// one PipelineState, and writing it back reads back to the same value.
TEST_CASE("PSN-1 raster_state reads every state group into PipelineState and writes it back",
          "[raster][psn1]") {
    using Json = nlohmann::json;
    const auto pass = Json::parse(R"json({
      "draw": {"vertex_count": 4},
      "raster_state": {
        "topology": "triangle_strip",
        "primitive_restart": true,
        "cull": "back",
        "front_face": "clockwise",
        "polygon_mode": "line",
        "depth_clamp": true,
        "depth_bias": {"constant": -2.5, "slope": 1.25, "clamp": 0.01},
        "line_width": 3.0,
        "rasterizer_discard": false,
        "conservative": "overestimate",
        "sample_shading": 0.5,
        "sample_mask": 15,
        "alpha_to_coverage": true,
        "alpha_to_one": true,
        "depth_test": true,
        "depth_write": false,
        "depth_compare": "less_equal",
        "depth_bounds": [0.25, 0.75],
        "stencil": {
          "front": {"fail": "keep", "pass": "replace", "depth_fail": "invert",
                    "compare": "always", "compare_mask": 255, "write_mask": 15, "reference": 1},
          "back": {"pass": "increment_and_wrap", "compare": "equal", "reference": 2}
        },
        "logic_op": "xor",
        "blend_constants": [0.1, 0.2, 0.3, 0.4],
        "color_attachments": [{"blend": "opaque", "write_mask": "rgba"}]
      }
    })json");
    const auto contract = parseRasterPassContract(pass, 1, true, "raster pass 'all'");
    const auto &state = contract.state;
    CHECK(state.topology == RasterPrimitiveTopology::triangle_strip);
    CHECK(state.primitive_restart);
    CHECK(state.cull == RasterCullMode::back);
    CHECK(state.front_face == RasterFrontFace::clockwise);
    CHECK(state.polygon_mode == RasterPolygonMode::line);
    CHECK(state.depth_clamp);
    REQUIRE(state.depth_bias.has_value());
    CHECK(*state.depth_bias == RasterDepthBias{-2.5, 1.25, 0.01});
    CHECK(state.line_width == 3.0);
    CHECK_FALSE(state.rasterizer_discard);
    CHECK(state.conservative == RasterConservativeMode::overestimate);
    CHECK(state.sample_shading == std::optional<double>{0.5});
    CHECK(state.sample_mask == 15u);
    CHECK(state.alpha_to_coverage);
    CHECK(state.alpha_to_one);
    CHECK(state.depth.test);
    CHECK_FALSE(state.depth.write);
    CHECK(state.depth.compare == RasterDepthCompare::less_equal);
    CHECK(state.depth.bounds == std::optional{RasterDepthBounds{0.25, 0.75}});
    REQUIRE(state.stencil.has_value());
    CHECK(state.stencil->front == RasterStencilFaceState{
        .fail = RasterStencilOperation::keep, .pass = RasterStencilOperation::replace,
        .depth_fail = RasterStencilOperation::invert, .compare = RasterDepthCompare::always,
        .compare_mask = 255, .write_mask = 15, .reference = 1});
    // An unwritten face field keeps its default (keep, always, 0xff masks, 0).
    CHECK(state.stencil->back == RasterStencilFaceState{
        .pass = RasterStencilOperation::increment_and_wrap,
        .compare = RasterDepthCompare::equal, .reference = 2});
    CHECK(state.logic_op == std::optional{RasterLogicOperation::bitwise_xor});
    CHECK(state.blend_constants == std::array{0.1, 0.2, 0.3, 0.4});

    // Back to JSON and in again: the same value, through both entry points.
    const auto encoded = rasterPassContractToJson(contract);
    CHECK(parseRasterPassContract(encoded, 1, true, "raster pass 'all'") == contract);
    CHECK(parsePipelineState(pipelineStateToJson(state), 1) == state);
    CHECK(encoded.at("raster_state").at("logic_op") == "xor");
    CHECK(encoded.at("raster_state").at("stencil").at("back").at("compare_mask") == 255);
    // The canonical form is a fixed point.
    CHECK(pipelineStateToJson(parsePipelineState(pipelineStateToJson(state), 1)) ==
          pipelineStateToJson(state));

    // Dual-source and constant blend factors are blend factors like the rest.
    const auto dual = parseRasterFixedFunctionState(
        Json::parse(R"json({"raster_state": {"color_attachments": [{"blend": {
          "color": {"src": "src1_color", "dst": "one_minus_src1_color", "op": "add"},
          "alpha": {"src": "constant_alpha", "dst": "one_minus_constant_alpha", "op": "add"}}}]}})json"),
        1, "raster pass 'dual'");
    const auto &blend = dual.color_attachments.front().blend;
    CHECK(blend.color.source == MaterialOutputBlendFactor::source1_color);
    CHECK(blend.color.destination == MaterialOutputBlendFactor::one_minus_source1_color);
    CHECK(blend.alpha.source == MaterialOutputBlendFactor::constant_alpha);
    CHECK(blend.alpha.destination == MaterialOutputBlendFactor::one_minus_constant_alpha);
    CHECK(materialOutputBlendStateUsesDualSource(blend));
    CHECK(parsePipelineState(pipelineStateToJson(dual), 1) == dual);

    // Nothing written: the engine's defaults, not Vulkan's.
    const auto plain = parseRasterPassContract(Json{{"draw", Json::object()}}, 1, false).state;
    PipelineState defaults;
    defaults.color_attachments.resize(1);
    CHECK(plain == defaults);
    CHECK(plain.polygon_mode == RasterPolygonMode::fill);
    CHECK(plain.line_width == 1.0);
    CHECK_FALSE(plain.depth_bias.has_value());
    CHECK_FALSE(plain.stencil.has_value());
    CHECK_FALSE(plain.logic_op.has_value());
    CHECK(plain.sample_mask == rasterSampleMaskAll);
    CHECK_FALSE(pipelineStateToJson(plain).contains("stencil"));

    // The fingerprint (the cache / reload identity) sees the new groups.
    auto changed = contract;
    changed.state.stencil->front.reference = 3;
    CHECK(rasterPassContractFingerprint(changed) != rasterPassContractFingerprint(contract));
    changed = contract;
    changed.state.line_width = 2.0;
    CHECK(rasterPassContractFingerprint(changed) != rasterPassContractFingerprint(contract));
}

// PSN-1 A2: unknown keys and values out of range are refused by name, and so
// is state the pass's attachments cannot carry.
TEST_CASE("PSN-1 raster_state refuses unknown keys, out-of-range values and missing attachments by name",
          "[raster][psn1]") {
    using Json = nlohmann::json;
    const auto refusal = [](Json raster_state, bool depth = true) {
        try {
            (void)parseRasterPassContract(Json{{"draw", Json::object()}, {"raster_state", std::move(raster_state)}},
                                          1, depth, "raster pass 'p'");
        } catch (const std::runtime_error &error) {
            return std::string{error.what()};
        }
        return std::string{"<accepted>"};
    };
    using Catch::Matchers::ContainsSubstring;
    CHECK_THAT(refusal({{"polygon_mod", "line"}}), ContainsSubstring("raster_state has unknown field 'polygon_mod'"));
    CHECK_THAT(refusal({{"polygon_mode", "wire"}}),
               ContainsSubstring("field 'polygon_mode' has unknown value 'wire'"));
    CHECK_THAT(refusal({{"conservative", "on"}}), ContainsSubstring("field 'conservative' has unknown value 'on'"));
    CHECK_THAT(refusal({{"logic_op", "nxor"}}), ContainsSubstring("field 'logic_op' has unknown value 'nxor'"));
    CHECK_THAT(refusal({{"depth_bias", {{"constant", 1}, {"units", 2}}}}),
               ContainsSubstring("depth_bias has unknown field 'units'"));
    CHECK_THAT(refusal({{"depth_bias", {{"slope", "1"}}}}), ContainsSubstring("field 'slope' must be a number"));
    CHECK_THAT(refusal({{"line_width", 0}}), ContainsSubstring("field 'line_width' must be greater than 0"));
    CHECK_THAT(refusal({{"line_width", "3"}}), ContainsSubstring("field 'line_width' must be a number"));
    CHECK_THAT(refusal({{"sample_shading", 1.5}}), ContainsSubstring("field 'sample_shading' must be in [0, 1]"));
    CHECK_THAT(refusal({{"sample_mask", -1}}), ContainsSubstring("field 'sample_mask' must be an unsigned integer"));
    CHECK_THAT(refusal({{"depth_bounds", {0.8, 0.2}}}), ContainsSubstring("field 'depth_bounds' must be [min, max]"));
    CHECK_THAT(refusal({{"depth_bounds", {0.0, 1.5}}}), ContainsSubstring("field 'depth_bounds' must be [min, max]"));
    CHECK_THAT(refusal({{"depth_bounds", {0.0, 0.5, 1.0}}}),
               ContainsSubstring("field 'depth_bounds' must be an array of 2 numbers"));
    CHECK_THAT(refusal({{"blend_constants", {1, 1, 1}}}),
               ContainsSubstring("field 'blend_constants' must be an array of 4 numbers"));
    const Json face{{"pass", "replace"}};
    CHECK_THAT(refusal({{"stencil", {{"front", face}}}}), ContainsSubstring("stencil requires both front and back"));
    CHECK_THAT(refusal({{"stencil", {{"front", face}, {"back", face}, {"both", face}}}}),
               ContainsSubstring("stencil has unknown field 'both'"));
    CHECK_THAT(refusal({{"stencil", {{"front", {{"reference", 256}}}, {"back", face}}}}),
               ContainsSubstring("stencil front field 'reference' must be at most 255"));
    CHECK_THAT(refusal({{"stencil", {{"front", face}, {"back", {{"compare", "eq"}}}}}}),
               ContainsSubstring("stencil back field 'compare' has unknown value 'eq'"));
    CHECK_THAT(refusal({{"stencil", {{"front", {{"op", "keep"}}}, {"back", face}}}}),
               ContainsSubstring("stencil front has unknown field 'op'"));
    CHECK_THAT(refusal({{"primitive_restart", true}}),
               ContainsSubstring("field 'primitive_restart' requires topology line_strip or triangle_strip"));
    CHECK(refusal({{"primitive_restart", true}, {"topology", "line_strip"}}) == "<accepted>");
    CHECK_THAT(refusal({{"logic_op", "copy"}, {"color_attachments", Json::array({{{"blend", "blend"}}})}}),
               ContainsSubstring("field 'logic_op' replaces blending, but color_attachments[0] enables blend"));
    // Depth-dependent state needs a depth attachment; depth clamp does not.
    CHECK_THAT(refusal({{"stencil", {{"front", face}, {"back", face}}}}, false),
               ContainsSubstring("enables stencil without a depth attachment"));
    CHECK_THAT(refusal({{"depth_bias", {{"constant", 1}}}}, false),
               ContainsSubstring("enables depth_bias without a depth attachment"));
    CHECK_THAT(refusal({{"depth_bounds", {0.0, 1.0}}}, false),
               ContainsSubstring("enables depth_bounds without a depth attachment"));
    CHECK(refusal({{"depth_clamp", true}}, false) == "<accepted>");
    // A fullscreen pass owns no depth attachment: its depth_* keys stay
    // refused, and so is stencil.
    CHECK_THROWS_WITH(parseFullscreenRasterFixedFunctionState(
                          Json{{"raster_state", {{"depth_bias", {{"constant", 1}}}}}}, "fullscreen pass 'f'"),
                      ContainsSubstring("does not support depth field 'depth_bias'"));
    CHECK_THROWS_WITH(parseFullscreenRasterFixedFunctionState(
                          Json{{"raster_state", {{"stencil", {{"front", face}, {"back", face}}}}}},
                          "fullscreen pass 'f'"),
                      ContainsSubstring("fullscreen pass 'f' enables stencil without a depth attachment"));
    CHECK(parseFullscreenRasterFixedFunctionState(Json{{"raster_state", {{"polygon_mode", "line"}}}})
              .polygon_mode == RasterPolygonMode::line);

    // A depth attachment whose format has no stencil component cannot carry
    // a stencil test; the lowering names the pass and the format.
    const auto stencil = parseRasterPassContract(
        Json{{"draw", Json::object()}, {"raster_state", {{"stencil", {{"front", face}, {"back", face}}}}}}, 1, true);
    CHECK_THROWS_WITH(validatePipelineStateStencilFormat(stencil.state, false, "D32_SFLOAT", "raster pass 'mask'"),
                      "raster pass 'mask' enables stencil, but its depth attachment format 'D32_SFLOAT' has no "
                      "stencil component (use D24_UNORM_S8_UINT)");
    CHECK_NOTHROW(validatePipelineStateStencilFormat(stencil.state, true, "D24_UNORM_S8_UINT", "raster pass 'mask'"));
    CHECK_NOTHROW(validatePipelineStateStencilFormat(PipelineState{}, false, "D32_SFLOAT", "raster pass 'plain'"));
}

// PSN-1 A3: the device facts decide; a missing feature is refused as
// pipeline.state_unsupported@1 {pass, field, feature}.
TEST_CASE("PSN-1 state a device lacks the feature for is refused as pipeline.state_unsupported@1",
          "[raster][psn1]") {
    using Json = nlohmann::json;
    const PipelineStateDeviceFacts all{
        .fill_mode_non_solid = true, .wide_lines = true, .depth_clamp = true, .depth_bias_clamp = true,
        .depth_bounds = true, .sample_rate_shading = true, .alpha_to_one = true, .logic_op = true,
        .dual_src_blend = true, .max_fragment_dual_src_attachments = 1, .conservative_rasterization = true,
        .primitive_underestimation = true, .conservative_point_and_line_rasterization = true};
    const auto line = parseRasterPassContract(
        Json{{"draw", {{"vertex_count", 3}}}, {"raster_state", {{"polygon_mode", "line"}}}}, 1, false,
        "raster pass 'wire'");
    CHECK_NOTHROW(requirePipelineStateSupported(line.state, all, "wire"));
    auto without = all;
    without.fill_mode_non_solid = false;
    try {
        requirePipelineStateSupported(line.state, without, "wire");
        FAIL("polygon_mode line was taken without fillModeNonSolid");
    } catch (const std::runtime_error &error) {
        const auto coded = splitCodedRefusalMessage(error.what());
        REQUIRE(coded.has_value());
        CHECK(coded->code == "pipeline.state_unsupported@1");
        CHECK(coded->fields == Json{{"pass", "wire"}, {"field", "polygon_mode"}, {"feature", "fillModeNonSolid"}});
    }
    // The engine's defaults need nothing optional.
    PipelineState defaults;
    defaults.color_attachments.resize(1);
    CHECK_FALSE(firstUnsupportedPipelineState(defaults, PipelineStateDeviceFacts{}).has_value());

    // Every requirement, each against the one fact it needs.
    struct Case {
        const char *name;
        Json raster_state;
        bool PipelineStateDeviceFacts::*fact;
        const char *field;
        const char *feature;
    };
    const Json dual_blend{{"color", {{"src", "one"}, {"dst", "src1_color"}, {"op", "add"}}},
                          {"alpha", {{"src", "one"}, {"dst", "zero"}, {"op", "add"}}}};
    const std::vector<Case> cases{
        {"point", {{"polygon_mode", "point"}}, &PipelineStateDeviceFacts::fill_mode_non_solid, "polygon_mode",
         "fillModeNonSolid"},
        {"clamp", {{"depth_clamp", true}}, &PipelineStateDeviceFacts::depth_clamp, "depth_clamp", "depthClamp"},
        {"bias clamp", {{"depth_bias", {{"constant", 1}, {"clamp", 0.5}}}},
         &PipelineStateDeviceFacts::depth_bias_clamp, "depth_bias", "depthBiasClamp"},
        {"wide", {{"line_width", 3}}, &PipelineStateDeviceFacts::wide_lines, "line_width", "wideLines"},
        {"conservative", {{"conservative", "overestimate"}},
         &PipelineStateDeviceFacts::conservative_rasterization, "conservative",
         "VK_EXT_conservative_rasterization"},
        {"underestimate", {{"conservative", "underestimate"}},
         &PipelineStateDeviceFacts::primitive_underestimation, "conservative", "primitiveUnderestimation"},
        {"conservative lines", {{"conservative", "overestimate"}, {"topology", "line_list"}},
         &PipelineStateDeviceFacts::conservative_point_and_line_rasterization, "conservative",
         "conservativePointAndLineRasterization"},
        {"sample shading", {{"sample_shading", 1.0}}, &PipelineStateDeviceFacts::sample_rate_shading,
         "sample_shading", "sampleRateShading"},
        {"alpha to one", {{"alpha_to_one", true}}, &PipelineStateDeviceFacts::alpha_to_one, "alpha_to_one",
         "alphaToOne"},
        {"bounds", {{"depth_bounds", {0.0, 0.5}}}, &PipelineStateDeviceFacts::depth_bounds, "depth_bounds",
         "depthBounds"},
        {"logic", {{"logic_op", "invert"}}, &PipelineStateDeviceFacts::logic_op, "logic_op", "logicOp"},
        {"dual source", {{"color_attachments", Json::array({{{"blend", dual_blend}}})}},
         &PipelineStateDeviceFacts::dual_src_blend, "color_attachments", "dualSrcBlend"},
    };
    for (const auto &entry : cases) {
        DYNAMIC_SECTION(entry.name) {
            const auto state = parseRasterPassContract(
                Json{{"draw", Json::object()}, {"raster_state", entry.raster_state}}, 1, true).state;
            CHECK_FALSE(firstUnsupportedPipelineState(state, all).has_value());
            auto facts = all;
            facts.*entry.fact = false;
            CHECK(firstUnsupportedPipelineState(state, facts) ==
                  std::optional{PipelineStateRequirement{entry.field, entry.feature}});
        }
    }
    // Dual-source blending writes as many attachments as the device allows.
    const auto two = parseRasterPassContract(
        Json{{"draw", Json::object()},
             {"raster_state", {{"color_attachments", Json::array({{{"blend", dual_blend}}, {{"blend", "opaque"}}})}}}},
        2, false).state;
    CHECK(firstUnsupportedPipelineState(two, all) ==
          std::optional{PipelineStateRequirement{"color_attachments", "maxFragmentDualSrcAttachments"}});
    // A material's refusal names it too.
    CHECK(pipelineStateUnsupportedMessage("forward", {"polygon_mode", "fillModeNonSolid"}, "grid") ==
          R"(pipeline.state_unsupported@1: {"pass":"forward","field":"polygon_mode","feature":"fillModeNonSolid","material":"grid"})");
}
