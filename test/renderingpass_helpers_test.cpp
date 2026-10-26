#include "registered_render_targets_test_support.hpp"
#include "compute_task_test_support.hpp"
#include "rendering_pass_test_support.hpp"
#include "../src/project/generationresources.hpp"
#include <catch2/generators/catch_generators.hpp>
#include "../src/core/renderingpass/computetask.hpp"
#include "../src/core/renderingpass/materialpassattachments.hpp"
#include "../src/core/renderingpass/renderingpassjsonhelpers.hpp"
#include "../src/core/renderingpass/renderingpasscontainer.hpp"
#include "../src/core/renderingpass/renderingpassruntimecompiler.hpp"
#include "../src/core/renderingpass/renderingpassvalidation.hpp"
#include "../src/core/renderingpass/rasterpassvulkanadapter.hpp"
#include "../src/core/renderingpass/rendertargetconfigregistration.hpp"
#include "../src/core/renderingpass/rendertargetmetadataresolver.hpp"
#include "../src/core/renderingpass/rendertargetnameresolver.hpp"
#include "../src/core/renderingpass/rendertargetjsonparser.hpp"
#include "../src/core/renderingpass/viewexecutionscheduler.hpp"
#include "../src/project/renderpipeline.hpp"
#include "../src/core/renderer/fullscreenpassrenderer.hpp"
#include "../src/core/userpublic/render/pass_implementation_abi_v1.hpp"
#include "render_graph_ir_test_support.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <algorithm>
#include <array>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <variant>
#include <vector>

namespace Pelican {

namespace {

using Json = nlohmann::json;

Json fixtureImage(const std::string &name, bool depth = false) {
    return Json{{"name", name}, {"extent_scale", 1.0},
        {"format", depth ? "D32_SFLOAT" : "R16G16B16A16_SFLOAT"},
        {"usage", Json::array({depth ? "DEPTH_STENCIL_ATTACHMENT" : "COLOR_ATTACHMENT", "SAMPLED"})}};
}

std::vector<PassDefinition> passesInGraph(Json graph, const Json &images,
    const Json &buffers, const RenderTargetNameResolver &names) {
    graph["name"] = "main";
    return TestSupport::renderingPassDefinitionsOf(Json{{"render_targets", images}, {"buffers", buffers},
        {"rendering_passes", Json::array({std::move(graph)})}}, names).at(0).passes;
}

PassDefinition passInGraph(const Json &pass, const Json &images,
    const Json &buffers, const RenderTargetNameResolver &names) {
    return passesInGraph(Json{{"passes", Json::array({pass})}}, images, buffers, names).at(0);
}

// Former leaf fixtures provide only target or material fields. Complete their
// surrounding declaration explicitly, then use the production parse/conversion.
PassDefinition targetPassOf(Json fields, const std::string &name, const Json &images,
    const RenderTargetNameResolver &names) {
    fields["name"] = name; fields["type"] = "raster";
    fields["draw"] = {{"vertex_count", 3}};
    fields["shader"] = {{"vertex", "engine://fixture"}, {"fragment", "engine://fixture"}};
    return passInGraph(fields, images, Json::array(), names);
}

PassDefinition namedMaterialOf(Json fields, const std::string &name, const Json &images,
    const Json &buffers, const RenderTargetNameResolver &names) {
    fields["name"] = name; fields["type"] = "material";
    fields["output"] = {{"color", "swapchain"}, {"depth", nullptr}};
    return passInGraph(fields, images, buffers, names);
}

// These tests used to provide only the fullscreen-specific fields. Supply
// the surrounding graph and explicit resources; all pass values still go
// through the same parse and conversion as production.
FullscreenPassInfo fullscreenInfoOf(nlohmann::json fields, const std::string &name) {
    using Json = nlohmann::json;
    fields["name"] = name;
    fields["type"] = "fullscreen";
    if (!fields.contains("output")) fields["output"] = {{"color", "swapchain"}, {"depth", nullptr}};
    Json images = Json::array();
    for (const auto *target : {"scene_color", "first_input", "second_input"}) {
        images.push_back(Json{{"name", target}, {"extent_scale", 1.0},
            {"format", "R8G8B8A8_UNORM"}, {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}});
    }
    images.push_back(Json{{"name", "scene_depth"}, {"extent_scale", 1.0}, {"history", true},
        {"format", "D32_SFLOAT"}, {"usage", Json::array({"DEPTH_STENCIL_ATTACHMENT", "SAMPLED"})}});
    images.push_back(Json{{"name", "environment_probe"}, {"extent_scale", 1.0}, {"width", 256}, {"height", 256},
        {"dimension", "cube"}, {"format", "R16G16B16A16_SFLOAT"}, {"usage", Json::array({"SAMPLED"})}});
    const Json config{{"render_targets", std::move(images)},
        {"buffers", Json::array({Json{{"name", "light_inventory"}, {"size", 16}}})},
        {"rendering_passes", Json::array({Json{{"name", "main"}, {"passes", Json::array({std::move(fields)})}}})}};
    return TestSupport::renderingPassDefinitionsOf(config).at(0).passes.at(0).fullscreenInfo();
}

// Complete the former material-info fixtures with an explicit graph and
// attachments. The fields under test are read by production's single parse.
PassDefinition materialPassOf(nlohmann::json fields, const std::string &name) {
    using Json = nlohmann::json;
    fields["name"] = name;
    fields["type"] = "material";
    if (!fields.contains("output")) fields["output"] = {{"color", "swapchain"}, {"depth", "material_depth"}};
    const Json config{{"render_targets", Json::array({Json{{"name", "material_depth"},
        {"extent_scale", 1.0}, {"format", "D32_SFLOAT"},
        {"usage", Json::array({"DEPTH_STENCIL_ATTACHMENT"})}}})},
        {"rendering_passes", Json::array({Json{{"name", "main"}, {"passes", Json::array({std::move(fields)})}}})}};
    return TestSupport::renderingPassDefinitionsOf(config).at(0).passes.at(0);
}

PassDefinition typedFixtureOf(const std::string &type, Json fields = Json::object()) {
    fields["name"] = "utility"; fields["type"] = type;
    fields["output"] = {{"color", type == "shadow_depth" ? Json(nullptr) : Json("swapchain")},
        {"depth", type == "shadow_depth" ? Json("depth") : Json(nullptr)}};
    if (type == "fullscreen" || type == "raster")
        fields["shader"] = {{"vertex", "engine://fixture"}, {"fragment", "engine://fixture"}};
    if (type == "raster") fields["draw"] = {{"vertex_count", 3}};
    Json config{{"render_targets", Json::array({fixtureImage("depth", true)})},
        {"rendering_passes", Json::array({Json{{"name", "main"}, {"passes", Json::array({fields})}}})}};
    return TestSupport::renderingPassDefinitionsOf(config).at(0).passes.at(0);
}

} // namespace


TEST_CASE("rendering pass helper ids name invalid and special targets", "[renderingpass]") {
    REQUIRE(invalidRenderingPassId().value == invalidRenderingPassIdValue);
    REQUIRE_FALSE(isValidRenderingPassId(invalidRenderingPassId()));
    REQUIRE(isValidRenderingPassId(RenderingPassId{0}));

    REQUIRE(noRenderTargetId().value == invalidRenderTargetIdValue);
    REQUIRE(swapchainRenderTargetId().value == swapchainRenderTargetIdValue);
    REQUIRE_FALSE(isConcreteRenderTarget(noRenderTargetId()));
    REQUIRE_FALSE(isConcreteRenderTarget(swapchainRenderTargetId()));
    REQUIRE(isSpecialRenderTarget(noRenderTargetId()));
    REQUIRE(isSpecialRenderTarget(swapchainRenderTargetId()));
    REQUIRE(isSwapchainRenderTarget(swapchainRenderTargetId()));
}

TEST_CASE("rendering pass JSON helpers parse known values", "[renderingpass]") {
    REQUIRE(parseAuthoredImageFormat("R8_UNORM") == ImageFormat::r8_unorm);
    REQUIRE(parseAuthoredImageFormat("D32_SFLOAT") == ImageFormat::d32_sfloat);
    REQUIRE(
        parseAuthoredImageFormat("R32_SFLOAT") ==
        ImageFormat::r32_sfloat);
    REQUIRE(
        std::string{imageFormatName(
            ImageFormat::r32_sfloat)} ==
        "R32_SFLOAT");

    const auto usage = parseAuthoredImageUsage(
        std::vector<std::string>{"COLOR_ATTACHMENT", "SAMPLED"});
    REQUIRE(static_cast<bool>(usage & ImageUsageBit::color_attachment));
    REQUIRE(static_cast<bool>(usage & ImageUsageBit::sampled));

    REQUIRE(std::holds_alternative<MaterialPassInfo>(typedFixtureOf("material").pass_info));
    REQUIRE(std::holds_alternative<FullscreenPassInfo>(typedFixtureOf("fullscreen").pass_info));
    REQUIRE(
        std::holds_alternative<
            GenericRasterPassInfo>(
            typedFixtureOf("raster").pass_info));
    REQUIRE(std::holds_alternative<ShadowDepthPassInfo>(typedFixtureOf("shadow_depth").pass_info));
    REQUIRE(std::holds_alternative<UiPassInfo>(typedFixtureOf("ui").pass_info));

    REQUIRE(typedFixtureOf("fullscreen", Json{{"push_constants", "projection_view"}}).fullscreenInfo().push_constants == FullscreenPushConstantData::eProjectionView);
    REQUIRE(parseAuthoredAttachmentLoadOp("dont_care") == AttachmentLoadOp::dont_care);
    REQUIRE(parseAuthoredAttachmentStoreOp("store") == AttachmentStoreOp::store);

    const nlohmann::json object{
        {"name", "main_pass"},
        {"scale", 1.5},
        {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
        {"count", 42},
    };
    REQUIRE(parseStringField(object, "name", "test") == "main_pass");
    REQUIRE(parseFloatField(object, "scale", "test") == 1.5f);
    REQUIRE(parseStringArrayField(object, "usage", "test").size() == 2);
    REQUIRE(parseUint32Field(object, "count", "test") == 42);
    CHECK(typedFixtureOf("ui").view_family == "$main");
    CHECK(typedFixtureOf("ui", Json{{"view_family", "$reflection/probe/0"}}).view_family == "$reflection/probe/0");
    TestSupport::requireNamedRefusal([&] { (void)typedFixtureOf("ui", Json{{"view_family", ""}}); },
        "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "utility"}, {"field", "view_family"}});
    CHECK(typedFixtureOf("ui", Json{{"regions", Json::array({"region.post", "region.debug"})}}).region_tags ==
        std::vector<std::string>{"region.post", "region.debug"});
    for (const auto &regions : {Json::array({"region.post", "region.post"}), Json::array({""})}) {
        TestSupport::requireNamedRefusal([&] { (void)typedFixtureOf("ui", Json{{"regions", regions}}); },
            "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "utility"}, {"field", "regions"},
                {"reason", regions.size() == 2 ? "duplicate" : "empty_or_too_long"}, {"region", regions.at(0)}});
    }
    const auto clear_color = typedFixtureOf("ui", Json{{"clear_color", Json::array({1.0, 0.5, 0.25, 1.0})}}).clear_color;
    REQUIRE(clear_color[0] == 1.0);
    REQUIRE(clear_color[1] == 0.5);
    REQUIRE(clear_color[2] == 0.25);
    REQUIRE(clear_color[3] == 1.0);

}

TEST_CASE("render target JSON parser returns target definitions", "[renderingpass]") {
    const nlohmann::json config{
        {"render_targets",
         nlohmann::json::array({{
             {"name", "half_color"},
             {"extent_scale", 0.5},
             {"format", "R8_UNORM"},
             {"format_candidates",
              nlohmann::json::array(
                  {"R8_UNORM",
                   "R16G16_SFLOAT"})},
             {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
         }})},
    };

    const auto definitions = Pelican::test_support::renderTargetDefinitionsOf(config);

    REQUIRE(definitions.size() == 1);
    REQUIRE(definitions[0].name == "half_color");
    REQUIRE(definitions[0].extent_scale == 0.5f);
    REQUIRE_FALSE(definitions[0].fixed_extent.has_value());
    REQUIRE(definitions[0].format == ImageFormat::r8_unorm);
    REQUIRE(
        definitions[0].format_candidates ==
        std::vector<ImageFormat>{
            ImageFormat::r8_unorm,
            ImageFormat::r16g16_sfloat});
    REQUIRE(static_cast<bool>(definitions[0].usage & ImageUsageBit::color_attachment));
    REQUIRE(static_cast<bool>(definitions[0].usage & ImageUsageBit::sampled));
}

TEST_CASE("render target JSON parser rejects malformed format candidates",
          "[renderingpass][physical-format]") {
    auto config = nlohmann::json{
        {"render_targets",
         nlohmann::json::array({{
             {"name", "color"},
             {"extent_scale", 1.0},
             {"format", "R8G8B8A8_UNORM"},
             {"usage",
              nlohmann::json::array(
                  {"COLOR_ATTACHMENT"})},
             {"format_candidates", "R16G16B16A16_SFLOAT"},
         }})},
    };
    REQUIRE_THROWS_WITH(
        Pelican::test_support::renderTargetDefinitionsOf(
            config),
        Catch::Matchers::ContainsSubstring(
            R"(render_graph.image_invalid@1 {"field":"format_candidates","reason":"requires_string_array")"));
}

TEST_CASE("render target JSON parser accepts fixed extents", "[renderingpass]") {
    const nlohmann::json config{
        {"render_targets",
         nlohmann::json::array({{
             {"name", "shadow_map"},
             {"extent_scale", 1.0},
             {"width", 2048},
             {"height", 2048},
             {"format", "D32_SFLOAT"},
             {"usage", nlohmann::json::array({"DEPTH_STENCIL_ATTACHMENT", "SAMPLED"})},
         }})},
    };

    const auto definitions = Pelican::test_support::renderTargetDefinitionsOf(config);

    REQUIRE(definitions.size() == 1);
    REQUIRE(definitions[0].fixed_extent.has_value());
    REQUIRE(definitions[0].fixed_extent->width == 2048);
    REQUIRE(definitions[0].fixed_extent->height == 2048);
}

TEST_CASE("render target JSON parser accepts history and its declarative clear",
          "[renderingpass][temporal]") {
    const auto definitions = Pelican::test_support::renderTargetDefinitionsOf(nlohmann::json{
        {"render_targets", nlohmann::json::array({{
            {"name", "temporal_accum"}, {"extent_scale", 1.0},
            {"format", "R16G16B16A16_SFLOAT"},
            {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
            {"history", true}, {"clear_color", {0.1f, 0.2f, 0.3f, 1.0f}}
        }})}
    });
    REQUIRE(definitions.size() == 1);
    REQUIRE(definitions.front().history);
    REQUIRE(definitions.front().history_clear_color[0] == Catch::Approx(0.1));
    REQUIRE(definitions.front().history_clear_color[3] == Catch::Approx(1.0));
}

TEST_CASE(
    "history clear is encoded after integer target format selection",
    "[renderingpass][temporal][typed-clear][wp218]") {
    const auto definitions =
        Pelican::test_support::renderTargetDefinitionsOf(
            nlohmann::json{
                {"render_targets",
                 nlohmann::json::array(
                     {{{"name", "history_id"},
                       {"extent_scale", 1.0},
                       {"format", "R32_UINT"},
                       {"usage",
                        nlohmann::json::array(
                            {"COLOR_ATTACHMENT",
                             "SAMPLED"})},
                       {"history", true},
                       {"clear_color",
                        {4294967295.0, 3, 0, 0}}}})}});
    REQUIRE(definitions.size() == 1);
    const auto physical =
        physicalRenderTargetHistoryClearColor(
            definitions.front());
    REQUIRE(
        physical.uint32[0] ==
        std::numeric_limits<
            std::uint32_t>::max());
    REQUIRE(physical.uint32[1] == 3u);

    auto invalid = definitions.front();
    invalid.history_clear_color[0] = 0.5;
    REQUIRE_THROWS_WITH(
        physicalRenderTargetHistoryClearColor(
            invalid),
        Catch::Matchers::ContainsSubstring(
            "fractional or out of range"));
}

TEST_CASE("resolver v2 selects B8 SRGB scene/display and float HDR", "[renderingpass][color-c1b]") {
    const auto config = nlohmann::json{
        {"resolver_version", 2},
        {"render_targets",
         nlohmann::json::array({
             {{"name", "lit_color"},
              {"extent_scale", 1.0},
              {"format", "B8G8R8A8_UNORM"},
              {"format_class", "scene"},
              {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})}},
             {{"name", "display"},
              {"extent_scale", 1.0},
              {"format", "B8G8R8A8_SRGB"},
              {"format_class", "display"},
              {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"})}},
         })},
    };

    const auto resolved = resolveRenderTargetFormatClassesV2(
        config, vk::Format::eR8G8B8A8Srgb, vk::Extent2D{64, 32}, false);
    REQUIRE(resolved.at("render_targets").at(0).at("format").get<std::string>() ==
            "B8G8R8A8_SRGB");
    REQUIRE(resolved.at("render_targets").at(1).at("format").get<std::string>() ==
            "B8G8R8A8_SRGB");
    REQUIRE(resolved.at("render_targets").at(1).at("width").get<uint32_t>() == 64);
    REQUIRE(resolved.at("render_targets").at(1).at("height").get<uint32_t>() == 32);
    REQUIRE(Pelican::test_support::renderTargetDefinitionsOf(resolved).at(0).format ==
            ImageFormat::b8g8r8a8_srgb);
    const auto hdr = resolveRenderTargetFormatClassesV2(
        config, vk::Format::eR8G8B8A8Srgb, vk::Extent2D{64, 32}, true);
    REQUIRE(hdr.at("render_targets").at(0).at("format") == "R16G16B16A16_SFLOAT");
}

TEST_CASE("fullscreen pass JSON parser reads explicit fullscreen options", "[renderingpass]") {
    const nlohmann::json pass_json{
        {"push_constants", "camera_position"},
        {"uses_light_data", true},
        {"shader", {{"vertex", "fullscreen"}, {"fragment", "lighting"}}},
    };

    const auto info = fullscreenInfoOf(pass_json, "lighting_pass");

    REQUIRE(info.push_constants == FullscreenPushConstantData::eCameraPosition);
    REQUIRE(info.uses_light_data);
    REQUIRE(info.vert_shader.ref == "fullscreen");
    REQUIRE(info.vert_shader.kind == ShaderReferenceKind::stem);
    REQUIRE(info.frag_shader.ref == "lighting");
    REQUIRE(info.frag_shader.kind == ShaderReferenceKind::stem);
}

TEST_CASE(
    "fullscreen fixed state stays optional and rejects every depth spelling",
    "[renderingpass][fullscreen][raster-state][wp353]") {
    const nlohmann::json omitted{
        {"shader", {{"vertex", "fullscreen"}, {"fragment", "lighting"}}},
    };
    const auto legacy =
        fullscreenInfoOf(omitted, "legacy");
    REQUIRE_FALSE(legacy.raster_state.has_value());

    auto authored = omitted;
    authored["raster_state"] = {
        {"topology", "triangle_strip"},
        {"cull", "back"},
        {"front_face", "clockwise"},
        {"color_attachments",
         nlohmann::json::array(
             {{{"blend", "additive"},
               {"write_mask", "ga"}}})},
    };
    const auto parsed =
        fullscreenInfoOf(authored, "authored");
    REQUIRE(parsed.raster_state.has_value());
    CHECK(
        parsed.raster_state->topology ==
        RasterPrimitiveTopology::triangle_strip);
    CHECK(
        parsed.raster_state->cull ==
        RasterCullMode::back);
    CHECK(
        parsed.raster_state->front_face ==
        RasterFrontFace::clockwise);
    REQUIRE(
        parsed.raster_state->color_attachments.size() == 1);
    CHECK(
        parsed.raster_state->color_attachments.front().write_mask ==
        (materialOutputWriteGreen |
         materialOutputWriteAlpha));

    for (const auto *field : {
             "depth_test", "depth_write", "depth_compare",
             "depth_bias"}) {
        DYNAMIC_SECTION(field) {
            auto invalid = omitted;
            invalid["raster_state"] = nlohmann::json::object();
            invalid["raster_state"][field] =
                std::string_view{field} == "depth_compare"
                    ? nlohmann::json{"less"}
                    : nlohmann::json{true};
            TestSupport::requireNamedRefusal([&] { (void)fullscreenInfoOf(
                    invalid, "depth_is_out_of_scope"); },
            "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "depth_is_out_of_scope"}, {"field", "raster_state"}, {"reason", "invalid"}});
        }
    }
}

TEST_CASE(
    "WP357a fullscreen command recording narrows only a blended engine triangle list",
    "[renderingpass][fullscreen][raster-state][wp357a]") {
    const auto blended = nlohmann::json::parse(R"json({
      "shader": {
        "vertex": "engine://fullscreen",
        "fragment": "engine://bloom_upsample"
      },
      "raster_state": {
        "topology": "triangle_list",
        "color_attachments": [{"blend": "additive"}]
      }
    })json");
    const auto triangle =
        fullscreenInfoOf(
            blended, "wp357a_triangle_blend");
    CHECK((fullscreenDrawCommand(triangle) ==
           FullscreenDrawCommand{3, 1, 0, 0}));

    auto line = triangle;
    line.raster_state->topology =
        RasterPrimitiveTopology::line_list;
    CHECK((fullscreenDrawCommand(line) ==
           FullscreenDrawCommand{6, 1, 0, 0}));

    auto opaque = triangle;
    opaque.raster_state->color_attachments.front()
        .blend.enabled = false;
    CHECK((fullscreenDrawCommand(opaque) ==
           FullscreenDrawCommand{6, 1, 0, 0}));

    auto authored_line = blended;
    authored_line["raster_state"]["topology"] =
        "line_list";
    TestSupport::requireNamedRefusal([&] { (void)fullscreenInfoOf(
            authored_line, "wp357a_line_blend"); },
            "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "wp357a_line_blend"}, {"field", "raster_state.topology"}, {"reason", "fullscreen_requires_triangle_list"}});
}

TEST_CASE(
    "render target JSON parser keeps mip and array-layer contracts",
    "[renderingpass][subresource][wp209b]") {
    const auto definitions =
        Pelican::test_support::renderTargetDefinitionsOf(
            nlohmann::json::parse(R"json({
              "render_targets": [
                {
                  "name": "fixed_pyramid",
                  "extent_scale": 1.0,
                  "format": "R16G16_SFLOAT",
                  "usage": ["SAMPLED", "STORAGE"],
                  "mip_levels": 6,
                  "layers": 3
                },
                {
                  "name": "full_pyramid",
                  "extent_scale": 1.0,
                  "format": "R16G16_SFLOAT",
                  "usage": ["SAMPLED", "STORAGE"],
                  "mip_levels": "full"
                }
              ]
            })json"));
    REQUIRE(definitions.size() == 2);
    const ImageMipLevelCount fixed{
        ImageMipLevelMode::fixed, 6};
    const ImageMipLevelCount full{
        ImageMipLevelMode::full_chain, 1};
    REQUIRE(
        definitions[0].mip_levels ==
        fixed);
    REQUIRE(
        definitions[1].mip_levels ==
        full);
    REQUIRE(definitions[0].array_layers == 3);
    REQUIRE(definitions[1].array_layers == 1);
    REQUIRE(resolveImageMipLevels(
                definitions[1].mip_levels,
                128, 64) == 8);

    auto invalid = nlohmann::json::parse(R"json({
      "render_targets": [{
        "name": "bad",
        "extent_scale": 1.0,
        "format": "R16G16_SFLOAT",
        "usage": ["SAMPLED"],
        "mip_levels": 0
      }]
    })json");
    REQUIRE_THROWS_WITH(
        Pelican::test_support::renderTargetDefinitionsOf(
            invalid),
        Catch::Matchers::ContainsSubstring(
            R"(render_graph.image_invalid@1 {"field":"mip_levels","reason":"not_positive")"));
    invalid["render_targets"][0]["mip_levels"] =
        "automatic";
    REQUIRE_THROWS_WITH(
        Pelican::test_support::renderTargetDefinitionsOf(
            invalid),
        Catch::Matchers::ContainsSubstring(
            R"("field":"mip_levels","reason":"not_full")"));
    invalid["render_targets"][0]["mip_levels"] = 1;
    invalid["render_targets"][0]["layers"] = 0;
    REQUIRE_THROWS_WITH(
        Pelican::test_support::renderTargetDefinitionsOf(
            invalid),
        Catch::Matchers::ContainsSubstring(
            R"("field":"layers","reason":"not_positive")"));
}

TEST_CASE(
    "render target JSON parser keeps cube resource shape separate from "
    "array layers",
    "[renderingpass][cube][wp236]") {
    auto document = nlohmann::json::parse(R"json({
      "render_targets": [{
        "name": "environment_probe",
        "extent_scale": 1.0,
        "width": 256,
        "height": 256,
        "dimension": "cube",
        "format": "R16G16B16A16_SFLOAT",
        "usage": ["COLOR_ATTACHMENT", "SAMPLED"],
        "mip_levels": "full"
      }]
    })json");
    const auto definitions =
        Pelican::test_support::renderTargetDefinitionsOf(
            document);
    REQUIRE(definitions.size() == 1);
    REQUIRE(
        definitions.front().dimension ==
        ImageResourceDimension::cube);
    REQUIRE(definitions.front().array_layers == 6);
    REQUIRE(
        definitions.front().fixed_extent ==
        ResolvedResourceExtent{256, 256});

    auto invalid = document;
    invalid["render_targets"][0].erase("width");
    invalid["render_targets"][0].erase("height");
    REQUIRE_THROWS_WITH(
        Pelican::test_support::renderTargetDefinitionsOf(
            invalid),
        Catch::Matchers::ContainsSubstring(
            R"("field":"dimension","reason":"cube_requires_fixed_extent")"));

    invalid = document;
    invalid["render_targets"][0]["height"] = 128;
    REQUIRE_THROWS_WITH(
        Pelican::test_support::renderTargetDefinitionsOf(
            invalid),
        Catch::Matchers::ContainsSubstring(
            R"("field":"dimension","reason":"cube_requires_square")"));

    invalid = document;
    invalid["render_targets"][0]["layers"] = 5;
    REQUIRE_THROWS_WITH(
        Pelican::test_support::renderTargetDefinitionsOf(
            invalid),
        Catch::Matchers::ContainsSubstring(
            R"("field":"layers","reason":"cube_requires_six")"));

    invalid = document;
    invalid["render_targets"][0]["dimension"] =
        "3d";
    REQUIRE_THROWS_WITH(
        Pelican::test_support::renderTargetDefinitionsOf(
            invalid),
        Catch::Matchers::ContainsSubstring(
            R"("field":"dimension","reason":"not_2d_or_cube")"));
}

TEST_CASE(
    "fullscreen pass JSON parser keeps per-input sampling typed",
    "[renderingpass][upscale][sampling]") {
    const nlohmann::json pass_json{
        {"input", nlohmann::json::array({"first_input", "second_input"})},
        {"input_sampling",
         nlohmann::json::array(
             {{{"filter", "nearest"},
               {"address", "clamp_to_edge"}},
              {{"filter", "linear"},
               {"address", "mirrored_repeat"}}})},
        {"shader",
         {{"vertex", "fullscreen"},
          {"fragment", "upscale"}}},
    };

    const auto info =
        fullscreenInfoOf(
            pass_json, "upscale");

    REQUIRE(info.input_sampling ==
            std::vector<ShaderResourcePortSampling>{
                {ShaderResourcePortFilter::nearest,
                 ShaderResourcePortAddressMode::clamp_to_edge},
                {ShaderResourcePortFilter::linear,
                 ShaderResourcePortAddressMode::mirrored_repeat},
            });
    auto invalid = pass_json;
    invalid["input_sampling"][0]["address"] =
        "wrap_somehow";
    TestSupport::requireNamedRefusal([&] { (void)fullscreenInfoOf(
            invalid, "upscale"); },
            "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "upscale"}, {"field", "input_sampling"}, {"reason", "invalid"}});
}

TEST_CASE(
    "fullscreen pass JSON parser keeps named resource ports independent "
    "from graph edges",
    "[renderingpass][resource-port][wp207a]") {
    const auto pass_json =
        nlohmann::json::parse(R"json({
          "input": ["scene_color", "scene_depth@history"],
          "resource_ports": {
            "color": {
              "resource": "scene_color",
              "access": "sampled",
              "sampling": {
                "filter": "nearest",
                "address": "clamp_to_edge"
              }
            },
            "previous_depth": {
              "resource": "scene_depth@history",
              "view": "per_view"
            }
          },
          "shader": {
            "vertex": "fullscreen",
            "fragment": "resolve"
          }
        })json");

    const auto info =
        fullscreenInfoOf(
            pass_json, "resolve");
    REQUIRE(info.resource_ports.size() == 2);
    REQUIRE(
        info.resource_ports.at(0).name == "color");
    REQUIRE(
        info.resource_ports.at(0).resource ==
        "scene_color");
    REQUIRE(
        info.resource_ports.at(0).sampling.filter ==
        ShaderResourcePortFilter::nearest);
    REQUIRE(
        info.resource_ports.at(0)
            .sampling.address_mode ==
        ShaderResourcePortAddressMode::
            clamp_to_edge);
    REQUIRE(
        info.resource_ports.at(1).view ==
        ShaderResourcePortView::per_view);

    auto conflicting = pass_json;
    conflicting["input_sampling"] =
        nlohmann::json::array(
            {{{"filter", "linear"}}});
    TestSupport::requireNamedRefusal([&] { (void)fullscreenInfoOf(
            conflicting, "resolve"); },
            "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "resolve"}, {"field", "input_sampling"}, {"reason", "replaced_by_resource_ports"}});

    auto unknown = pass_json;
    unknown["resource_ports"]["color"]["resource"] =
        "not_an_input";
    TestSupport::requireNamedRefusal([&] { (void)fullscreenInfoOf(
            unknown, "resolve"); },
            "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "resolve"}, {"field", "resource_ports"}, {"reason", "invalid"}});

    auto duplicate = pass_json;
    duplicate["resource_ports"]["color"]["subresource"] =
        {{"mip", 0}};
    duplicate["resource_ports"]["previous_depth"]
             ["resource"] = "scene_color";
    duplicate["resource_ports"]["previous_depth"]
             ["access"] = "sampled";
    duplicate["resource_ports"]["previous_depth"]
             ["subresource"] = {{"mip", 1}};
    TestSupport::requireNamedRefusal([&] { (void)fullscreenInfoOf(
            duplicate, "resolve"); },
            "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "resolve"}, {"field", "resource_ports"}, {"reason", "duplicate_resource"}});
}

TEST_CASE(
    "fullscreen pass JSON parser accepts explicit read-only buffer ports",
    "[renderingpass][resource-port][clustered][wp208]") {
    const auto pass_json =
        nlohmann::json::parse(R"json({
          "input": ["scene_color", "light_inventory"],
          "resource_ports": {
            "color": {
              "resource": "scene_color",
              "access": "sampled"
            },
            "lights": {
              "resource": "light_inventory",
              "kind": "buffer",
              "element": "uvec4",
              "access": "storage"
            }
          },
          "shader": {
            "vertex": "fullscreen",
            "fragment": "lighting"
          }
        })json");

    const auto info =
        fullscreenInfoOf(
            pass_json, "lighting");
    REQUIRE(info.resource_ports.size() == 2);
    REQUIRE(
        info.resource_ports.at(1).kind ==
        ShaderResourcePortKind::buffer);
    REQUIRE(
        info.resource_ports.at(1).buffer_element ==
        ShaderResourceBufferElement::uvec4);

    auto invalid = pass_json;
    invalid["resource_ports"]["lights"]["access"] =
        "sampled";
    TestSupport::requireNamedRefusal([&] { (void)fullscreenInfoOf(
            invalid, "lighting"); },
            "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "lighting"}, {"field", "resource_ports"}, {"reason", "invalid"}});
}

TEST_CASE(
    "generic raster pass parser retains open implementation ids and typed resources",
    "[renderingpass][raster][wp238b]") {
    const auto pass_json =
        nlohmann::json::parse(R"json({
          "input": ["scene_color", "draw_metadata"],
          "resource_ports": {
            "scene": {
              "resource": "scene_color",
              "access": "sampled",
              "sampling": {
                "filter": "nearest",
                "address": "clamp_to_edge"
              }
            },
            "metadata": {
              "resource": "draw_metadata",
              "kind": "buffer",
              "element": "uvec4",
              "access": "storage"
            }
          },
          "draw": {
            "implementation": "project.procedural_sky@2",
            "operation": "direct",
            "vertex_count": 18,
            "instance_count": 4
          },
          "raster_state": {
            "topology": "triangle_strip",
            "cull": "front",
            "depth_test": true,
            "color_attachments": [
              {"blend": "opaque", "write_mask": "rgba"},
              {"blend": "additive", "write_mask": "rg"}
            ]
          },
          "shader": {
            "implementation": "project.sky_shader@4",
            "vertex": "shaders/sky_vertex",
            "fragment": "shaders/sky_fragment"
          }
        })json");

    const Json images = Json::array({fixtureImage("scene_color"), fixtureImage("out_a"), fixtureImage("out_b"), fixtureImage("depth", true)});
    const Json buffers = Json::array({Json{{"name", "draw_metadata"}, {"size", 64}}});
    const auto raster = [&](Json fields) {
        fields["name"] = "sky"; fields["type"] = "raster";
        fields["output"] = {{"color", Json::array({"out_a", "out_b"})}, {"depth", "depth"}};
        Json config{{"render_targets", images}, {"buffers", buffers},
            {"rendering_passes", Json::array({Json{{"name", "main"}, {"passes", Json::array({fields})}}})}};
        return TestSupport::renderingPassDefinitionsOf(config).at(0).passes.at(0).genericRasterInfo();
    };
    const auto info =
        raster(pass_json);
    CHECK(
        info.contract.geometry.implementation ==
        "project.procedural_sky@2");
    CHECK(
        info.shader_implementation ==
        "project.sky_shader@4");
    CHECK(
        info.vert_shader.ref ==
        "shaders/sky_vertex");
    REQUIRE(info.frag_shader);
    CHECK(
        info.frag_shader->ref ==
        "shaders/sky_fragment");
    REQUIRE(info.resource_ports.size() == 2);
    const auto metadata_port =
        std::find_if(
            info.resource_ports.begin(),
            info.resource_ports.end(),
            [](const auto &port) {
                return port.name == "metadata";
            });
    REQUIRE(
        metadata_port !=
        info.resource_ports.end());
    CHECK(
        metadata_port->kind ==
        ShaderResourcePortKind::buffer);
    CHECK(
        std::get<RasterDirectDrawOperation>(
            info.contract.geometry.operation)
            .instance_count == 4);

    auto missing_fragment = pass_json;
    missing_fragment["shader"].erase("fragment");
    TestSupport::requireNamedRefusal([&] { (void)raster(missing_fragment); },
        "render_graph.pass_execution_invalid@1", {{"graph", "main"}, {"node", "sky"}, {"field", "shader.fragment"}, {"reason", "required"}});

    auto unversioned = pass_json;
    unversioned["shader"]["implementation"] =
        "project.sky_shader";
    TestSupport::requireNamedRefusal([&] { (void)raster(unversioned); },
        "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "sky"}, {"field", "shader.implementation"}, {"reason", "not_a_canonical_id"}});
}

TEST_CASE(
    "generic raster Vulkan adapter maps logical MRT state into physical scope slots",
    "[renderingpass][raster][vulkan][wp238b]") {
    auto contract = parseRasterPassContract(
        nlohmann::json::parse(R"json({
          "draw": {"vertex_count": 3},
          "raster_state": {
            "topology": "line_strip",
            "cull": "back",
            "front_face": "clockwise",
            "depth_test": true,
            "depth_write": true,
            "depth_compare": "greater",
            "color_attachments": [
              {"blend": "opaque", "write_mask": "r"},
              {"blend": "additive", "write_mask": "ga"}
            ]
          }
        })json"),
        2, true);
    GraphicsPipelineDesc desc;
    desc.color_formats = {
        vk::Format::eR16G16B16A16Sfloat,
        vk::Format::eR8G8B8A8Unorm,
        vk::Format::eR16G16Sfloat};
    desc.depth_format =
        vk::Format::eD32Sfloat;
    const std::array locations{
        1u,
        unusedGraphicsAttachmentMapping,
        0u};
    applyVulkanRasterPassContract(
        contract, desc, locations);

    CHECK(
        desc.topology ==
        vk::PrimitiveTopology::eLineStrip);
    CHECK(
        desc.cull_mode ==
        vk::CullModeFlagBits::eBack);
    CHECK(
        desc.front_face ==
        vk::FrontFace::eClockwise);
    CHECK(desc.depth_test);
    CHECK(desc.depth_write);
    CHECK(
        desc.depth_compare ==
        vk::CompareOp::eGreater);
    REQUIRE(
        desc.color_attachment_states.size() == 3);
    CHECK(
        desc.color_attachment_states[0]
            .blend_enabled);
    CHECK(
        desc.color_attachment_states[0]
            .write_mask ==
        (vk::ColorComponentFlagBits::eG |
         vk::ColorComponentFlagBits::eA));
    CHECK(
        desc.color_attachment_states[1]
            .write_mask ==
        vk::ColorComponentFlags{});
    CHECK_FALSE(
        desc.color_attachment_states[2]
            .blend_enabled);
    CHECK(
        desc.color_attachment_states[2]
            .write_mask ==
        vk::ColorComponentFlagBits::eR);

    const std::array duplicate{0u, 0u, 1u};
    CHECK_THROWS_WITH(
        applyVulkanRasterPassContract(
            contract, desc, duplicate),
        Catch::Matchers::ContainsSubstring(
            "duplicates a logical color output"));
}

TEST_CASE(
    "fullscreen Vulkan adapter maps authored state into a non-identity physical slot",
    "[renderingpass][fullscreen][raster-state][vulkan][wp353]") {
    const auto state = parseRasterFixedFunctionState(
        nlohmann::json::parse(R"json({
          "raster_state": {
            "topology": "triangle_strip",
            "cull": "front",
            "front_face": "clockwise",
            "color_attachments": [{
              "blend": {
                "color": {"src": "one", "dst": "one", "op": "add"},
                "alpha": {"src": "one", "dst": "one", "op": "add"}
              },
              "write_mask": "rb"
            }]
          }
        })json"),
        1, "fullscreen physical mapping");
    GraphicsPipelineDesc desc;
    desc.color_formats = {
        vk::Format::eR16G16B16A16Sfloat,
        vk::Format::eR8G8B8A8Unorm};
    const std::array locations{
        unusedGraphicsAttachmentMapping, 0u};

    applyVulkanRasterPassContract(
        state, desc, locations);

    CHECK(
        desc.topology ==
        vk::PrimitiveTopology::eTriangleStrip);
    CHECK(
        desc.cull_mode ==
        vk::CullModeFlagBits::eFront);
    CHECK(
        desc.front_face ==
        vk::FrontFace::eClockwise);
    REQUIRE(desc.color_attachment_states.size() == 2);
    CHECK(
        desc.color_attachment_states[0].write_mask ==
        vk::ColorComponentFlags{});
    CHECK_FALSE(
        desc.color_attachment_states[0].blend_enabled);
    CHECK(desc.color_attachment_states[1].blend_enabled);
    CHECK(
        desc.color_attachment_states[1].source_color ==
        vk::BlendFactor::eOne);
    CHECK(
        desc.color_attachment_states[1].destination_color ==
        vk::BlendFactor::eOne);
    CHECK(
        desc.color_attachment_states[1].write_mask ==
        (vk::ColorComponentFlagBits::eR |
         vk::ColorComponentFlagBits::eB));

    struct ViewExpectation {
        const char *name;
        GraphicsPipelineViewContract view;
    };
    const std::array<ViewExpectation, 3> views{
        ViewExpectation{
            "flat", GraphicsPipelineViewContract{}},
        // Sequential XR registers one single-view pipeline and invokes it
        // once per eye; multiview registers one two-view pipeline.
        ViewExpectation{
            "xr_sequential", GraphicsPipelineViewContract{}},
        ViewExpectation{
            "xr_multiview",
            GraphicsPipelineViewContract::multiview(2)},
    };
    for (const auto &view : views) {
        DYNAMIC_SECTION(view.name) {
            GraphicsPipelineDesc variant;
            variant.color_formats = {
                vk::Format::eR16G16B16A16Sfloat,
                vk::Format::eR8G8B8A8Unorm};
            variant.view = view.view;
            applyVulkanRasterPassContract(
                state, variant, locations);
            REQUIRE(
                variant.color_attachment_states ==
                desc.color_attachment_states);
            REQUIRE(
                variant.topology == desc.topology);
            REQUIRE(
                variant.cull_mode == desc.cull_mode);
            REQUIRE(
                variant.front_face == desc.front_face);
            REQUIRE(variant.view == view.view);
        }
    }
}

// PSN-1: the one Vulkan mapping fills every fixed-function field the factory
// used to hard-code, and refuses what the desc's attachments cannot carry.
TEST_CASE(
    "PSN-1 the Vulkan mapping fills every fixed-function desc field from the pipeline state",
    "[renderingpass][raster-state][vulkan][psn1]") {
    // Nothing authored: the desc is the factory's old fixed values.
    {
        GraphicsPipelineDesc desc;
        desc.color_formats = {vk::Format::eR8G8B8A8Unorm};
        desc.depth_format = vk::Format::eD32Sfloat;
        const auto plain = parseRasterFixedFunctionState(nlohmann::json::object(), 1, "plain");
        applyVulkanRasterPassContract(plain, desc);
        GraphicsPipelineDesc expected;
        expected.color_formats = desc.color_formats;
        expected.depth_format = desc.depth_format;
        expected.color_attachment_states = desc.color_attachment_states;
        CHECK(desc == expected);
        CHECK(desc.polygon_mode == vk::PolygonMode::eFill);
        CHECK(desc.line_width == 1.0f);
        CHECK_FALSE(desc.depth_bias.has_value());
        CHECK_FALSE(desc.stencil.has_value());
        CHECK_FALSE(desc.logic_op.has_value());
        CHECK_FALSE(desc.min_sample_shading.has_value());
        CHECK(desc.sample_mask == 0xffffffffu);
        CHECK_FALSE(desc.primitive_restart);
    }

    const auto contract = parseRasterPassContract(
        nlohmann::json::parse(R"json({
          "draw": {"vertex_count": 4},
          "raster_state": {
            "topology": "triangle_strip", "primitive_restart": true,
            "polygon_mode": "line", "depth_clamp": true,
            "depth_bias": {"constant": -8, "slope": 2, "clamp": 0.5},
            "line_width": 3, "rasterizer_discard": true, "conservative": "underestimate",
            "sample_shading": 0.25, "sample_mask": 5, "alpha_to_coverage": true, "alpha_to_one": true,
            "depth_test": true, "depth_compare": "greater", "depth_bounds": [0.125, 0.875],
            "stencil": {
              "front": {"fail": "zero", "pass": "replace", "depth_fail": "decrement_and_clamp",
                        "compare": "not_equal", "compare_mask": 7, "write_mask": 3, "reference": 9},
              "back": {"pass": "increment_and_clamp", "compare": "less"}
            },
            "blend_constants": [0.5, 0.25, 0.125, 1],
            "color_attachments": [{"blend": {
              "color": {"src": "constant_color", "dst": "one_minus_src1_alpha", "op": "add"},
              "alpha": {"src": "src1_color", "dst": "one_minus_constant_alpha", "op": "add"}}}]
          }
        })json"),
        1, true);
    // logic_op replaces blending: the same state with a logic op instead.
    auto logic = contract;
    logic.state.color_attachments.front().blend.enabled = false;
    logic.state.logic_op = RasterLogicOperation::nand;
    GraphicsPipelineDesc desc;
    desc.color_formats = {vk::Format::eR8G8B8A8Unorm};
    desc.depth_format = vk::Format::eD24UnormS8Uint;
    applyVulkanRasterPassContract(logic, desc);
    CHECK(desc.topology == vk::PrimitiveTopology::eTriangleStrip);
    CHECK(desc.primitive_restart);
    CHECK(desc.polygon_mode == vk::PolygonMode::eLine);
    CHECK(desc.depth_clamp);
    CHECK(desc.depth_bias ==
          std::optional{GraphicsPipelineDepthBias{.constant_factor = -8.0f, .clamp = 0.5f, .slope_factor = 2.0f}});
    CHECK(desc.line_width == 3.0f);
    CHECK(desc.rasterizer_discard);
    CHECK(desc.conservative_rasterization == vk::ConservativeRasterizationModeEXT::eUnderestimate);
    CHECK(desc.min_sample_shading == std::optional{0.25f});
    CHECK(desc.sample_mask == 5u);
    CHECK(desc.alpha_to_coverage);
    CHECK(desc.alpha_to_one);
    CHECK(desc.depth_test);
    CHECK_FALSE(desc.depth_write);
    CHECK(desc.depth_compare == vk::CompareOp::eGreater);
    CHECK(desc.depth_bounds == std::optional{std::array{0.125f, 0.875f}});
    REQUIRE(desc.stencil.has_value());
    CHECK(desc.stencil->front == GraphicsPipelineStencilFaceState{
        .fail = vk::StencilOp::eZero, .pass = vk::StencilOp::eReplace,
        .depth_fail = vk::StencilOp::eDecrementAndClamp, .compare = vk::CompareOp::eNotEqual,
        .compare_mask = 7, .write_mask = 3, .reference = 9});
    CHECK(desc.stencil->back == GraphicsPipelineStencilFaceState{
        .pass = vk::StencilOp::eIncrementAndClamp, .compare = vk::CompareOp::eLess});
    CHECK(desc.logic_op == std::optional{vk::LogicOp::eNand});
    CHECK(desc.blend_constants == std::array{0.5f, 0.25f, 0.125f, 1.0f});

    // The new blend factors lower through the same table.
    const auto &blended = contract;
    GraphicsPipelineDesc blend_desc;
    blend_desc.color_formats = {vk::Format::eR8G8B8A8Unorm};
    blend_desc.depth_format = vk::Format::eD24UnormS8Uint;
    applyVulkanRasterPassContract(blended, blend_desc);
    REQUIRE(blend_desc.color_attachment_states.size() == 1);
    const auto &attachment = blend_desc.color_attachment_states.front();
    CHECK(attachment.source_color == vk::BlendFactor::eConstantColor);
    CHECK(attachment.destination_color == vk::BlendFactor::eOneMinusSrc1Alpha);
    CHECK(attachment.source_alpha == vk::BlendFactor::eSrc1Color);
    CHECK(attachment.destination_alpha == vk::BlendFactor::eOneMinusConstantAlpha);
    // A state change is a desc change: no cache keyed by desc reuses the old.
    CHECK(blend_desc != desc);

    // Stencil on a depth format without a stencil component is refused with
    // the pass and the format named; without a depth attachment, earlier.
    GraphicsPipelineDesc depth_only;
    depth_only.color_formats = {vk::Format::eR8G8B8A8Unorm};
    depth_only.depth_format = vk::Format::eD32Sfloat;
    CHECK_THROWS_WITH(
        applyVulkanRasterPassContract(logic, depth_only, {}, "Raster pass 'mask'"),
        "Raster pass 'mask' enables stencil, but its depth attachment format 'D32_SFLOAT' has no "
        "stencil component (use D24_UNORM_S8_UINT)");
    PipelineState stencil_only;
    stencil_only.stencil = RasterStencilState{};
    GraphicsPipelineDesc no_depth;
    CHECK_THROWS_WITH(
        applyVulkanPipelineState(stencil_only, no_depth, "shadow"),
        Catch::Matchers::ContainsSubstring("shadow enables stencil without a depth attachment"));

    // The device facts come from the runtime capability set.
    VulkanRuntimeCapabilities capabilities;
    capabilities.fill_mode_non_solid = true;
    capabilities.max_fragment_dual_src_attachments = 1;
    capabilities.conservative_point_and_line_rasterization = true;
    const auto facts = pipelineStateDeviceFacts(capabilities);
    CHECK(facts.fill_mode_non_solid);
    CHECK_FALSE(facts.wide_lines);
    CHECK(facts.max_fragment_dual_src_attachments == 1u);
    CHECK(facts.conservative_point_and_line_rasterization);
}

TEST_CASE(
    "shader resource port JSON accepts sampled cube views",
    "[renderingpass][resource-port][cube][wp236]") {
    auto pass_json = nlohmann::json::parse(R"json({
      "input": ["environment_probe"],
      "resource_ports": {
        "environment": {
          "resource": "environment_probe",
          "kind": "image",
          "access": "sampled",
          "view": "cube"
        }
      },
      "shader": {
        "vertex": "fullscreen",
        "fragment": "environment"
      }
    })json");
    const auto info =
        fullscreenInfoOf(
            pass_json, "environment");
    REQUIRE(info.resource_ports.size() == 1);
    REQUIRE(
        info.resource_ports.front().view ==
        ShaderResourcePortView::cube);

    pass_json["resource_ports"]["environment"]
             ["access"] = "storage";
    TestSupport::requireNamedRefusal([&] { (void)fullscreenInfoOf(
            pass_json, "environment"); },
            "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "environment"}, {"field", "resource_ports"}, {"reason", "invalid"}});
}

TEST_CASE(
    "shader resource ports type disjoint mip reads and writes",
    "[renderingpass][resource-port][subresource][wp209b]") {
    const auto config =
        nlohmann::json::parse(R"json({
          "compute_tasks": [{
            "name": "reduce_depth",
            "shader": "shaders/reduce_depth",
            "reads": ["depth_pyramid"],
            "writes": ["depth_pyramid"],
            "resource_ports": {
              "source_depth": {
                "resource": "depth_pyramid",
                "access": "sampled",
                "subresource": {
                  "mip": 2,
                  "layer": 1
                }
              },
              "reduced_depth": {
                "resource": "depth_pyramid",
                "access": "storage",
                "subresource": {
                  "mip": 3,
                  "layer": 1
                }
              }
            }
          }]
        })json");
    const auto tasks =
        Pelican::TestSupport::computeTaskDefinitionsOf(
            config);
    REQUIRE(tasks.size() == 1);
    REQUIRE(
        tasks.front().resource_ports.size() ==
        2);
    const auto source = std::find_if(
        tasks.front().resource_ports.begin(),
        tasks.front().resource_ports.end(),
        [](const auto &port) {
            return port.name == "source_depth";
        });
    REQUIRE(
        source !=
        tasks.front().resource_ports.end());
    REQUIRE(source->subresource.has_value());
    REQUIRE(
        source->subresource->base_mip_level ==
        2);
    REQUIRE(
        source->subresource->base_array_layer ==
        1);

    auto overlap = config;
    overlap["compute_tasks"][0]
           ["resource_ports"]["reduced_depth"]
           ["subresource"]["mip"] = 2;
    REQUIRE_THROWS_WITH(
        Pelican::TestSupport::computeTaskDefinitionsOf(
            overlap),
        Catch::Matchers::ContainsSubstring(
            "overlapping shader read/write subresources"));

    auto implicit = config;
    implicit["compute_tasks"][0]
            ["resource_ports"]["source_depth"]
            .erase("subresource");
    REQUIRE_THROWS_WITH(
        Pelican::TestSupport::computeTaskDefinitionsOf(
            implicit),
        Catch::Matchers::ContainsSubstring(
            "only through an explicit subresource"));
}

TEST_CASE(
    "compute task schedules distinguish frame and logical-view work",
    "[renderingpass][compute][schedule][xr][wp210]") {
    const auto config =
        nlohmann::json::parse(R"json({
          "compute_tasks": [
            {
              "name": "once",
              "shader": "shaders/once"
            },
            {
              "name": "per_eye",
              "shader": "shaders/per_eye",
              "schedule": "per_view",
              "view_family": "$reflection/probe/0"
            }
          ]
        })json");

    const auto tasks =
        Pelican::TestSupport::computeTaskDefinitionsOf(
            config);
    REQUIRE(tasks.size() == 2);
    REQUIRE(
        tasks[0].schedule ==
        ComputeTaskSchedule::per_frame);
    REQUIRE(
        tasks[1].schedule ==
        ComputeTaskSchedule::per_view);
    REQUIRE(
        tasks[0].view_family ==
        "$main");
    REQUIRE(
        tasks[1].view_family ==
        "$reflection/probe/0");
    REQUIRE(
        std::string{
            computeTaskScheduleName(
                tasks[0].schedule)} ==
        "per_frame");
    REQUIRE(
        std::string{
            computeTaskScheduleName(
                tasks[1].schedule)} ==
        "per_view");

    auto invalid = config;
    invalid["compute_tasks"][1]["schedule"] =
        "per_pass";
    TestSupport::requireNamedRefusal(
        [&] { (void)Pelican::TestSupport::computeTaskDefinitionsOf(invalid); },
        "render_graph.compute_task_invalid@1",
        {{"node", "per_eye"}, {"field", "schedule"}, {"reason", "not_per_frame_per_view_or_once"}});

    // PS-1: once is a third schedule.
    auto once = config;
    once["compute_tasks"][1]["schedule"] = "once";
    const auto once_tasks =
        Pelican::TestSupport::computeTaskDefinitionsOf(once);
    REQUIRE(std::string{computeTaskScheduleName(
                once_tasks[1].schedule)} == "once");
}

TEST_CASE(
    "ray tracing compute tasks parse supported stages and hard-reject out-of-scope stages",
    "[renderingpass][compute][ray-tracing][wp285]") {
    const auto config = nlohmann::json::parse(R"json({
      "compute_tasks": [{
        "name": "trace_mask",
        "ray_tracing": {
          "raygen": "shaders/trace_mask",
          "miss": "shaders/trace_mask",
          "closesthit": "shaders/trace_mask"
        },
        "writes": ["mask"],
        "resource_ports": {
          "mask": {"resource": "mask", "access": "storage"}
        },
        "dispatch": {"rays_from": {"port": "mask"}}
      }]
    })json");
    const auto tasks =
        Pelican::TestSupport::computeTaskDefinitionsOf(config);
    REQUIRE(tasks.size() == 1);
    REQUIRE(tasks.front().ray_tracing.has_value());
    CHECK(tasks.front().ray_tracing->raygen.stage ==
          ShaderStage::raygen);
    REQUIRE(tasks.front().ray_tracing->misses.size() == 1);
    CHECK(tasks.front().ray_tracing->misses.front().stage ==
          ShaderStage::miss);
    REQUIRE(
        tasks.front().ray_tracing->closest_hits.size() == 1);
    CHECK(tasks.front().ray_tracing->closest_hits.front().stage ==
          ShaderStage::closesthit);
    CHECK(tasks.front().ray_tracing->max_recursion == 1);
    REQUIRE(tasks.front().dispatch.rays_from.has_value());
    CHECK(tasks.front().dispatch.rays_from->port == "mask");

    auto explicit_rays = config;
    explicit_rays["compute_tasks"][0]["dispatch"] = {{"rays", {2, 3, 4}}};
    const auto ray_tasks = Pelican::TestSupport::computeTaskDefinitionsOf(explicit_rays);
    REQUIRE(ray_tasks[0].dispatch.rays.has_value());
    CHECK(*ray_tasks[0].dispatch.rays == std::array<std::uint32_t, 3>{2, 3, 4});
    CHECK_FALSE(ray_tasks[0].dispatch.rays_from.has_value());
    for (const auto &bad : std::vector<nlohmann::json>{
            {0, 1, 1}, {-1, 1, 1}, {1.5, 1, 1}, {4294967296ULL, 1, 1},
            {1, 1}, {1, 1, 1, 1}, "2,3,4"}) {
        auto invalid = explicit_rays;
        invalid["compute_tasks"][0]["dispatch"]["rays"] = bad;
        CHECK_THROWS_WITH(Pelican::TestSupport::computeTaskDefinitionsOf(invalid),
            Catch::Matchers::ContainsSubstring("invalid_trace_dispatch"));
    }
    auto mixed_rays = explicit_rays;
    mixed_rays["compute_tasks"][0]["dispatch"]["rays_from"] = {{"port", "mask"}};
    CHECK_THROWS_WITH(Pelican::TestSupport::computeTaskDefinitionsOf(mixed_rays),
        Catch::Matchers::ContainsSubstring("invalid_trace_dispatch"));
    auto compute_rays = explicit_rays;
    compute_rays["compute_tasks"][0].erase("ray_tracing");
    compute_rays["compute_tasks"][0]["shader"] = "shaders/compute";
    CHECK_THROWS_WITH(Pelican::TestSupport::computeTaskDefinitionsOf(compute_rays),
        Catch::Matchers::ContainsSubstring("invalid_trace_dispatch"));

    auto explicit_depth = config;
    explicit_depth["compute_tasks"][0]["ray_tracing"]
                  ["max_recursion"] = 2;
    const auto depth_tasks =
        Pelican::TestSupport::computeTaskDefinitionsOf(explicit_depth);
    CHECK(depth_tasks.front().ray_tracing->max_recursion == 2);

    for (const auto &invalid_depth :
         {nlohmann::json{0}, nlohmann::json{-1},
          nlohmann::json{1.5}, nlohmann::json{"2"}}) {
        auto invalid = config;
        invalid["compute_tasks"][0]["ray_tracing"]
               ["max_recursion"] = invalid_depth;
        REQUIRE_THROWS_WITH(
            Pelican::TestSupport::computeTaskDefinitionsOf(invalid),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.invalid_recursion@1"));
    }

    auto with_callable = config;
    with_callable["compute_tasks"][0]["ray_tracing"]["callable"] = {"shaders/a", "shaders/b"};
    const auto callable_tasks = Pelican::TestSupport::computeTaskDefinitionsOf(with_callable);
    REQUIRE(callable_tasks[0].ray_tracing->callables.size() == 2);
    CHECK(callable_tasks[0].ray_tracing->callables[1].stage == ShaderStage::callable);
    with_callable["compute_tasks"][0]["ray_tracing"]["callable_record"] = {
        {{"name", "increment"}, {"type", "u32"}}};
    with_callable["compute_tasks"][0]["ray_tracing"]["callable_records"] = {
        {{"increment", 100}}, {{"increment", 200}}};
    const auto record_tasks = Pelican::TestSupport::computeTaskDefinitionsOf(with_callable);
    REQUIRE(record_tasks[0].ray_tracing->callable_records.size() == 2);
    CHECK(record_tasks[0].ray_tracing->callable_records[1].size() == 4);
    with_callable["compute_tasks"][0]["ray_tracing"]["callable_records"].erase(1);
    CHECK_THROWS_WITH(Pelican::TestSupport::computeTaskDefinitionsOf(with_callable),
        Catch::Matchers::ContainsSubstring("callable_records_invalid"));


    for (const auto *unsupported :
         {"any_hit", "intersection"}) {
        auto invalid = config;
        invalid["compute_tasks"][0]["ray_tracing"]
               [unsupported] = "shaders/unsupported";
        REQUIRE_THROWS_WITH(
            Pelican::TestSupport::computeTaskDefinitionsOf(invalid),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.unsupported_shader_stage@1"));

        invalid = config;
        invalid["compute_tasks"][0][unsupported] =
            "shaders/unsupported";
        REQUIRE_THROWS_WITH(
            Pelican::TestSupport::computeTaskDefinitionsOf(invalid),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.unsupported_shader_stage@1"));
    }
}

TEST_CASE(
    "compute image dispatch derives groups from a typed resource port",
    "[renderingpass][compute][dispatch][extent][wp231]") {
    const auto config =
        nlohmann::json::parse(R"json({
          "compute_tasks": [{
            "name": "reduce_reflection",
            "shader": "shaders/reduce_reflection",
            "reads": ["reflection"],
            "writes": ["reflection"],
            "resource_ports": {
              "source": {
                "resource": "reflection",
                "access": "sampled",
                "subresource": {"mip": 1}
              },
              "reduced": {
                "resource": "reflection",
                "access": "storage",
                "subresource": {"mip": 2}
              }
            },
            "dispatch": {
              "groups_from": {"port": "reduced"}
            }
          }]
        })json");

    const auto tasks =
        Pelican::TestSupport::computeTaskDefinitionsOf(
            config);
    REQUIRE(tasks.size() == 1);
    REQUIRE(
        tasks.front().dispatch.groups_from
            .has_value());
    REQUIRE(
        tasks.front().dispatch.groups_from
            ->port == "reduced");

    auto mixed = config;
    mixed["compute_tasks"][0]["dispatch"]
         ["groups"] =
        nlohmann::json::array({1, 1, 1});
    TestSupport::requireNamedRefusal(
        [&] { (void)Pelican::TestSupport::computeTaskDefinitionsOf(mixed); },
        "render_graph.compute_task_invalid@1",
        {{"node", "reduce_reflection"}, {"field", "dispatch.groups"}, {"reason", "combined_with_groups_from"}});

    auto legacy_scalar = config;
    legacy_scalar["compute_tasks"][0]
                 ["dispatch"]["groups_from"] =
        "pixel_count";
    TestSupport::requireNamedRefusal(
        [&] { (void)Pelican::TestSupport::computeTaskDefinitionsOf(legacy_scalar); },
        "render_graph.compute_task_invalid@1",
        {{"node", "reduce_reflection"}, {"field", "dispatch.groups_from"}, {"reason", "not_an_object"}});

    auto authored_local_size = config;
    authored_local_size["compute_tasks"][0]
                       ["dispatch"]["local_size"] =
        64;
    TestSupport::requireNamedRefusal(
        [&] { (void)Pelican::TestSupport::computeTaskDefinitionsOf(authored_local_size); },
        "render_graph.compute_task_invalid@1",
        {{"node", "reduce_reflection"}, {"field", "dispatch.local_size"}, {"reason", "declared_by_the_shader"}});

    auto missing_port = config;
    missing_port["compute_tasks"][0]
                ["dispatch"]["groups_from"]
                ["port"] = "missing";
    TestSupport::requireNamedRefusal(
        [&] { (void)Pelican::TestSupport::computeTaskDefinitionsOf(missing_port); },
        "render_graph.compute_task_invalid@1",
        {{"node", "reduce_reflection"}, {"field", "dispatch.groups_from.port"}, {"reason", "unknown_resource_port"}});

    const auto buffer_port =
        nlohmann::json::parse(R"json({
          "compute_tasks": [{
            "name": "write_buffer",
            "shader": "shaders/write_buffer",
            "writes": ["output"],
            "resource_ports": {
              "output": {
                "resource": "output",
                "kind": "buffer",
                "element": "uint",
                "access": "storage"
              }
            },
            "dispatch": {
              "groups_from": {"port": "output"}
            }
          }]
        })json");
    TestSupport::requireNamedRefusal(
        [&] { (void)Pelican::TestSupport::computeTaskDefinitionsOf(buffer_port); },
        "render_graph.compute_task_invalid@1",
        {{"node", "write_buffer"}, {"field", "dispatch.groups_from.port"}, {"reason", "not_an_image"}});
}

TEST_CASE(
    "typed lighting buffers derive size from render extent and parse compute ports",
    "[renderingpass][resource-port][clustered][wp208]") {
    const auto config =
        nlohmann::json::parse(R"json({
          "render_targets": [
            {
              "name": "display",
              "width": 1280,
              "height": 720,
              "extent_scale": 1.0,
              "format": "B8G8R8A8_SRGB",
              "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
            },
            {
              "name": "lit_color",
              "extent_scale": 0.5,
              "format": "R16G16B16A16_SFLOAT",
              "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
            }
          ],
          "buffers": [
            {
              "name": "light_inventory",
              "size": 65568,
              "host_source": "scene_lights_v2"
            },
            {
              "name": "light_selection",
              "size_from_extent": {
                "resource": "lit_color",
                "tile_width": 32,
                "tile_height": 32,
                "header_bytes": 32,
                "bytes_per_tile": 260,
                "copies": 2
              }
            }
          ],
          "compute_tasks": [
            {
              "name": "select_lights",
              "shader": "shaders/select_lights",
              "reads": ["light_inventory"],
              "writes": ["light_selection"],
              "resource_ports": {
                "inventory": {
                  "resource": "light_inventory",
                  "kind": "buffer",
                  "element": "uvec4",
                  "access": "storage"
                },
                "selection": {
                  "resource": "light_selection",
                  "kind": "buffer",
                  "element": "uint",
                  "access": "storage"
                }
              }
            }
          ]
        })json");

    auto matrix_config = nlohmann::json::parse(R"({"buffers":[{
        "name":"capture_matrices", "size":400,
        "host_source":"view_family_matrices_v1", "view_family":"$project/capture"
    }]})");
    const auto matrix_buffers = Pelican::test_support::bufferDefinitionsOf(matrix_config);
    REQUIRE(matrix_buffers.size() == 1);
    CHECK(matrix_buffers[0].host_view_family == "$project/capture");
    CHECK(matrix_buffers[0].host_source == FrameGraphHostBufferSource::view_family_matrices_v1);
    matrix_config["buffers"][0].erase("view_family");
    CHECK_THROWS(Pelican::test_support::bufferDefinitionsOf(matrix_config));

    // RC-3c2a: the selection is sized from lit_color as the generation
    // resolves it: half of a 1280x720 output.
    const auto buffers =
        Pelican::test_support::bufferDefinitionsOf(
            config, {1280, 720});
    REQUIRE(buffers.size() == 2);
    REQUIRE(
        buffers[0].host_source ==
        FrameGraphHostBufferSource::
            scene_lights_v2);
    REQUIRE(buffers[0].size == 65568);
    REQUIRE(buffers[1].extent_size.has_value());
    REQUIRE(
        buffers[1].extent_size->resource ==
        "lit_color");
    REQUIRE(
        buffers[1].size ==
        2u * (32u + 20u * 12u * 260u));
    REQUIRE(
        buffers[1].extent_size->copies ==
        2);

    const auto tasks =
        Pelican::TestSupport::computeTaskDefinitionsOf(
            config);
    REQUIRE(tasks.size() == 1);
    REQUIRE(tasks[0].resource_ports.size() == 2);
    REQUIRE(
        tasks[0].resource_ports[0].kind ==
        ShaderResourcePortKind::buffer);
    REQUIRE(
        tasks[0].resource_ports[0]
            .buffer_element ==
        ShaderResourceBufferElement::uvec4);
    REQUIRE(
        tasks[0].resource_ports[1]
            .buffer_element ==
        ShaderResourceBufferElement::
            unsigned_integer);

    auto invalid = config;
    invalid["buffers"][1]["size"] = 16;
    REQUIRE_THROWS_WITH(
        Pelican::test_support::bufferDefinitionsOf(
            invalid),
        Catch::Matchers::ContainsSubstring(
            R"(render_graph.buffer_invalid@1 {"field":"size","reason":"with_size_from_extent")"));

    invalid = config;
    invalid["buffers"][1]
           ["size_from_extent"]["copies"] =
        0;
    REQUIRE_THROWS_WITH(
        Pelican::test_support::bufferDefinitionsOf(
            invalid),
        Catch::Matchers::ContainsSubstring(
            R"(render_graph.buffer_invalid@1 {"field":"size_from_extent","reason":"not_positive")"));
}

TEST_CASE(
    "typed indirect compute and ray dispatch create graph reads and validate command buffers",
    "[renderingpass][compute][indirect][wp210]") {
    auto config =
        nlohmann::json::parse(R"json({
          "buffers": [
            {
              "name": "dispatch_arguments",
              "size": 24,
              "command_layout": "compute_dispatch"
            },
            {
              "name": "result",
              "size": 4
            }
          ],
          "compute_tasks": [
            {
              "name": "build_dispatch",
              "shader": "shaders/build_dispatch",
              "writes": ["dispatch_arguments"],
              "dispatch": {"groups": [1, 1, 1]}
            },
            {
              "name": "consume_dispatch",
              "shader": "shaders/consume_dispatch",
              "writes": ["result"],
              "dispatch": {
                "indirect": {
                  "buffer": "dispatch_arguments",
                  "offset": 12
                }
              }
            }
          ]
        })json");

    const bool ray = GENERATE(false, true);
    if (ray) {
        config["buffers"][0]["command_layout"] = "ray_dispatch";
        config["compute_tasks"][1].erase("shader");
        config["compute_tasks"][1]["ray_tracing"] = {
            {"raygen", "shaders/trace"}, {"miss", "shaders/miss"},
            {"closesthit", "shaders/hit"}};
    }
    const auto buffers =
        Pelican::test_support::bufferDefinitionsOf(
            config);
    const auto tasks =
        Pelican::TestSupport::computeTaskDefinitionsOf(
            config);
    REQUIRE(
        buffers.at(0).command_layout ==
        (ray ? FrameGraphBufferCommandLayout::ray_dispatch
             : FrameGraphBufferCommandLayout::compute_dispatch));
    REQUIRE(
        std::string{
            frameGraphBufferCommandLayoutName(
                *buffers.at(0).command_layout)} ==
        (ray ? "ray_dispatch" : "compute_dispatch"));
    REQUIRE(tasks.at(1).dispatch.indirect.has_value());
    REQUIRE(
        tasks.at(1).dispatch.indirect->buffer ==
        "dispatch_arguments");
    REQUIRE(
        tasks.at(1).dispatch.indirect->offset == 12);
    REQUIRE_NOTHROW(
        validateComputeTaskBufferContracts(
            TestSupport::renderGraphOf(config), buffers));

    const auto graphs =
        TestSupport::frameGraphsOf(
            config);
    REQUIRE(graphs.size() == 1);
    const auto consumer = std::find_if(
        graphs.front().nodes.begin(),
        graphs.front().nodes.end(),
        [](const FrameGraphNodeDefinition &node) {
            return node.name ==
                   "consume_dispatch";
        });
    REQUIRE(consumer != graphs.front().nodes.end());
    REQUIRE(
        std::find(
            consumer->reads.begin(),
            consumer->reads.end(),
            "dispatch_arguments") !=
        consumer->reads.end());
    const auto plan =
        planFrameGraph(graphs.front());
    REQUIRE(
        framePlanOrder(plan) ==
        std::vector<std::string>{
            "build_dispatch",
            "consume_dispatch"});
    REQUIRE(plan.barriers.size() == 1);
    REQUIRE(
        plan.barriers.front().resource ==
        "dispatch_arguments");
    REQUIRE(
        plan.barriers.front().from ==
        "build_dispatch");
    REQUIRE(
        plan.barriers.front().to ==
        "consume_dispatch");

    auto mixed = config;
    mixed["compute_tasks"][1]["dispatch"]["groups"] =
        nlohmann::json::array({1, 1, 1});
    if (ray) {
        // A ray tracing task's launch keeps its ray tracing refusal.
        REQUIRE_THROWS_WITH(
            Pelican::TestSupport::computeTaskDefinitionsOf(mixed),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.invalid_trace_dispatch@1"));
    } else {
        TestSupport::requireNamedRefusal(
            [&] { (void)Pelican::TestSupport::computeTaskDefinitionsOf(mixed); },
            "render_graph.compute_task_invalid@1",
            {{"node", "consume_dispatch"}, {"field", "dispatch.indirect"}, {"reason", "combined_with_groups"}});
    }

    auto unaligned = config;
    unaligned["compute_tasks"][1]
             ["dispatch"]["indirect"]["offset"] =
        2;
    TestSupport::requireNamedRefusal(
        [&] { (void)Pelican::TestSupport::computeTaskDefinitionsOf(unaligned); },
        "render_graph.compute_task_invalid@1",
        {{"node", "consume_dispatch"}, {"field", "dispatch.indirect.offset"}, {"reason", "not_4_byte_aligned"}});

    auto wrong_layout = config;
    wrong_layout["buffers"][0].erase(
        "command_layout");
    const auto untyped_buffers =
        Pelican::test_support::bufferDefinitionsOf(
            wrong_layout);
    // RC-3c2b1: the IR's task against the sized buffers, refused by name.
    TestSupport::requireNamedRefusal(
        [&] {
            validateComputeTaskBufferContracts(
                TestSupport::renderGraphOf(config), untyped_buffers);
        },
        "render_graph.compute_task_invalid@1",
        {{"node", "consume_dispatch"},
         {"field", "dispatch.indirect.buffer"},
         {"reason", "command_layout_mismatch"},
         {"buffer", "dispatch_arguments"},
         {"expected", ray ? "ray_dispatch" : "compute_dispatch"}});

    auto unknown_buffer = config;
    unknown_buffer["compute_tasks"][1]["dispatch"]["indirect"]["buffer"] =
        "missing_arguments";
    TestSupport::requireNamedRefusal(
        [&] {
            validateComputeTaskBufferContracts(
                TestSupport::renderGraphOf(unknown_buffer), buffers);
        },
        "render_graph.compute_task_invalid@1",
        {{"node", "consume_dispatch"},
         {"reason", "unknown_buffer"},
         {"buffer", "missing_arguments"}});

    auto self_writing = config;
    self_writing["compute_tasks"][1]["writes"].push_back(
        "dispatch_arguments");
    TestSupport::requireNamedRefusal(
        [&] {
            validateComputeTaskBufferContracts(
                TestSupport::renderGraphOf(self_writing), buffers);
        },
        "render_graph.compute_task_invalid@1",
        {{"node", "consume_dispatch"},
         {"reason", "written_by_task"},
         {"buffer", "dispatch_arguments"}});

    auto out_of_bounds = config;
    out_of_bounds["compute_tasks"][1]
                 ["dispatch"]["indirect"]["offset"] =
        16;
    TestSupport::requireNamedRefusal(
        [&] {
            validateComputeTaskBufferContracts(
                TestSupport::renderGraphOf(out_of_bounds), buffers);
        },
        "render_graph.compute_task_invalid@1",
        {{"node", "consume_dispatch"},
         {"field", "dispatch.indirect.offset"},
         {"reason", "command_exceeds_buffer"},
         {"offset", 16},
         {"size", 24}});
}

TEST_CASE(
    "typed GPU draw source creates render dependencies and validates fixed-state command buffers",
    "[renderingpass][indirect][draw][wp210]") {
    const auto config =
        nlohmann::json::parse(R"json({
          "buffers": [
            {
              "name": "candidate_draws",
              "size": 40,
              "host_source": "scene_draw_commands_v1",
              "command_layout": "indexed_draw"
            },
            {
              "name": "candidate_bounds",
              "size": 64,
              "host_source": "scene_draw_bounds_v1"
            },
            {
              "name": "visible_draws",
              "size": 40,
              "command_layout": "indexed_draw"
            },
            {
              "name": "visible_draw_count",
              "size": 4,
              "command_layout": "draw_count"
            }
          ],
          "compute_tasks": [
            {
              "name": "build_visible_draws",
              "shader": "shaders/build_visible_draws",
              "reads": ["candidate_draws", "candidate_bounds"],
              "writes": ["visible_draws", "visible_draw_count"],
              "before": ["geometry"],
              "dispatch": {"groups": [1, 1, 1]}
            }
          ],
          "rendering_passes": [
            {
              "name": "main",
              "passes": [
                {
                  "name": "geometry",
                  "type": "material",
                  "material_range": {"start": 0, "count": 1},
                  "gpu_draw_source": {
                    "commands": "visible_draws",
                    "count": "visible_draw_count",
                    "max_draw_count": 2
                  },
                  "output": {
                    "color": "swapchain",
                    "depth": null
                  }
                }
              ]
            }
          ]
        })json");

    const auto buffers =
        Pelican::test_support::bufferDefinitionsOf(
            config);
    REQUIRE(
        buffers.at(0).host_source ==
        FrameGraphHostBufferSource::
            scene_draw_commands_v1);
    REQUIRE(
        buffers.at(0).command_layout ==
        FrameGraphBufferCommandLayout::
            indexed_draw);
    REQUIRE(
        buffers.at(1).host_source ==
        FrameGraphHostBufferSource::
            scene_draw_bounds_v1);
    REQUIRE(
        std::string{
            frameGraphHostBufferSourceName(
                *buffers.at(1).host_source)} ==
        "scene_draw_bounds_v1");
    REQUIRE(
        std::string{
            frameGraphBufferCommandLayoutName(
                *buffers.at(3).command_layout)} ==
        "draw_count");
    REQUIRE_NOTHROW(
        validateGpuDrawSourceBufferContracts(TestSupport::renderGraphOf(config), buffers));

    const auto source =
        TestSupport::renderingPassDefinitionsOf(config).at(0).passes.at(0).materialInfo().gpu_draw_source;
    REQUIRE(source.has_value());
    REQUIRE(source->commands == "visible_draws");
    REQUIRE(source->count == "visible_draw_count");
    REQUIRE(source->max_draw_count == 2);
    REQUIRE(source->command_offset == 0);
    REQUIRE(source->count_offset == 0);

    const auto graphs =
        TestSupport::frameGraphsOf(
            config);
    REQUIRE(graphs.size() == 1);
    const auto plan =
        planFrameGraph(graphs.front());
    REQUIRE(
        framePlanOrder(plan) ==
        std::vector<std::string>{
            "build_visible_draws", "geometry"});
    REQUIRE(plan.barriers.size() == 2);
    REQUIRE(
        std::any_of(
            plan.barriers.begin(),
            plan.barriers.end(),
            [](const auto &barrier) {
                return barrier.resource ==
                       "visible_draws";
            }));
    REQUIRE(
        std::any_of(
            plan.barriers.begin(),
            plan.barriers.end(),
            [](const auto &barrier) {
                return barrier.resource ==
                       "visible_draw_count";
            }));

    auto wrong_layout = config;
    wrong_layout["buffers"][2]
                ["command_layout"] =
        "draw_count";
    const auto wrong_layout_buffers =
        Pelican::test_support::bufferDefinitionsOf(
            wrong_layout);
    REQUIRE_THROWS_WITH(
        validateGpuDrawSourceBufferContracts(TestSupport::renderGraphOf(wrong_layout),
            wrong_layout_buffers),
        Catch::Matchers::ContainsSubstring(
            "requires command_layout 'indexed_draw'"));

    auto too_small = config;
    too_small["buffers"][2]["size"] = 20;
    const auto too_small_buffers =
        Pelican::test_support::bufferDefinitionsOf(
            too_small);
    TestSupport::requireNamedRefusal([&] {
        validateGpuDrawSourceBufferContracts(TestSupport::renderGraphOf(too_small), too_small_buffers);
    }, "render_graph.gpu_draw_source_invalid@1", {{"graph", "main"}, {"node", "geometry"},
        {"field", "gpu_draw_source.command_offset"}, {"reason", "command_range_exceeds_buffer"},
        {"buffer", "visible_draws"}, {"bytes", 40}, {"offset", 0}, {"size", 20}});

    auto unaligned = config;
    unaligned["rendering_passes"][0]["passes"][0]
             ["gpu_draw_source"]["count_offset"] =
        2;
    // RC-3c2a: the IR's read of the declaration refuses it, before any
    // buffer is looked at.
    REQUIRE_THROWS_WITH(
        validateGpuDrawSourceBufferContracts(TestSupport::renderGraphOf(unaligned), buffers),
        Catch::Matchers::ContainsSubstring(
            R"("field":"gpu_draw_source","graph":"main","node":"geometry","reason":"offset_unaligned")"));

    auto multiple_ranges = config;
    multiple_ranges["rendering_passes"][0]["passes"][0]
                   ["material_range"]["count"] =
        2;
    REQUIRE_THROWS_WITH(
        validateGpuDrawSourceBufferContracts(TestSupport::renderGraphOf(multiple_ranges), buffers),
        Catch::Matchers::ContainsSubstring(
            "fixed_state_requires_one_material"));

    auto wrong_pass = config;
    wrong_pass["rendering_passes"][0]["passes"][0]
              ["type"] =
        "fullscreen";
    REQUIRE_THROWS_WITH(
        validateGpuDrawSourceBufferContracts(TestSupport::renderGraphOf(wrong_pass), buffers),
        Catch::Matchers::ContainsSubstring(
            "does not own field 'material_range'"));

    auto invalid_host_source = config;
    invalid_host_source["buffers"][0]
                       ["command_layout"] =
        "draw_count";
    REQUIRE_THROWS_WITH(
        Pelican::test_support::bufferDefinitionsOf(
            invalid_host_source),
        Catch::Matchers::ContainsSubstring(
            "scene_draw_commands_requires_indexed_draw"));

    auto invalid_bounds_size = config;
    invalid_bounds_size["buffers"][1]["size"] =
        40;
    REQUIRE_THROWS_WITH(
        Pelican::test_support::bufferDefinitionsOf(
            invalid_bounds_size),
        Catch::Matchers::ContainsSubstring(
            "scene_draw_bounds_not_whole_records"));

    auto invalid_bounds_layout = config;
    invalid_bounds_layout["buffers"][1]
                         ["command_layout"] =
        "indexed_draw";
    REQUIRE_THROWS_WITH(
        Pelican::test_support::bufferDefinitionsOf(
            invalid_bounds_layout),
        Catch::Matchers::ContainsSubstring(
            "scene_draw_records_are_data"));

    auto segmented = config;
    segmented["buffers"].push_back({
        {"name", "draw_segments"},
        {"size", 128},
        {"host_source",
         "scene_draw_segments_v1"},
    });
    segmented["rendering_passes"][0]["passes"][0]
             ["material_range"]["count"] = 2;
    auto &segmented_source =
        segmented["rendering_passes"][0]
                 ["passes"][0]
                 ["gpu_draw_source"];
    segmented_source["layout"] =
        "draw_queue_segments_v1";
    segmented_source["segments"] =
        "draw_segments";
    const auto segmented_buffers =
        Pelican::test_support::bufferDefinitionsOf(
            segmented);
    REQUIRE_NOTHROW(
        validateGpuDrawSourceBufferContracts(TestSupport::renderGraphOf(segmented),
            segmented_buffers));
    const auto parsed_segmented =
        TestSupport::renderingPassDefinitionsOf(segmented).at(0).passes.at(0).materialInfo().gpu_draw_source;
    REQUIRE(parsed_segmented.has_value());
    CHECK(
        parsed_segmented->layout ==
        GpuDrawSourceLayout::
            draw_queue_segments_v1);
    CHECK(
        std::string{
            frameGraphHostBufferSourceName(
                *segmented_buffers.back()
                     .host_source)} ==
        "scene_draw_segments_v1");

    auto wrong_segment_source = segmented;
    wrong_segment_source["buffers"].back()
                        .erase("host_source");
    const auto wrong_segment_buffers =
        Pelican::test_support::bufferDefinitionsOf(
            wrong_segment_source);
    REQUIRE_THROWS_WITH(
        validateGpuDrawSourceBufferContracts(TestSupport::renderGraphOf(wrong_segment_source),
            wrong_segment_buffers),
        Catch::Matchers::ContainsSubstring(
            "scene_draw_segments_v1"));

    auto invalid_segment_size = segmented;
    invalid_segment_size["buffers"].back()
                        ["size"] = 40;
    REQUIRE_THROWS_WITH(
        Pelican::test_support::bufferDefinitionsOf(
            invalid_segment_size),
        Catch::Matchers::ContainsSubstring(
            "scene_draw_segments_not_whole_records"));

    auto invalid_segment_layout = segmented;
    invalid_segment_layout["buffers"].back()
                          ["command_layout"] =
        "indexed_draw";
    REQUIRE_THROWS_WITH(
        Pelican::test_support::bufferDefinitionsOf(
            invalid_segment_layout),
        Catch::Matchers::ContainsSubstring(
            "scene_draw_records_are_data"));

    auto missing_segments = segmented;
    missing_segments["rendering_passes"][0]
                    ["passes"][0]
                    ["gpu_draw_source"]
                    .erase("segments");
    REQUIRE_THROWS_WITH(
        TestSupport::renderingPassDefinitionsOf(missing_segments),
        Catch::Matchers::ContainsSubstring(
            R"("reason":"segments_required")"));
}

TEST_CASE("fullscreen pass JSON parser rejects explicit shader files and names the stem form", "[renderingpass]") {
    const nlohmann::json pass_json{
        {"shader", {{"vertex", "fullscreen.vert.spv"}, {"fragment", "lighting"}}},
    };

    TestSupport::requireNamedRefusal([&] { (void)fullscreenInfoOf(pass_json, "lighting_pass"); },
            "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "lighting_pass"}, {"field", "shader.vertex"}, {"reason", "explicit_extension"}, {"ref", "fullscreen.vert.spv"}});

}

TEST_CASE("fullscreen pass JSON parser does not infer options from pass name", "[renderingpass]") {
    const nlohmann::json pass_json{
        {"shader", {{"vertex", "fullscreen"}, {"fragment", "lighting"}}},
    };

    const auto info = fullscreenInfoOf(pass_json, "lighting_pass");

    REQUIRE(info.push_constants == FullscreenPushConstantData::eNone);
    REQUIRE_FALSE(info.uses_light_data);
}

TEST_CASE("pass info JSON parser reads shadow depth shader option", "[renderingpass]") {
    using Json = nlohmann::json;
    const auto definition = [](Json fields) {
        fields["name"] = "shadow_depth";
        fields["type"] = "shadow_depth";
        fields["output"] = {{"color", nullptr}, {"depth", "depth"}};
        return TestSupport::renderingPassDefinitionsOf(Json{
            {"render_targets", Json::array({Json{{"name", "depth"}, {"extent_scale", 1.0},
                {"format", "D32_SFLOAT"}, {"usage", Json::array({"DEPTH_STENCIL_ATTACHMENT"})}}})},
            {"passes", Json::array({std::move(fields)})}}).at(0).passes.at(0);
    };
    const auto pass_def = definition(Json::object());
    REQUIRE(pass_def.isShadowDepth());
    REQUIRE(pass_def.shadowDepthInfo().vert_shader.ref == "engine://shadow_depth");
    REQUIRE(pass_def.shadowDepthInfo().vert_shader.kind == ShaderReferenceKind::stem);
    const auto custom = definition(Json{{"shader", {{"vertex", "shaders/custom_shadow"}}}});
    REQUIRE(custom.shadowDepthInfo().vert_shader.ref == "shaders/custom_shadow");
    TestSupport::requireNamedRefusal([&] { (void)definition(Json{{"shader", Json::object()}}); },
        "render_graph.pass_execution_invalid@1", {{"node", "shadow_depth"}, {"field", "shader.vertex"}, {"reason", "required"}});
}

TEST_CASE("fullscreen pass JSON parser rejects deprecated projection matrix flag", "[renderingpass]") {
    const nlohmann::json pass_json{
        {"needs_projection_matrix", true},
        {"shader", {{"vertex", "fullscreen"}, {"fragment", "ssao"}}},
    };

    TestSupport::requireNamedRefusal([&] { (void)fullscreenInfoOf(pass_json, "ssao_pass"); },
            "render_graph.pass_field_ownership@1", {{"graph", "main"}, {"node", "ssao_pass"}});
}

TEST_CASE("pass info JSON parser reads pass type and applies UI defaults", "[renderingpass]") {
    using Json = nlohmann::json;
    const auto pass_def = TestSupport::renderingPassDefinitionsOf(Json{{"passes", Json::array({
        Json{{"name", "ui"}, {"type", "ui"}, {"output", {{"color", "swapchain"}, {"depth", nullptr}}}}})}}).at(0).passes.at(0);
    REQUIRE(pass_def.isUi());
    REQUIRE(pass_def.color_load_op == AttachmentLoadOp::load);
    REQUIRE(pass_def.color_store_op == AttachmentStoreOp::store);
}

TEST_CASE("pass info JSON parser applies fullscreen info only to fullscreen passes", "[renderingpass]") {
    const nlohmann::json fullscreen_json{
        {"shader", {{"vertex", "fullscreen"}, {"fragment", "debug_texture"}}},
        {"push_constants", "projection_view"},
    };
    const auto fullscreen_info = fullscreenInfoOf(fullscreen_json, "debug_texture");
    REQUIRE(fullscreen_info.frag_shader.ref == "debug_texture");
    REQUIRE(fullscreen_info.push_constants == FullscreenPushConstantData::eProjectionView);
    const auto material_pass = materialPassOf(nlohmann::json::object(), "geometry");
    REQUIRE(material_pass.isMaterial());
    TestSupport::requireNamedRefusal([&] { (void)materialPassOf(fullscreen_json, "geometry"); },
        "render_graph.pass_field_ownership@1", {{"graph", "main"}, {"node", "geometry"}});
}

TEST_CASE(
    "pass definition JSON parser keeps GPU draw sources material-only",
    "[renderingpass][wp303]") {
    const Json images = Json::array({fixtureImage("scene_color"), fixtureImage("scene_depth", true)});
    const Json buffers = Json::array({Json{{"name", "visible_draws"}, {"size", 320}, {"usage", Json::array({"STORAGE_BUFFER", "INDIRECT_BUFFER"})}}, Json{{"name", "visible_draw_count"}, {"size", 4}, {"usage", Json::array({"STORAGE_BUFFER", "INDIRECT_BUFFER"})}}});
    const auto name_resolver =
        RenderTargetNameResolver{
            [](const std::string &name) {
                if (name == "scene_color") {
                    return GlobalRenderTargetId{0};
                }
                if (name == "scene_depth") {
                    return GlobalRenderTargetId{1};
                }
                return noRenderTargetId();
            }};

    const nlohmann::json gpu_draw_source{
        {"commands", "visible_draws"},
        {"count", "visible_draw_count"},
        {"max_draw_count", 16},
    };
    const nlohmann::json fullscreen_json{
        {"name", "post_process"},
        {"type", "fullscreen"},
        {"output", {{"color", "scene_color"}, {"depth", nullptr}}},
        {"shader", {{"vertex", "fullscreen"}, {"fragment", "post_process"}}},
        {"gpu_draw_source", gpu_draw_source},
    };

    TestSupport::requireNamedRefusal([&] { (void)passInGraph(fullscreen_json, images, buffers, name_resolver); },
        "render_graph.pass_field_ownership@1", {{"graph", "main"}, {"node", "post_process"}});

    auto contract_on_fullscreen = fullscreen_json;
    contract_on_fullscreen.erase("gpu_draw_source");
    contract_on_fullscreen["material_contract"] =
        "forward_opaque_v1";
    TestSupport::requireNamedRefusal([&] { (void)passInGraph(contract_on_fullscreen, images, buffers, name_resolver); },
        "render_graph.pass_field_ownership@1", {{"graph", "main"}, {"node", "post_process"}});

    const nlohmann::json material_json{
        {"name", "gpu_geometry"},
        {"type", "material"},
        {"material_contract", "forward_opaque_v1"},
        {"material_range", {{"start", 0}, {"count", 1}}},
        {"gpu_draw_source", gpu_draw_source},
        {"output", {{"color", "scene_color"}, {"depth", "scene_depth"}}},
    };
    const auto material_pass =
        passInGraph(material_json, images, buffers, name_resolver);

    REQUIRE(material_pass.isMaterial());
    REQUIRE(material_pass.materialInfo().gpu_draw_source);
    CHECK(material_pass.materialInfo().gpu_draw_source->commands ==
          "visible_draws");
    CHECK(material_pass.materialInfo().gpu_draw_source->count ==
          "visible_draw_count");
    CHECK(material_pass.materialInfo().gpu_draw_source->max_draw_count ==
          16);
}

TEST_CASE("material pass selects an opaque named variant through an explicit bounded route",
          "[renderingpass][material-variant][wp206b]") {
    const auto authored = nlohmann::json{
        {"material_contract", "forward_opaque_v1"},
        {"material_filter",
         {{"include", {"outlined"}}}},
        {"material_variant", "silhouette"},
    };
    const auto pass = materialPassOf(authored, "silhouette_overlay");
    REQUIRE(pass.materialInfo().material_variant ==
            "silhouette");
    REQUIRE(pass.materialInfo().contract ==
            MaterialPassContract::forward_opaque_v1);
    REQUIRE(pass.materialInfo().material_filter
                ->include ==
            std::vector<std::string>{"outlined"});

    auto invalid = authored;
    invalid.erase("material_contract");
    TestSupport::requireNamedRefusal([&] { (void)materialPassOf(invalid, "silhouette_overlay"); },
        "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "silhouette_overlay"},
            {"field", "material_variant"}, {"reason", "requires_material_contract"}});

    invalid = authored;
    invalid["material_filter"]["include"] =
        nlohmann::json::array();
    TestSupport::requireNamedRefusal([&] { (void)materialPassOf(invalid, "silhouette_overlay"); },
        "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "silhouette_overlay"},
            {"field", "material_variant"}, {"reason", "requires_material_filter_include"}});

    invalid = authored;
    invalid["material_variant"] = "bad/name";
    TestSupport::requireNamedRefusal([&] { (void)materialPassOf(invalid, "silhouette_overlay"); },
        "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "silhouette_overlay"},
            {"field", "material_variant"}, {"reason", "invalid_name"}});
}

TEST_CASE("rendering pass target JSON parser applies output and input targets", "[renderingpass]") {
    const auto resolver = RenderTargetNameResolver{[](const std::string &name) {
        if (name == "color_target") {
            return GlobalRenderTargetId{3};
        }
        if (name == "depth_target") {
            return GlobalRenderTargetId{4};
        }
        if (name == "input_target") {
            return GlobalRenderTargetId{5};
        }
        return noRenderTargetId();
    }};


    const Json images = Json::array({fixtureImage("color_target"), fixtureImage("depth_target", true), fixtureImage("input_target")});
    const nlohmann::json pass_json{
        {"output", {{"color", "color_target"}, {"depth", "depth_target"}}},
        {"input", "input_target"},
    };

    const auto pass_def = targetPassOf(pass_json, "postprocess", images, resolver);

    REQUIRE(pass_def.output_color.size() == 1);
    REQUIRE(pass_def.output_color[0] == GlobalRenderTargetId{3});
    REQUIRE(pass_def.output_depth == GlobalRenderTargetId{4});
    REQUIRE(pass_def.input_targets.size() == 1);
    REQUIRE(pass_def.input_targets[0] == GlobalRenderTargetId{5});
}

TEST_CASE("rendering pass target JSON parser handles swapchain and omitted input", "[renderingpass]") {
    const auto resolver = RenderTargetNameResolver{[](const std::string &) { return noRenderTargetId(); }};


    const Json images = Json::array();
    const nlohmann::json pass_json{
        {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
    };

    const auto pass_def = targetPassOf(pass_json, "present", images, resolver);

    REQUIRE(pass_def.output_color.size() == 1);
    REQUIRE(isSwapchainRenderTarget(pass_def.output_color[0]));
    REQUIRE(pass_def.output_depth == noRenderTargetId());
    REQUIRE(pass_def.input_targets.empty());
}

TEST_CASE(
    "rendering pass target JSON parser preserves raster attachment subresources",
    "[renderingpass][attachment-subresource][wp235]") {
    const auto resolver =
        RenderTargetNameResolver{
            [](const std::string &name) {
                if (name == "probe_color") {
                    return GlobalRenderTargetId{3};
                }
                if (name == "probe_depth") {
                    return GlobalRenderTargetId{4};
                }
                return noRenderTargetId();
            }};
    const Json images = Json::array({fixtureImage("probe_color"), fixtureImage("probe_depth", true)});
    const auto authored = nlohmann::json{
        {"output",
         {
             {"color",
              {
                  {
                      {"target", "probe_color"},
                      {"subresource",
                       {
                           {"mip", 2},
                           {"layer", 6},
                           {"layer_count", 4},
                       }},
                  },
              }},
             {"depth",
              {
                  {"target", "probe_depth"},
                  {"subresource",
                   {
                       {"mip", 0},
                       {"layer", 10},
                       {"layer_count", 4},
                   }},
              }},
         }},
    };

    const auto pass = targetPassOf(authored, "probe_face", images, resolver);
    REQUIRE(pass.output_color.size() == 1);
    REQUIRE(
        pass.output_color.front().target ==
        GlobalRenderTargetId{3});
    REQUIRE((
        pass.output_color.front().subresource ==
        std::optional<ImageSubresourceRange>{
            ImageSubresourceRange{
                .base_mip_level = 2,
                .base_array_layer = 6,
                .layer_count = 4,
            }}));
    REQUIRE(
        pass.output_depth.target ==
        GlobalRenderTargetId{4});
    REQUIRE(
        pass.output_depth.subresource
            ->base_array_layer == 10);

    auto invalid = authored;
    invalid["output"]["color"][0]
           ["subresource"]["mip_count"] =
        "remaining";
    TestSupport::requireNamedRefusal([&] { (void)targetPassOf(invalid, "probe_face", images, resolver); },
        "render_graph.attachment_invalid@1", {{"graph", "main"}, {"node", "probe_face"}, {"field", "output.color"}, {"reason", "subresource_not_one_mip"}});

    invalid = authored;
    invalid["output"]["color"][0]
           ["unknown"] = true;
    TestSupport::requireNamedRefusal([&] { (void)targetPassOf(invalid, "probe_face", images, resolver); },
        "render_graph.attachment_invalid@1", {{"graph", "main"}, {"node", "probe_face"}, {"field", "output.color"}, {"reason", "unknown_field"}, {"key", "unknown"}});

    invalid = authored;
    invalid["output"]["color"][0]
           ["target"] = "swapchain";
    TestSupport::requireNamedRefusal([&] { (void)targetPassOf(invalid, "probe_face", images, resolver); },
        "render_graph.attachment_invalid@1", {{"graph", "main"}, {"node", "probe_face"}, {"field", "output.color"}, {"reason", "swapchain_subresource"}});
}

TEST_CASE(
    "WP352 output attachments accept the pass operation spellings without widening their keys",
    "[renderingpass][attachment-operations][wp352]") {
    using Json = nlohmann::json;
    const RenderTargetNameResolver names{[](const std::string &name) {
        return name == "color" ? GlobalRenderTargetId{3} : name == "depth" ? GlobalRenderTargetId{4} : noRenderTargetId();
    }};
    const auto definitions = [&](const Json &color, const Json &depth) {
        return TestSupport::renderingPassDefinitionsOf(Json{
            {"render_targets", Json::array({
                Json{{"name", "color"}, {"extent_scale", 1.0}, {"format", "R8G8B8A8_UNORM"}, {"usage", Json::array({"COLOR_ATTACHMENT"})}},
                Json{{"name", "depth"}, {"extent_scale", 1.0}, {"format", "D32_SFLOAT"}, {"usage", Json::array({"DEPTH_STENCIL_ATTACHMENT"})}}})},
            {"rendering_passes", Json::array({Json{{"name", "main"}, {"passes", Json::array({
                Json{{"name", "operations"}, {"type", "material"}, {"output", {{"color", color}, {"depth", depth}}}}})}}})}}, names).at(0).passes.at(0);
    };
    const std::vector<std::pair<std::string, AttachmentLoadOp>> loads{
        {"Clear", AttachmentLoadOp::clear}, {"clear", AttachmentLoadOp::clear},
        {"Load", AttachmentLoadOp::load}, {"load", AttachmentLoadOp::load},
        {"DontCare", AttachmentLoadOp::dont_care}, {"dont_care", AttachmentLoadOp::dont_care},
        {"dontCare", AttachmentLoadOp::dont_care}};
    for (const auto &[spelling, expected] : loads) {
        CAPTURE(spelling);
        const auto pass = definitions(Json::array({Json{{"target", "color"}, {"load_op", spelling}}}),
            Json{{"target", "depth"}, {"load_op", spelling}});
        REQUIRE(pass.output_color.size() == 1);
        CHECK(pass.output_color.front().target == GlobalRenderTargetId{3});
        CHECK(pass.output_depth.target == GlobalRenderTargetId{4});
        REQUIRE(pass.colorAttachmentOperations(0).load_op == expected);
        REQUIRE(pass.depthAttachmentOperations().load_op == expected);
    }
    const std::vector<std::pair<std::string, AttachmentStoreOp>> stores{
        {"Store", AttachmentStoreOp::store}, {"store", AttachmentStoreOp::store},
        {"DontCare", AttachmentStoreOp::dont_care}, {"dont_care", AttachmentStoreOp::dont_care},
        {"dontCare", AttachmentStoreOp::dont_care}};
    for (const auto &[spelling, expected] : stores) {
        CAPTURE(spelling);
        const auto pass = definitions(Json::array({Json{{"target", "color"}, {"store_op", spelling}}}),
            Json{{"target", "depth"}, {"store_op", spelling}});
        REQUIRE(pass.output_color.size() == 1);
        REQUIRE(pass.colorAttachmentOperations(0).store_op == expected);
        REQUIRE(pass.depthAttachmentOperations().store_op == expected);
    }
    TestSupport::requireNamedRefusal([&] {
        (void)definitions(Json::array({Json{{"target", "color"}, {"clear", Json::array({0, 0, 0, 1})}}}), "depth");
    }, "render_graph.attachment_invalid@1", {{"graph", "main"}, {"node", "operations"}, {"field", "output.color"}, {"reason", "unknown_field"}, {"key", "clear"}});
    TestSupport::requireNamedRefusal([&] {
        (void)definitions("color", Json{{"target", "depth"}, {"unknown", true}});
    }, "render_graph.attachment_invalid@1", {{"graph", "main"}, {"node", "operations"}, {"field", "output.depth"}, {"reason", "unknown_field"}, {"key", "unknown"}});
    TestSupport::requireNamedRefusal([&] {
        (void)definitions(Json::array({Json{{"target", "color"}, {"load_op", "preserve_somehow"}}}), "depth");
    }, "render_graph.attachment_invalid@1", {{"graph", "main"}, {"node", "operations"}, {"field", "output.color"}, {"reason", "unknown_op"}, {"target", "color"}, {"value", "preserve_somehow"}});
}

TEST_CASE("rendering pass target JSON parser rejects malformed pass outputs", "[renderingpass]") {
    const auto resolver = RenderTargetNameResolver{[](const std::string &) { return noRenderTargetId(); }};

    const Json images = Json::array();
    for (const auto &fields : std::vector<Json>{Json::object(), Json{{"output", 1}},
            Json{{"output", {{"color", "swapchain"}}}}}) {
        const bool missing_depth = fields.contains("output") && fields["output"].is_object();
        TestSupport::requireNamedRefusal([&] { (void)targetPassOf(fields, "bad_pass", images, resolver); },
            "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "bad_pass"},
                {"field", missing_depth ? "output.depth" : "output"}, {"reason", missing_depth ? "missing" : "requires_object"}});
    }
}

TEST_CASE("pass definition JSON parser builds a fullscreen pass definition", "[renderingpass]") {
    const Json images = Json::array({fixtureImage("half_color")});
    const Json buffers = Json::array();
    const auto name_resolver = RenderTargetNameResolver{[](const std::string &name) {
        if (name == "half_color") {
            return GlobalRenderTargetId{3};
        }
        return noRenderTargetId();
    }};


    const nlohmann::json pass_json{
        {"name", "debug_texture"},
        {"type", "fullscreen"},
        {"output", {{"color", "half_color"}, {"depth", nullptr}}},
        {"shader", {{"vertex", "fullscreen"}, {"fragment", "debug_texture"}}},
        {"implementation", {{"provider", "fixture.fullscreen"}}},
        {"view_family", "$reflection/probe/0"},
        {"regions",
         nlohmann::json::array(
             {"region.post.fixture", "region.post"})},
        {"push_constants", "projection_view"},
        {"uses_light_data", true},
        {"raster_state",
         {{"cull", "back"},
          {"color_attachments",
           nlohmann::json::array(
               {{{"blend", "additive"}}})}}},
        {"clear_color", nlohmann::json::array({0.0f, 0.0f, 0.0f, 1.0f})},
    };

    const auto pass_def = passInGraph(pass_json, images, buffers, name_resolver);

    REQUIRE(pass_def.name == "debug_texture");
    REQUIRE(pass_def.isFullscreen());
    REQUIRE(pass_def.output_color.size() == 1);
    REQUIRE(pass_def.output_color[0] == GlobalRenderTargetId{3});
    REQUIRE(pass_def.output_depth == noRenderTargetId());
    REQUIRE(pass_def.fullscreenInfo().frag_shader.ref == "debug_texture");
    REQUIRE(pass_def.fullscreenInfo().push_constants == FullscreenPushConstantData::eProjectionView);
    REQUIRE(pass_def.fullscreenInfo().uses_light_data);
    REQUIRE(pass_def.fullscreenInfo().raster_state.has_value());
    CHECK(
        pass_def.fullscreenInfo().raster_state->cull ==
        RasterCullMode::back);
    REQUIRE(
        pass_def.requested_implementation_provider ==
        std::optional<std::string>{"fixture.fullscreen"});
    REQUIRE_FALSE(pass_def.implementation_selection.has_value());
    REQUIRE(
        pass_def.region_tags ==
        std::vector<std::string>{
            "region.post.fixture", "region.post"});
    REQUIRE(
        pass_def.view_family ==
        "$reflection/probe/0");

    auto output_transform_json = pass_json;
    output_transform_json["name"] = "output_transform_fixture";
    output_transform_json["type"] = "output_transform";
    const auto output_transform =
        passInGraph(output_transform_json, images, buffers, name_resolver);
    REQUIRE(output_transform.isFullscreen());
    REQUIRE(
        output_transform.fullscreenInfo()
            .raster_state.has_value());
    REQUIRE(
        output_transform.fullscreenInfo().push_constants ==
        FullscreenPushConstantData::eProjectionView);
    REQUIRE(
        output_transform.fullscreenInfo().uses_light_data);

    const nlohmann::json non_owner{
        {"name", "ui_fixture"},
        {"type", "ui"},
        {"output",
         {{"color", "half_color"},
          {"depth", nullptr}}},
        {"raster_state", pass_json.at("raster_state")},
    };
    TestSupport::requireNamedRefusal([&] { (void)passInGraph(non_owner, images, buffers, name_resolver); },
        "render_graph.pass_field_ownership@1", {{"graph", "main"}, {"node", "ui_fixture"}});
}

TEST_CASE(
    "WP326 restores all three engine pass-shape rejections with resolved negative controls",
    "[renderingpass][frameplanner][pass-shape][wp326][negative-contrast]") {
    const RenderTargetNameResolver names{
        [](const std::string &name) {
            if (name == "lit_color") {
                return GlobalRenderTargetId{10};
            }
            if (name == "offscreen_depth") {
                return GlobalRenderTargetId{11};
            }
            if (name == "source_color") {
                return GlobalRenderTargetId{12};
            }
            if (name == "output_color") {
                return GlobalRenderTargetId{13};
            }
            if (name == "second_color") {
                return GlobalRenderTargetId{14};
            }
            return noRenderTargetId();
        }};
    const auto rejectionMessage = [](auto &&operation) {
        try {
            operation();
        } catch (const std::exception &error) {
            return std::string{error.what()};
        }
        return std::string{};
    };

    SECTION("regression 1 observes named material inputs after resolution") {
        auto formerly_accepted = nlohmann::json::parse(R"json({
          "name": "forward_transparent",
          "type": "material",
          "material_contract": "forward_transparent_v1",
          "screen_inputs": {"opaque_color": "lit_color"},
          "output": {
            "color": ["lit_color"],
            "depth": "offscreen_depth"
          }
        })json");
        const auto definitions = [&](const nlohmann::json &pass) {
            using Json = nlohmann::json;
            const Json config{
                {"render_targets", Json::array({
                    Json{{"name", "lit_color"}, {"extent_scale", 1.0},
                         {"format", "R16G16B16A16_SFLOAT"}, {"history", true},
                         {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}},
                    Json{{"name", "offscreen_depth"}, {"extent_scale", 1.0},
                         {"format", "D32_SFLOAT"}, {"usage", Json::array({"DEPTH_STENCIL_ATTACHMENT"})}}})},
                {"rendering_passes", Json::array({Json{{"name", "main"}, {"passes", Json::array({pass})}}})}};
            return TestSupport::renderingPassDefinitionsOf(config, names).at(0).passes.at(0);
        };
        TestSupport::requireNamedRefusal([&] { (void)definitions(formerly_accepted); },
            "render_graph.pass_shape@1", {{"graph", "main"}, {"node", "forward_transparent"},
                {"violation", "current_frame_color_feedback"}, {"target", "lit_color"}});
        // The other named material read follows the same policy.
        formerly_accepted.erase("screen_inputs");
        formerly_accepted["material_resources"] = {{"current_color", "lit_color"}};
        TestSupport::requireNamedRefusal([&] { (void)definitions(formerly_accepted); },
            "render_graph.pass_shape@1", {{"node", "forward_transparent"},
                {"violation", "current_frame_color_feedback"}, {"target", "lit_color"}});

        const auto history_neighbor = nlohmann::json::parse(R"json({
          "name": "forward_transparent_history",
          "type": "material",
          "material_contract": "forward_transparent_v1",
          "material_resources": {
            "previous_color": {"resource": "lit_color@history", "access": "sampled", "subresource": {"mip": 0}},
            "previous_color_again": {"resource": "lit_color@history", "access": "sampled", "subresource": {"mip": 0}}
          },
          "output": {
            "color": ["lit_color"],
            "depth": "offscreen_depth"
          }
        })json");
        const auto resolved = definitions(history_neighbor);
        REQUIRE(
            resolved.input_targets ==
            std::vector<GlobalRenderTargetId>{
                GlobalRenderTargetId{10}});
        REQUIRE(
            resolved.input_target_history ==
            std::vector<bool>{true});
        REQUIRE(
            resolved.output_color.front().target ==
            resolved.input_targets.front());
        REQUIRE(
            resolved.materialInfo()
                .material_resources.front().history);
        // Both ports survive, but the image is read once. History is part
        // of the binding and of the common input list, not just acceptance.
        REQUIRE(resolved.materialInfo().material_resources.size() == 2);
        for (const auto &binding : resolved.materialInfo().material_resources) {
            CHECK(binding.target == GlobalRenderTargetId{10});
            CHECK(binding.history);
        }
    }

    SECTION("regression 2 reserves swapchain for every authored resource kind") {
        const auto formerly_accepted = nlohmann::json::parse(R"json({
          "buffers": [{"name": "swapchain", "size": 16}],
          "rendering_passes": [{
            "name": "main",
            "passes": [{
              "name": "read_reserved_buffer",
              "type": "fullscreen",
              "input": ["swapchain"],
              "output": {"color": "output_color", "depth": null},
              "shader": {
                "vertex": "engine://fullscreen",
                "fragment": "engine://copy"
              }
            }]
          }]
        })json");
        const auto declaration_rejected = rejectionMessage([&] {
            (void)Pelican::test_support::bufferDefinitionsOf(
                formerly_accepted);
        });
        REQUIRE_FALSE(declaration_rejected.empty());
        REQUIRE_THAT(
            declaration_rejected,
            Catch::Matchers::ContainsSubstring(
                R"(render_graph.buffer_invalid@1 {"field":"name","reason":"reserved","resource":"swapchain"})"));

        const auto render_target_rejected =
            rejectionMessage([&] {
                (void)Pelican::test_support::renderTargetDefinitionsOf(
                    nlohmann::json::parse(R"json({
                      "render_targets": [{"name": "swapchain"}]
                    })json"));
            });
        REQUIRE_FALSE(render_target_rejected.empty());
        REQUIRE_THAT(
            render_target_rejected,
            Catch::Matchers::ContainsSubstring(
                R"(render_graph.image_invalid@1 {"field":"name","reason":"reserved","resource":"swapchain"})"));

        const auto &invalid_pass =
            formerly_accepted.at("rendering_passes")
                .at(0).at("passes").at(0);
        const auto input_rejected = rejectionMessage([&] {
            (void)TestSupport::renderingPassDefinitionsOf(nlohmann::json{
                {"passes", nlohmann::json::array({invalid_pass})}}, names);
        });
        REQUIRE_FALSE(input_rejected.empty());
        REQUIRE_THAT(
            input_rejected,
            Catch::Matchers::ContainsSubstring(
                R"("violation":"swapchain_input")"));

        const auto valid_neighbor = nlohmann::json::parse(R"json({
          "buffers": [{"name": "light_buffer", "size": 16}],
          "rendering_passes": [{
            "name": "main",
            "passes": [{
              "name": "read_regular_buffer",
              "type": "fullscreen",
              "input": ["light_buffer"],
              "output": {"color": "output_color", "depth": null},
              "shader": {
                "vertex": "engine://fullscreen",
                "fragment": "engine://copy"
              }
            }]
          }]
        })json");
        const auto buffers =
            Pelican::test_support::bufferDefinitionsOf(valid_neighbor);
        REQUIRE(buffers.size() == 1);
        REQUIRE(buffers.front().name == "light_buffer");
        auto complete = valid_neighbor;
        complete["render_targets"] = nlohmann::json::array({
            {{"name", "output_color"}, {"extent_scale", 1.0}, {"format", "R8G8B8A8_UNORM"},
             {"usage", nlohmann::json::array({"COLOR_ATTACHMENT"})}}});
        const auto resolved = TestSupport::renderingPassDefinitionsOf(complete, names).at(0).passes.at(0);
        REQUIRE(
            resolved.input_buffers ==
            std::vector<std::string>{"light_buffer"});
        REQUIRE(resolved.input_targets.empty());
        REQUIRE(
            resolved.output_color.front().target ==
            GlobalRenderTargetId{13});
    }

    SECTION("regression 3 rejects one malformed fullscreen in both parser paths") {
        const auto formerly_accepted = nlohmann::json::parse(R"json({
          "name": "wp326_bad_fullscreen_graph",
          "render_targets": [
            {"name": "source_color"},
            {"name": "output_color"},
            {"name": "second_color"},
            {"name": "offscreen_depth"}
          ],
          "passes": [{
            "name": "bad_fullscreen",
            "type": "fullscreen",
            "input": ["source_color", "source_color"],
            "output": {
              "color": ["output_color", "second_color"],
              "depth": "offscreen_depth"
            },
            "shader": {
              "vertex": "engine://fullscreen",
              "fragment": "engine://copy"
            }
          }]
        })json");
        const auto frameplanner_rejected = rejectionMessage([&] {
            (void)TestSupport::frameGraphOf(
                formerly_accepted);
        });
        const auto parser_rejected = rejectionMessage([&] {
            (void)TestSupport::renderingPassDefinitionsOf(formerly_accepted, names);
        });
        REQUIRE_FALSE(frameplanner_rejected.empty());
        REQUIRE_FALSE(parser_rejected.empty());
        // Planning and execution conversion enter the same named shape rule.
        REQUIRE_THAT(
            frameplanner_rejected,
            Catch::Matchers::ContainsSubstring("render_graph.pass_shape@1") &&
                Catch::Matchers::ContainsSubstring(R"("violation":"color_output_count")") &&
                Catch::Matchers::ContainsSubstring(R"("node":"bad_fullscreen")"));
        REQUIRE_THAT(
            parser_rejected,
            Catch::Matchers::ContainsSubstring(
                R"("violation":"color_output_count")") &&
                Catch::Matchers::ContainsSubstring("bad_fullscreen"));

        const auto valid_neighbor = nlohmann::json::parse(R"json({
          "name": "wp326_valid_fullscreen_graph",
          "render_targets": [
            {"name": "source_color"},
            {"name": "output_color"}
          ],
          "passes": [{
            "name": "valid_fullscreen",
            "type": "fullscreen",
            "input": ["source_color"],
            "output": {"color": "output_color", "depth": null},
            "shader": {
              "vertex": "engine://fullscreen",
              "fragment": "engine://copy"
            }
          }]
        })json");
        const auto graph =
            TestSupport::frameGraphOf(valid_neighbor);
        REQUIRE(graph.nodes.size() == 1);
        REQUIRE(
            graph.nodes.front().reads ==
            std::vector<std::string>{"source_color"});
        REQUIRE(
            graph.nodes.front().writes ==
            std::vector<std::string>{"output_color"});

        const auto resolved = TestSupport::renderingPassDefinitionsOf(valid_neighbor, names).at(0).passes.at(0);
        REQUIRE(
            resolved.input_targets ==
            std::vector<GlobalRenderTargetId>{
                GlobalRenderTargetId{12}});
        REQUIRE(
            resolved.output_color.size() == 1);
        REQUIRE(
            resolved.output_color.front().target ==
            GlobalRenderTargetId{13});
        REQUIRE_FALSE(
            isConcreteRenderTarget(resolved.output_depth));
    }
}

TEST_CASE(
    "pass definition JSON parser builds an arbitrary-MRT generic raster pass",
    "[renderingpass][raster][wp238b]") {
    const Json images = Json::array({fixtureImage("gbuffer_a"), fixtureImage("gbuffer_b"), fixtureImage("scene_depth", true)});
    const Json buffers = Json::array();
    const auto name_resolver =
        RenderTargetNameResolver{
            [](const std::string &name) {
                if (name == "gbuffer_a")
                    return GlobalRenderTargetId{10};
                if (name == "gbuffer_b")
                    return GlobalRenderTargetId{11};
                if (name == "scene_depth")
                    return GlobalRenderTargetId{12};
                return noRenderTargetId();
            }};

    const auto encoded =
        nlohmann::json::parse(R"json({
          "name": "custom_gbuffer",
          "type": "raster",
          "resolution_domain": "scene",
          "output": {
            "color": ["gbuffer_a", "gbuffer_b"],
            "depth": "scene_depth"
          },
          "draw": {
            "implementation": "project.gbuffer_draw@1",
            "vertex_count": 3
          },
          "raster_state": {
            "depth_test": true,
            "depth_write": true,
            "color_attachments": [
              {"blend": "opaque", "write_mask": "rgba"},
              {"blend": "opaque", "write_mask": "rg"}
            ]
          },
          "shader": {
            "vertex": "shaders/custom_gbuffer_vertex",
            "fragment": "shaders/custom_gbuffer_fragment"
          }
        })json");

    const auto pass =
        passInGraph(encoded, images, buffers, name_resolver);
    CHECK(pass.isGenericRaster());
    CHECK(
        pass.resolution_domain ==
        RenderResolutionDomain::scene);
    REQUIRE(pass.output_color.size() == 2);
    CHECK(
        pass.output_depth ==
        GlobalRenderTargetId{12});
    CHECK(
        pass.genericRasterInfo()
            .contract.state.depth.write);
    CHECK(
        pass.genericRasterInfo()
            .contract.state.color_attachments[1]
            .write_mask ==
        (materialOutputWriteRed |
         materialOutputWriteGreen));
}

TEST_CASE(
    "pass definition JSON parser keeps implementation selection typed and fullscreen-only",
    "[renderingpass][render-pass][wp200]") {
    const Json images = Json::array({fixtureImage("half_color")});
    const Json buffers = Json::array();
    const auto name_resolver =
        RenderTargetNameResolver{
            [](const std::string &name) {
                return name == "half_color"
                           ? GlobalRenderTargetId{3}
                           : noRenderTargetId();
            }};

    const nlohmann::json material_json{
        {"name", "material"},
        {"type", "material"},
        {"implementation",
         {{"provider", "fixture.fullscreen"}}},
        {"output",
         {{"color", "half_color"},
          {"depth", nullptr}}},
    };
    TestSupport::requireNamedRefusal([&] { (void)passInGraph(material_json, images, buffers, name_resolver); },
        "render_graph.pass_field_ownership@1", {{"graph", "main"}, {"node", "material"}});

    auto malformed = nlohmann::json{
        {"name", "fullscreen"},
        {"type", "fullscreen"},
        {"implementation",
         {{"provider", "fixture.fullscreen"},
          {"fallback", "builtin.fullscreen_v1"}}},
        {"output",
         {{"color", "half_color"},
          {"depth", nullptr}}},
        {"shader",
         {{"vertex", "fullscreen"},
          {"fragment", "composite"}}},
    };
    TestSupport::requireNamedRefusal([&] { (void)passInGraph(malformed, images, buffers, name_resolver); },
        "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "fullscreen"}, {"field", "implementation.fallback"}, {"reason", "unknown_field"}});

    malformed["implementation"] =
        {{"provider", ""}};
    TestSupport::requireNamedRefusal([&] { (void)passInGraph(malformed, images, buffers, name_resolver); },
        "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "fullscreen"}, {"field", "implementation.provider"}, {"reason", "empty_or_too_long"}});
    malformed["implementation"] = {
        {"provider",
         std::string(
             RenderPass::maximumProviderNameBytesV1 + 1,
             'x')}};
    TestSupport::requireNamedRefusal([&] { (void)passInGraph(malformed, images, buffers, name_resolver); },
        "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "fullscreen"}, {"field", "implementation.provider"}, {"reason", "empty_or_too_long"}});
}

TEST_CASE("fullscreen pass JSON parser accepts shader stem references", "[renderingpass]") {
    const nlohmann::json pass_json{
        {"shader", {{"vertex", "shaders/fullscreen"}, {"fragment", "engine://bloom_blur_h"}}},
    };

    const auto info = fullscreenInfoOf(pass_json, "stem_pass");

    REQUIRE(info.vert_shader.ref == "shaders/fullscreen");
    REQUIRE(info.vert_shader.stage == ShaderStage::vertex);
    REQUIRE(info.vert_shader.kind == ShaderReferenceKind::stem);
    REQUIRE_FALSE(info.vert_shader.backend_specific);
    REQUIRE(info.frag_shader.ref == "engine://bloom_blur_h");
    REQUIRE(info.frag_shader.stage == ShaderStage::fragment);
    REQUIRE(info.frag_shader.kind == ShaderReferenceKind::stem);
}

TEST_CASE("pass definition JSON parser rejects invalid pass object", "[renderingpass]") {
    TestSupport::requireNamedRefusal([&] {
        (void)TestSupport::renderingPassDefinitionsOf(Json{{"rendering_passes", Json::array({
            Json{{"name", "main"}, {"passes", Json::array({Json::array()})}}})}});
    }, "render_graph.not_object@1", {{"graph", "main"}, {"field", "passes"}, {"index", 0}});
}

TEST_CASE("pass sequence JSON parser resolves input target references", "[renderingpass]") {
    const Json images = Json::array({fixtureImage("source_color"), fixtureImage("final_color")});
    const auto name_resolver = RenderTargetNameResolver{[](const std::string &name) {
        if (name == "source_color") {
            return GlobalRenderTargetId{3};
        }
        if (name == "final_color") {
            return GlobalRenderTargetId{4};
        }
        return noRenderTargetId();
    }};


    const nlohmann::json pass_set_json{
        {"passes",
         nlohmann::json::array({
             {{"name", "source"}, {"type", "fullscreen"},
              {"output", {{"color", "source_color"}, {"depth", nullptr}}},
              {"shader", {{"vertex", "fullscreen"}, {"fragment", "source"}}}},
             {{"name", "composite"}, {"type", "fullscreen"}, {"input", "source_color"},
              {"output", {{"color", "final_color"}, {"depth", nullptr}}},
              {"shader", {{"vertex", "fullscreen"}, {"fragment", "composite"}}}},
         })},
    };

    const auto passes = passesInGraph(pass_set_json, images, Json::array(), name_resolver);

    REQUIRE(passes.size() == 2);
    REQUIRE(passes[0].name == "source");
    REQUIRE(passes[1].input_targets.size() == 1);
    REQUIRE(passes[1].input_targets[0] == GlobalRenderTargetId{3});

    // Its producer may be a compute/ray task outside this raster-only list.
    const auto consumer_only = passesInGraph(nlohmann::json{{"passes", nlohmann::json::array({pass_set_json["passes"][1]})}}, images, Json::array(), name_resolver);
    REQUIRE(consumer_only.size() == 1);
    REQUIRE(consumer_only[0].input_targets[0] == GlobalRenderTargetId{3});
}

TEST_CASE("pass sequence JSON parser allows depth output as later input", "[renderingpass]") {
    const Json images = Json::array({fixtureImage("shadow_map", true), fixtureImage("lit_color")});
    const auto name_resolver = RenderTargetNameResolver{[](const std::string &name) {
        if (name == "shadow_map") {
            return GlobalRenderTargetId{3};
        }
        if (name == "lit_color") {
            return GlobalRenderTargetId{4};
        }
        return noRenderTargetId();
    }};


    const nlohmann::json pass_set_json{
        {"passes",
         nlohmann::json::array({
             {{"name", "shadow_depth"}, {"type", "shadow_depth"},
              {"output", {{"color", nullptr}, {"depth", "shadow_map"}}}},
             {{"name", "lighting_pass"}, {"type", "fullscreen"}, {"input", "shadow_map"},
              {"output", {{"color", "lit_color"}, {"depth", nullptr}}},
              {"shader", {{"vertex", "fullscreen"}, {"fragment", "lighting"}}}},
         })},
    };

    const auto passes = passesInGraph(pass_set_json, images, Json::array(), name_resolver);

    REQUIRE(passes.size() == 2);
    REQUIRE(passes[0].isShadowDepth());
    REQUIRE(passes[1].input_targets == std::vector<GlobalRenderTargetId>{GlobalRenderTargetId{3}});
}

TEST_CASE("pass sequence JSON parser rejects duplicate pass names", "[renderingpass]") {
    const Json images = Json::array();
    const auto name_resolver = RenderTargetNameResolver{[](const std::string &) { return noRenderTargetId(); }};


    const nlohmann::json pass_set_json{
        {"passes",
         nlohmann::json::array({
             {{"name", "same"}, {"type", "fullscreen"}, {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
              {"shader", {{"vertex", "fullscreen"}, {"fragment", "one"}}}},
             {{"name", "same"}, {"type", "fullscreen"}, {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
              {"shader", {{"vertex", "fullscreen"}, {"fragment", "two"}}}},
         })},
    };

    TestSupport::requireNamedRefusal([&] { (void)passesInGraph(pass_set_json, images, Json::array(), name_resolver); },
        "render_graph.duplicate_node@1", {{"graph", "main"}, {"node", "same"}});
}

TEST_CASE("pass sequence JSON parser rejects malformed pass list", "[renderingpass]") {
    const Json images = Json::array();
    const auto name_resolver = RenderTargetNameResolver{[](const std::string &) { return noRenderTargetId(); }};


    for (const auto &graph : {Json::object(), Json{{"passes", 1}}}) {
        TestSupport::requireNamedRefusal([&] { (void)passesInGraph(graph, images, Json::array(), name_resolver); },
            "render_graph.section_invalid@1", {{"graph", "main"}, {"field", "passes"}});
    }
    CHECK(passesInGraph(Json{{"passes", Json::array()}}, images, Json::array(), name_resolver).empty());
}

TEST_CASE("pass attachment options parser applies explicit color attachment options", "[renderingpass]") {

    const nlohmann::json pass_json{
        {"clear_color", nlohmann::json::array({0.25f, 0.5f, 0.75f, 1.0f})},
        {"color_load_op", "Load"},
        {"color_store_op", "DontCare"},
        {"depth_load_op", "Clear"},
        {"depth_store_op", "Store"},
    };

    const auto pass_def = materialPassOf(pass_json, "bloom_composite");

    REQUIRE(pass_def.clear_color[0] == 0.25);
    REQUIRE(pass_def.clear_color[1] == 0.5);
    REQUIRE(pass_def.clear_color[2] == 0.75);
    REQUIRE(pass_def.clear_color[3] == 1.0);
    REQUIRE(pass_def.colorAttachmentOperations(0).load_op == AttachmentLoadOp::load);
    REQUIRE(pass_def.colorAttachmentOperations(0).store_op == AttachmentStoreOp::dont_care);
    REQUIRE(pass_def.depthAttachmentOperations().load_op == AttachmentLoadOp::clear);
    REQUIRE(pass_def.depthAttachmentOperations().store_op == AttachmentStoreOp::store);
}

TEST_CASE(
    "pass attachment options apply sparse per-target clear colors",
    "[renderingpass][typed-clear][wp218]") {
    using Json = nlohmann::json;
    const auto definition = [](Json fields) {
        fields["name"] = "extended_geometry";
        fields["type"] = "material";
        return TestSupport::renderingPassDefinitionsOf(Json{
            {"render_targets", Json::array({
                Json{{"name", "albedo"}, {"extent_scale", 1.0}, {"format", "R8G8B8A8_UNORM"}, {"usage", Json::array({"COLOR_ATTACHMENT"})}},
                Json{{"name", "object_id"}, {"extent_scale", 1.0}, {"format", "R32_UINT"}, {"usage", Json::array({"COLOR_ATTACHMENT"})}},
                Json{{"name", "depth"}, {"extent_scale", 1.0}, {"format", "D32_SFLOAT"}, {"usage", Json::array({"DEPTH_STENCIL_ATTACHMENT"})}}})},
            {"rendering_passes", Json::array({Json{{"name", "main"}, {"passes", Json::array({std::move(fields)})}}})}}).at(0).passes.at(0);
    };
    const nlohmann::json pass_json{
        {"output",
         {{"color",
           nlohmann::json::array(
               {"albedo",
                {{"target", "object_id"},
                 {"load_op", "Load"}}})},
          {"depth", "depth"}}},
        {"clear_color", {0.0, 0.0, 0.0, 1.0}},
        {"clear_colors",
         {{"object_id",
           {4294967295.0, 0.0, 0.0, 0.0}}}},
    };

    const auto pass_def = definition(pass_json);
    REQUIRE(
        pass_def.logicalColorClearValue(0) ==
        std::array<double, 4>{
            0.0, 0.0, 0.0, 1.0});
    REQUIRE(
        pass_def.logicalColorClearValue(1) ==
        std::array<double, 4>{
            4294967295.0, 0.0, 0.0, 0.0});

    auto invalid = pass_json;
    invalid["clear_colors"] = {
        {"not_an_output", {0, 0, 0, 0}}};
    TestSupport::requireNamedRefusal([&] { (void)definition(invalid); },
        "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "extended_geometry"},
            {"field", "clear_colors"}, {"reason", "not_a_color_output"}, {"resource", "not_an_output"}});
}

TEST_CASE(
    "WP355a pass attachment operations require explicit load and store values",
    "[renderingpass][attachment-operations][wp355a][construction]") {
    STATIC_REQUIRE_FALSE(
        std::is_default_constructible_v<
            PassAttachmentOperations>);
    STATIC_REQUIRE(
        std::is_constructible_v<
            PassAttachmentOperations,
            AttachmentLoadOp,
            AttachmentStoreOp>);
    const PassAttachmentOperations operations{
        AttachmentLoadOp::clear,
        AttachmentStoreOp::store};
    CHECK(operations.load_op == AttachmentLoadOp::clear);
    CHECK(operations.store_op == AttachmentStoreOp::store);
}

TEST_CASE("pass attachment options parser preserves defaults when fields are omitted", "[renderingpass]") {
    const auto pass_def = materialPassOf(nlohmann::json::object(), "geometry");

    REQUIRE(pass_def.clear_color[0] == 0.0);
    REQUIRE(pass_def.clear_color[1] == 0.0);
    REQUIRE(pass_def.clear_color[2] == 0.0);
    REQUIRE(pass_def.clear_color[3] == 1.0);
    REQUIRE(pass_def.color_load_op == AttachmentLoadOp::clear);
    REQUIRE(pass_def.color_store_op == AttachmentStoreOp::store);
    REQUIRE(pass_def.depth_load_op == AttachmentLoadOp::clear);
    REQUIRE(pass_def.depth_store_op == AttachmentStoreOp::dont_care);
    REQUIRE((pass_def.colorAttachmentOperations(0) ==
             PassAttachmentOperations{AttachmentLoadOp::clear,
                                      AttachmentStoreOp::store}));
    REQUIRE((pass_def.depthAttachmentOperations() ==
             PassAttachmentOperations{AttachmentLoadOp::clear,
                                      AttachmentStoreOp::dont_care}));

    const auto ui_color = defaultPassAttachmentOperations(
        RenderPassType::ui, AttachmentAspect::color);
    REQUIRE(ui_color.load_op == AttachmentLoadOp::load);
    REQUIRE(ui_color.store_op == AttachmentStoreOp::store);
}

TEST_CASE("material pass info parser applies explicit material range", "[renderingpass]") {

    const nlohmann::json pass_json{
        {"material_range", {{"start", 3}, {"count", 7}}},
    };

    const auto pass_def = materialPassOf(pass_json, "geometry");

    REQUIRE(pass_def.materialInfo().material_start == 3);
    REQUIRE(pass_def.materialInfo().material_count == 7);
}

TEST_CASE("material pass info parser compiles stable include and exclude tags",
          "[renderingpass][draw-tag][wp206a]") {

    const auto pass_def = materialPassOf(
        nlohmann::json{
            {"material_filter",
             {{"include",
               {"outline", "character"}},
              {"exclude", {"hidden"}}}},
        }, "outline");

    REQUIRE(
        pass_def.materialInfo()
            .material_filter.has_value());
    const auto &filter =
        *pass_def.materialInfo().material_filter;
    REQUIRE(filter.include ==
            std::vector<std::string>{
                "character", "outline"});
    REQUIRE(filter.exclude ==
            std::vector<std::string>{"hidden"});
    REQUIRE(
        filter.id ==
        materialDrawTagFilterId(
            filter.include, filter.exclude));

    auto invalid = nlohmann::json{
        {"material_filter",
         {{"include", {""}}}},
    };
    TestSupport::requireNamedRefusal([&] { (void)materialPassOf(invalid, "outline"); },
        "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "outline"}, {"field", "material_filter"}, {"reason", "invalid"}});
    REQUIRE_THROWS_WITH(materialPassOf(invalid, "outline"), Catch::Matchers::ContainsSubstring("tag is empty or too long"));

    invalid = {
        {"material_filter",
         {{"include", {"outline", "outline"}}}},
    };
    TestSupport::requireNamedRefusal([&] { (void)materialPassOf(invalid, "outline"); },
        "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "outline"}, {"field", "material_filter"}, {"reason", "invalid"}});
    REQUIRE_THROWS_WITH(materialPassOf(invalid, "outline"), Catch::Matchers::ContainsSubstring("duplicate tag: outline"));

    invalid = {
        {"material_filter",
         {{"include", {"outline"}},
          {"exclude", {"outline"}}}},
    };
    TestSupport::requireNamedRefusal([&] { (void)materialPassOf(invalid, "outline"); },
        "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "outline"}, {"field", "material_filter"}, {"reason", "invalid"}});
    REQUIRE_THROWS_WITH(materialPassOf(invalid, "outline"), Catch::Matchers::ContainsSubstring("includes and excludes the same tag"));

    invalid = {
        {"material_filter",
         {{"any", {"outline"}}}},
    };
    TestSupport::requireNamedRefusal([&] { (void)materialPassOf(invalid, "outline"); },
        "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "outline"}, {"field", "material_filter"}, {"reason", "unknown_field"}, {"key", "any"}});
}

TEST_CASE("material pass screen inputs resolve named typed targets and reject mismatches",
          "[renderingpass][material-screen-input][rpe6b1]") {
    const Json images = Json::array({fixtureImage("opaque_color"), fixtureImage("opaque_depth", true)});
    const Json buffer_declarations = Json::array({Json{{"name", "deformed_positions"}, {"size", 64}}});
    const RenderTargetNameResolver names{[](const std::string &name) {
        if (name == "opaque_color") return GlobalRenderTargetId{10};
        if (name == "opaque_depth") return GlobalRenderTargetId{11};
        return noRenderTargetId();
    }};


    PassDefinition pass;
    pass.name = "forward_transparent";
    pass.pass_info = MaterialPassInfo{};
    pass = namedMaterialOf(nlohmann::json{{"screen_inputs",
                        {{"opaque_color", "opaque_color"},
                         {"opaque_depth", "opaque_depth"},
                         {"scene_depth", "opaque_depth"},
                         {"linear_view_depth", "opaque_depth"}}}}, "forward_transparent", images, buffer_declarations, names);
    REQUIRE(pass.materialInfo().screen_inputs.size() == 4);
    REQUIRE(pass.input_targets.size() == 2);
    REQUIRE(std::find(pass.input_targets.begin(), pass.input_targets.end(),
                      GlobalRenderTargetId{10}) != pass.input_targets.end());
    REQUIRE(std::find(pass.input_targets.begin(), pass.input_targets.end(),
                      GlobalRenderTargetId{11}) != pass.input_targets.end());
    const auto linear = std::find_if(
        pass.materialInfo().screen_inputs.begin(),
        pass.materialInfo().screen_inputs.end(), [](const auto &input) {
            return input.contract.name == "linear_view_depth";
        });
    REQUIRE(linear != pass.materialInfo().screen_inputs.end());
    REQUIRE(linear->contract.sampled_type ==
            linearViewDepthV1(makeBuiltinLogicalTypeRegistry()));
    REQUIRE(linear->contract.footprint.kind ==
            LogicalReadFootprintKind::same_pixel);
    REQUIRE(linear->contract.conversion ==
            std::optional<std::string>{"pelican.render.depth_linearize@1"});

    TestSupport::requireNamedRefusal([&] { (void)namedMaterialOf(nlohmann::json{{"screen_inputs",
                            {{"opaque_color", "opaque_depth"}}}}, "bad_forward", images, buffer_declarations, names); },
        "render_graph.pass_execution_invalid@1", {{"graph", "main"}, {"node", "bad_forward"}, {"field", "screen_input"}, {"reason", "invalid_contract"}, {"port", "opaque_color"}});

    TestSupport::requireNamedRefusal([&] { (void)namedMaterialOf(nlohmann::json{{"screen_inputs",
                            {{"history_magic", "opaque_color"}}}}, "unknown_forward", images, buffer_declarations, names); },
        "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "unknown_forward"}, {"field", "screen_inputs"}, {"reason", "unknown_contract"}});
}

TEST_CASE("material pass surface resources resolve the feature-owned directional shadow contract",
          "[renderingpass][material-surface-resource][wp205]") {
    const Json images = Json::array({fixtureImage("shadow_map", true), fixtureImage("scene_color")});
    const Json buffer_declarations = Json::array({Json{{"name", "deformed_positions"}, {"size", 64}}});
    const RenderTargetNameResolver names{[](const std::string &name) {
        if (name == "shadow_map") return GlobalRenderTargetId{20};
        if (name == "scene_color") return GlobalRenderTargetId{21};
        return noRenderTargetId();
    }};


    PassDefinition pass;
    pass.name = "forward_opaque";
    pass.pass_info = MaterialPassInfo{};
    pass = namedMaterialOf(nlohmann::json{
            {"surface_resources",
             {{"directional_shadow", "shadow_map"}}}}, "forward_opaque", images, buffer_declarations, names);

    REQUIRE(pass.materialInfo().surface_resources.size() == 1);
    const auto &binding =
        pass.materialInfo().surface_resources.front();
    REQUIRE(binding.target == GlobalRenderTargetId{20});
    REQUIRE(binding.contract.name == "directional_shadow");
    REQUIRE(binding.contract.source_type ==
            deviceDepthV1(makeBuiltinLogicalTypeRegistry()));
    REQUIRE(binding.contract.sampled_type ==
            deviceDepthV1(makeBuiltinLogicalTypeRegistry()));
    REQUIRE(binding.contract.footprint.kind ==
            LogicalReadFootprintKind::arbitrary);
    REQUIRE(binding.contract.sampling ==
            MaterialPassInputSampling::nearest_clamp_to_edge);
    REQUIRE(binding.contract.view_policy ==
            MaterialPassInputViewPolicy::
                family_array);
    REQUIRE(binding.contract.fallback ==
            MaterialPassInputFallback::fully_lit);
    REQUIRE(binding.contract.relation);
    REQUIRE(binding.contract.relation->light_index == 0);
    REQUIRE(binding.contract.relation->transform ==
            "pelican.light.shadow_view_projection@1");
    REQUIRE(pass.input_targets ==
            std::vector<GlobalRenderTargetId>{
                GlobalRenderTargetId{20}});

    TestSupport::requireNamedRefusal([&] { (void)namedMaterialOf(nlohmann::json{
                {"surface_resources",
                 {{"directional_shadow", "scene_color"}}}}, "bad_forward", images, buffer_declarations, names); },
        "render_graph.pass_execution_invalid@1", {{"graph", "main"}, {"node", "bad_forward"}, {"field", "surface_resource"}, {"reason", "invalid_contract"}, {"port", "directional_shadow"}});

    TestSupport::requireNamedRefusal([&] { (void)namedMaterialOf(nlohmann::json{
                {"surface_resources",
                 {{"opaque_depth", "shadow_map"}}}}, "bad_forward", images, buffer_declarations, names); },
        "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "bad_forward"}, {"field", "surface_resource"}, {"reason", "not_feature_owned"}, {"port", "opaque_depth"}});
}

TEST_CASE(
    "material pass resource ports resolve named image and buffer reads",
    "[renderingpass][material-resource][wp207b]") {
    const Json images = Json::array({fixtureImage("simulation_color"), [] { auto image = fixtureImage("history_color"); image["history"] = true; return image; }()});
    const Json buffer_declarations = Json::array({Json{{"name", "deformed_positions"}, {"size", 64}}});
    const RenderTargetNameResolver names{
        [](const std::string &name) {
            if (name == "simulation_color") {
                return GlobalRenderTargetId{30};
            }
            if (name == "history_color") {
                return GlobalRenderTargetId{31};
            }
            return noRenderTargetId();
        }};

    PassDefinition pass;
    pass.name = "geometry";
    pass.pass_info = MaterialPassInfo{};
    pass = namedMaterialOf(nlohmann::json{
            {"material_resources",
             {
                 {"displacement",
                  {
                      {"resource",
                       "deformed_positions"},
                      {"access", "storage"},
                  }},
                 {"simulation_color",
                  {
                      {"resource",
                       "simulation_color"},
                      {"access", "sampled"},
                      {"view", "per_view"},
                      {"sampling",
                       {
                           {"filter", "nearest"},
                           {"address",
                            "clamp_to_edge"},
                       }},
                      {"footprint",
                       {
                           {"kind",
                            "neighborhood"},
                           {"radius", 2u},
                       }},
                  }},
                 {"previous_color",
                  {
                      {"resource",
                       "history_color@history"},
                      {"access", "sampled"},
                  }},
             }}}, "geometry", images, buffer_declarations, names);

    REQUIRE(
        pass.materialInfo().material_resources.size() ==
        3);
    REQUIRE(
        pass.input_buffers ==
        std::vector<std::string>{
            "deformed_positions"});
    REQUIRE(pass.input_targets.size() == 2);
    REQUIRE(
        pass.input_target_history.size() == 2);
    const auto current_target = std::find(
        pass.input_targets.begin(),
        pass.input_targets.end(),
        GlobalRenderTargetId{30});
    const auto history_target = std::find(
        pass.input_targets.begin(),
        pass.input_targets.end(),
        GlobalRenderTargetId{31});
    REQUIRE(current_target != pass.input_targets.end());
    REQUIRE(history_target != pass.input_targets.end());
    REQUIRE_FALSE(pass.input_target_history.at(
        static_cast<std::size_t>(std::distance(
            pass.input_targets.begin(),
            current_target))));
    REQUIRE(pass.input_target_history.at(
        static_cast<std::size_t>(std::distance(
            pass.input_targets.begin(),
            history_target))));

    const auto find_binding =
        [&](std::string_view name)
        -> const MaterialPassResourceBinding & {
        const auto found = std::find_if(
            pass.materialInfo()
                .material_resources.begin(),
            pass.materialInfo()
                .material_resources.end(),
            [&](const auto &binding) {
                return binding.port.name == name;
            });
        REQUIRE(
            found !=
            pass.materialInfo()
                .material_resources.end());
        return *found;
    };
    const auto &displacement =
        find_binding("displacement");
    REQUIRE(displacement.isBuffer());
    REQUIRE_FALSE(displacement.isImage());
    REQUIRE(
        displacement.port.access ==
        ShaderResourcePortAccess::storage);

    const auto &simulation =
        find_binding("simulation_color");
    REQUIRE(simulation.isImage());
    REQUIRE(
        simulation.port.view ==
        ShaderResourcePortView::per_view);
    REQUIRE(
        simulation.port.sampling.filter ==
        ShaderResourcePortFilter::nearest);
    REQUIRE(
        simulation.port.sampling.address_mode ==
        ShaderResourcePortAddressMode::
            clamp_to_edge);
    REQUIRE(
        simulation.footprint.kind ==
        LogicalReadFootprintKind::neighborhood);
    REQUIRE(simulation.footprint.radius == 2u);

    const auto &history =
        find_binding("previous_color");
    REQUIRE(history.isImage());
    REQUIRE(history.history);
    REQUIRE(
        history.footprint.kind ==
        LogicalReadFootprintKind::temporal);

    PassDefinition subresource;
    subresource.name = "subresource";
    subresource.pass_info = MaterialPassInfo{};
    subresource = namedMaterialOf(nlohmann::json{
            {"material_resources",
             {{"pyramid",
               {
                   {"resource",
                    "simulation_color"},
                   {"access", "sampled"},
                   {"subresource",
                    {
                        {"mip", 1},
                        {"mip_count",
                         "remaining"},
                    }},
               }}}}}, "subresource", images, buffer_declarations, names);
    REQUIRE(
        subresource.materialInfo()
            .material_resources.size() == 1);
    const auto selected =
        subresource.materialInfo()
            .material_resources.front()
            .port.subresource;
    REQUIRE(selected.has_value());
    REQUIRE(selected->base_mip_level == 1);
    REQUIRE(
        selected->mip_count_mode ==
        ImageSubresourceMipCountMode::
            remaining);
    const auto resolved_selection =
        resolveImageSubresourceRange(
            *selected, 5, 1);
    REQUIRE(
        resolved_selection.base_mip_level ==
        1);
    REQUIRE(
        resolved_selection.level_count == 4);
    REQUIRE(
        resolved_selection.mip_count_mode ==
        ImageSubresourceMipCountMode::fixed);

    auto local_subresource =
        nlohmann::json{
            {"material_resources",
             {{"pyramid",
               {
                   {"resource",
                    "simulation_color"},
                   {"access", "sampled"},
                   {"subresource",
                    {{"mip", 1}}},
                   {"footprint", "same_pixel"},
               }}}}};
    TestSupport::requireNamedRefusal([&] { (void)namedMaterialOf(local_subresource, "subresource", images, buffer_declarations, names); },
        "render_graph.footprint_invalid@1", {{"graph", "main"}, {"node", "subresource"}, {"field", "material_resources"}, {"resource", "pyramid"}, {"reason", "same_pixel_subresource"}});

    TestSupport::requireNamedRefusal([&] { (void)namedMaterialOf(nlohmann::json{
                {"material_resources",
                 {{"unknown", "missing_target"}}}}, "missing", images, buffer_declarations, names); },
        "render_graph.pass_execution_invalid@1", {{"graph", "main"}, {"node", "missing"}, {"field", "material_resource"}, {"reason", "unknown_image"}, {"resource", "missing_target"}});

    TestSupport::requireNamedRefusal([&] { (void)namedMaterialOf(nlohmann::json{
                {"material_resources",
                 {{"previous",
                   "deformed_positions@history"}}}}, "buffer_history", images, buffer_declarations, names); },
        "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "buffer_history"}, {"field", "material_resources.previous"}, {"reason", "buffer_history"}});

    TestSupport::requireNamedRefusal([&] { (void)namedMaterialOf(nlohmann::json{
                {"material_resources",
                 {
                     {"current",
                      "simulation_color"},
                     {"previous",
                      "simulation_color@history"},
                 }}}, "mixed_history", images, buffer_declarations, names); },
        "render_graph.pass_shape@1", {{"graph", "main"}, {"node", "mixed_history"}, {"violation", "duplicate_input"}, {"target", "simulation_color"}});
}

TEST_CASE("material pass contracts parse and filter independently of local pass ids",
          "[renderingpass][material-routing]") {
    REQUIRE(forwardMaterialPassColorAttachmentFormat() ==
            vk::Format::eR16G16B16A16Sfloat);
    const auto pass_def = materialPassOf(nlohmann::json{{"material_contract", "forward_opaque_v1"}}, "forward_opaque");
    REQUIRE(pass_def.materialInfo().contract == MaterialPassContract::forward_opaque_v1);
    REQUIRE(materialPassShaderContract(pass_def.materialInfo().contract) ==
            MaterialShaderContract::forward_scene_color_v1);
    REQUIRE(materialPassAcceptsMaterial(
        pass_def.materialInfo().contract, pass_def.name,
        MaterialRouteClass::forward_opaque,
        MaterialShaderContract::forward_scene_color_v1));
    REQUIRE_FALSE(materialPassAcceptsMaterial(
        pass_def.materialInfo().contract, pass_def.name,
        MaterialRouteClass::forward_transparent,
        MaterialShaderContract::forward_scene_color_v1));
    REQUIRE_FALSE(materialPassAcceptsMaterial(
        pass_def.materialInfo().contract, pass_def.name,
        MaterialRouteClass::forward_opaque,
        MaterialShaderContract::forward_scene_color_v1,
        std::optional<std::string>{"hero_forward"}));
    TestSupport::requireNamedRefusal([&] {
        (void)materialPassOf(nlohmann::json{{"material_contract", "unknown_v1"}}, "forward_opaque");
    }, "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "forward_opaque"},
        {"field", "material_contract"}, {"reason", "unknown"}});

    REQUIRE(materialPassAcceptsMaterial(
        MaterialPassContract::legacy_gbuffer_v1, "geometry",
        MaterialRouteClass::deferred_geometry,
        MaterialShaderContract::legacy_gbuffer_v1));
    REQUIRE(materialPassAcceptsMaterial(
        MaterialPassContract::legacy_gbuffer_v1, "geometry",
        MaterialRouteClass::deferred_geometry,
        MaterialShaderContract::gbuffer_v1));
    REQUIRE_FALSE(materialPassAcceptsMaterial(
        MaterialPassContract::legacy_gbuffer_v1, "geometry",
        MaterialRouteClass::forward_opaque,
        MaterialShaderContract::forward_scene_color_v1));
}

TEST_CASE(
    "material output schemas remove the engine MRT count while preserving "
    "typed attachment validation",
    "[renderingpass][material-output][wp218]") {
    const nlohmann::json declaration{
        {"schema", "pelican.material_outputs"},
        {"version", 1},
        {"name", "project.extended_gbuffer"},
        {"outputs",
         {
             {{"name", "base_color"},
              {"type", "vec4"},
              {"source", "surface.base_color"}},
             {{"name", "normal"},
              {"type", "vec4"},
              {"source", "surface.normal_encoded"}},
             {{"name", "material"},
              {"type", "vec4"},
              {"source", "surface.material"}},
             {{"name", "world_position"},
              {"type", "vec4"},
              {"source", "input.world_position"}},
             {{"name", "emissive"},
              {"type", "vec4"},
              {"source", "surface.emissive"}},
             {{"name", "object_id"},
              {"type", "uint"},
              {"source", "custom"}},
             {{"name", "reactive_mask"},
              {"type", "float"},
              {"source", "custom"}},
         }},
    };

    // RC-3c2a: the attachments are the resource check's, on the IR and the
    // generation's description.
    const auto checked = [&](const nlohmann::json &outputs, std::size_t colors) {
        auto targets = nlohmann::json::array();
        auto color = nlohmann::json::array();
        for (std::size_t index = 0; index < 7; ++index) {
            targets.push_back({{"name", "gbuffer_" + std::to_string(index)},
                               {"extent_scale", 1.0},
                               {"format", index == 5 ? "R32_UINT" : "R16G16B16A16_SFLOAT"},
                               {"usage", nlohmann::json::array({"COLOR_ATTACHMENT"})}});
            if (index < colors) color.push_back("gbuffer_" + std::to_string(index));
        }
        targets.push_back({{"name", "depth"}, {"extent_scale", 1.0}, {"format", "D16_UNORM"},
                           {"usage", nlohmann::json::array({"DEPTH_STENCIL_ATTACHMENT"})}});
        const nlohmann::json config{
            {"render_targets", targets},
            {"rendering_passes",
             nlohmann::json::array({{{"name", "main"},
                                     {"passes", nlohmann::json::array({{{"name", "extended_geometry"},
                                                                        {"type", "material"},
                                                                        {"material_contract", "deferred_geometry_v1"},
                                                                        {"material_outputs", outputs},
                                                                        {"output", {{"color", color}, {"depth", "depth"}}}}})}}})}};
        const auto ir = TestSupport::renderGraphOf(config);
        validateRenderGraphResources(ir, resolveDeclaredGenerationResources(ir, {1280, 720}));
        return TestSupport::renderingPassDefinitionsOf(config).at(0).passes.at(0);
    };
    const auto pass = checked(declaration, 7);
    REQUIRE(pass.materialInfo().output_schema);
    REQUIRE(
        pass.materialInfo().output_schema->outputs.size() ==
        7);
    REQUIRE(
        materialOutputSchemaFingerprint(
            *pass.materialInfo().output_schema)
            .starts_with(
                "pelican.material_outputs@1:"
                "project.extended_gbuffer"));

    REQUIRE(
        pass.colorClearValue(5).numeric_class ==
        MaterialOutputNumericClass::unsigned_integer);
    REQUIRE(
        pass.colorClearValue(5)
            .vulkan()
            .uint32[3] == 1u);

    REQUIRE_NOTHROW(checked(declaration, 7));
    REQUIRE_THROWS_WITH(
        checked(declaration, 6),
        Catch::Matchers::ContainsSubstring(
            R"(render_graph.material_attachment_invalid@1 {"expected":7,"graph":"main","node":"extended_geometry","reason":"color_count"})"));

    auto invalid = declaration;
    invalid["outputs"][0]["type"] = "uint";
    invalid["outputs"][0]["source"] = "custom";
    REQUIRE_THROWS_WITH(
        checked(invalid, 7),
        Catch::Matchers::ContainsSubstring(R"("reason":"numeric_class","resource":"gbuffer_0")"));
}

TEST_CASE(
    "material output schema rejects duplicate names and lit data in deferred",
    "[renderingpass][material-output][wp218]") {
    nlohmann::json declaration{
        {"schema", "pelican.material_outputs"},
        {"version", 1},
        {"name", "project.invalid"},
        {"outputs",
         {
             {{"name", "same"},
              {"type", "vec4"},
              {"source", "surface.base_color"}},
             {{"name", "same"},
              {"type", "vec4"},
              {"source", "surface.emissive"}},
         }},
    };
    REQUIRE_THROWS_WITH(
        parseMaterialOutputSchema(declaration),
        Catch::Matchers::ContainsSubstring(
            "output names must be unique"));

    declaration["outputs"].erase(
        declaration["outputs"].begin() + 1);
    declaration["outputs"][0]["name"] = "lit";
    declaration["outputs"][0]["source"] =
        "lighting.scene_color";
    // RC-3c2a: the resource check, on the IR and the description.
    const nlohmann::json config{
        {"render_targets",
         nlohmann::json::array({{{"name", "color"}, {"extent_scale", 1.0}, {"format", "R16G16B16A16_SFLOAT"},
                                 {"usage", nlohmann::json::array({"COLOR_ATTACHMENT"})}},
                                {{"name", "depth"}, {"extent_scale", 1.0}, {"format", "D32_SFLOAT"},
                                 {"usage", nlohmann::json::array({"DEPTH_STENCIL_ATTACHMENT"})}}})},
        {"rendering_passes",
         nlohmann::json::array({{{"name", "main"},
                                 {"passes", nlohmann::json::array({{{"name", "deferred"},
                                                                    {"type", "material"},
                                                                    {"material_contract", "deferred_geometry_v1"},
                                                                    {"material_outputs", declaration},
                                                                    {"output", {{"color", nlohmann::json::array({"color"})},
                                                                                {"depth", "depth"}}}}})}}})}};
    const auto ir = TestSupport::renderGraphOf(config);
    REQUIRE_THROWS_WITH(
        validateRenderGraphResources(ir, resolveDeclaredGenerationResources(ir, {1, 1})),
        Catch::Matchers::ContainsSubstring(R"("reason":"scene_color_in_deferred")"));
}

TEST_CASE(
    "material output attachment state is sparse typed and schema keyed",
    "[renderingpass][material-output-state][wp219]") {
    const auto schema = parseMaterialOutputSchema(
        nlohmann::json{
            {"schema", "pelican.material_outputs"},
            {"version", 1},
            {"name", "project.weighted_oit"},
            {"outputs",
             nlohmann::json::array({
                 {{"name", "accum"},
                  {"type", "vec4"},
                  {"source", "surface.base_color"}},
                 {{"name", "revealage"},
                  {"type", "float"},
                  {"source", "custom"}},
                 {{"name", "object_id"},
                  {"type", "uint"},
                  {"source", "custom"}},
             })},
        });
    const nlohmann::json declaration{
        {"revealage",
         {
             {"blend",
              {
                  {"color",
                   {{"src", "zero"},
                    {"dst", "one_minus_src_color"},
                    {"op", "add"}}},
                  {"alpha",
                   {{"src", "zero"},
                    {"dst", "one_minus_src_alpha"},
                    {"op", "add"}}},
              }},
             {"write_mask", "r"},
         }},
        {"accum",
         {
             {"blend",
              {
                  {"color",
                   {{"src", "one"},
                    {"dst", "one"},
                    {"op", "add"}}},
                  {"alpha",
                   {{"src", "one"},
                    {"dst", "one"},
                    {"op", "add"}}},
              }},
         }},
        {"object_id",
         {
             {"blend", "opaque"},
             {"write_mask", "r"},
         }},
    };
    const auto states =
        parseMaterialOutputAttachmentStates(
            declaration, schema);
    REQUIRE(states.size() == 3);
    REQUIRE(states[0].output == "accum");
    REQUIRE(states[1].output == "revealage");
    REQUIRE(states[2].output == "object_id");
    REQUIRE(states[0].blend);
    REQUIRE(states[0].blend->enabled);
    REQUIRE(
        states[0].blend->color.destination ==
        MaterialOutputBlendFactor::one);
    REQUIRE(states[1].write_mask ==
            materialOutputWriteRed);
    REQUIRE(states[2].blend);
    REQUIRE_FALSE(states[2].blend->enabled);

    const auto canonical =
        materialOutputAttachmentStatesToJson(
            states, schema);
    REQUIRE(
        parseMaterialOutputAttachmentStates(
            canonical, schema) == states);
    REQUIRE(
        materialOutputAttachmentStatesFingerprint(
            states, schema)
            .starts_with(
                "pelican.material_output_states@1:"));

    auto invalid = declaration;
    invalid["object_id"]["blend"] = "additive";
    REQUIRE_THROWS_WITH(
        parseMaterialOutputAttachmentStates(
            invalid, schema),
        Catch::Matchers::ContainsSubstring(
            "cannot enable blending for an integer output"));

    invalid = declaration;
    invalid["accum"]["write_mask"] = "rr";
    REQUIRE_THROWS_WITH(
        parseMaterialOutputAttachmentStates(
            invalid, schema),
        Catch::Matchers::ContainsSubstring(
            "duplicate channel"));

    invalid = declaration;
    invalid["unknown"] = {
        {"write_mask", "rgba"}};
    REQUIRE_THROWS_WITH(
        parseMaterialOutputAttachmentStates(
            invalid, schema),
        Catch::Matchers::ContainsSubstring(
            "references unknown output"));

    REQUIRE_THROWS_WITH(
        materialPassOf(nlohmann::json{{"material_output_states", nlohmann::json::object()}}, "missing_schema"),
        Catch::Matchers::ContainsSubstring(
            R"("reason":"requires_material_outputs")"));
}

TEST_CASE("material pass availability distinguishes fullscreen-only graphs",
          "[renderingpass][material-routing]") {
    RenderingPassContainer container;

    PassDefinition fullscreen;
    fullscreen.name = "present";
    fullscreen.pass_info = FullscreenPassInfo{};
    container.registerCompiledRenderingPass(CompiledRenderingPass{
        "fullscreen_only", {{fullscreen, PassId{0}}}, {}});
    REQUIRE_FALSE(container.hasMaterialPasses());

    PassDefinition material;
    material.name = "geometry";
    material.pass_info = MaterialPassInfo{};
    material.rasterization_samples =
        vk::SampleCountFlagBits::e4;
    CompiledPassRenderingContract rendering;
    rendering.scope_index = 2;
    rendering.scope_id = "gbuffer_lighting";
    rendering.color_attachments = {
        GlobalRenderTargetId{4},
        GlobalRenderTargetId{5}};
    rendering.color_attachment_locations = {
        0, unusedPhysicalAttachmentMapping};
    rendering.color_attachment_input_indices = {
        unusedPhysicalAttachmentMapping,
        unusedPhysicalAttachmentMapping};
    rendering.local_read_scope = true;
    container.registerCompiledRenderingPass(CompiledRenderingPass{
        "with_material",
        {{material, PassId{0}, {}, rendering}},
        {}});
    REQUIRE(container.hasMaterialPasses());
    REQUIRE(container.supportsMaterialPass(
        MaterialRouteClass::deferred_geometry,
        MaterialShaderContract::gbuffer_v1));
    const auto bindings =
        container.materialPassRenderingBindings(
            MaterialRouteClass::deferred_geometry,
            MaterialShaderContract::gbuffer_v1);
    REQUIRE(bindings.size() == 1);
    CHECK(bindings.front().pass_name == "geometry");
    CHECK(bindings.front().rasterization_samples ==
          vk::SampleCountFlagBits::e4);
    CHECK(bindings.front().rendering == rendering);
    CHECK(container.materialPassRenderingBindings(
              MaterialRouteClass::forward_opaque,
              MaterialShaderContract::
                  forward_scene_color_v1)
              .empty());
    CHECK(container.materialPassRenderingBindings(
              MaterialRouteClass::deferred_geometry,
              MaterialShaderContract::gbuffer_v1,
              std::optional<std::string>{
                  "another_geometry"})
              .empty());
}

TEST_CASE(
    "material pass container resolves one sampled or local-read shader input ABI across variants",
    "[renderingpass][material][local-read][wp220]") {
    RenderingPassContainer container;
    const GlobalRenderTargetId unrelated{4};
    const GlobalRenderTargetId source{5};
    const GlobalRenderTargetId output{6};

    PassDefinition material;
    material.name = "decal";
    material.pass_info = MaterialPassInfo{};
    material.input_targets = {
        unrelated, source};
    material.input_target_history = {
        false, false};
    material.output_color = {output};
    material.materialInfo()
        .material_resources.push_back(
            MaterialPassResourceBinding{
                .port =
                    ShaderResourcePortDefinition{
                        .name = "gbuffer_normal",
                        .resource = "normal",
                        .kind =
                            ShaderResourcePortKind::
                                image,
                        .access =
                            ShaderResourcePortAccess::
                                sampled,
                    },
                .target = source,
                .footprint = {
                    LogicalReadFootprintKind::
                        same_pixel,
                    std::nullopt,
                },
            });

    CompiledPassRenderingContract rendering;
    rendering.local_read_scope = true;
    rendering.local_read_extent =
        vk::Extent2D{48, 24};
    rendering.color_attachments = {
        source, output};
    rendering.color_attachment_locations = {
        unusedPhysicalAttachmentMapping, 0};
    rendering.color_attachment_input_indices = {
        1, unusedPhysicalAttachmentMapping};
    container.registerCompiledRenderingPass(
        CompiledRenderingPass{
            "flat",
            {{material, PassId{0}, {}, rendering}},
            {}});

    const std::array requests{
        MaterialPassShaderInputRequest{
            .kind =
                MaterialPassShaderInputKind::
                    material_resource,
            .name = "gbuffer_normal",
        },
    };
    const auto local =
        container.materialPassShaderInputBindings(
            MaterialRouteClass::deferred_geometry,
            MaterialShaderContract::gbuffer_v1,
            requests);
    REQUIRE(local.size() == 1);
    REQUIRE(
        local.front().input_attachment_index ==
        1u);
    REQUIRE(
        local.front().input_attachment_extent ==
        rendering.local_read_extent);

    auto sampled_rendering = rendering;
    sampled_rendering.local_read_scope = false;
    sampled_rendering
        .color_attachment_input_indices = {
            unusedPhysicalAttachmentMapping,
            unusedPhysicalAttachmentMapping};
    container.registerCompiledRenderingPass(
        CompiledRenderingPass{
            "desktop",
            {{material, PassId{0}, {},
              sampled_rendering}},
            {}});
    REQUIRE_THROWS_WITH(
        container.materialPassShaderInputBindings(
            MaterialRouteClass::deferred_geometry,
            MaterialShaderContract::gbuffer_v1,
            requests),
        Catch::Matchers::ContainsSubstring(
            "different sampled/local-read ABIs"));
}

TEST_CASE(
    "material pass container resolves one output ABI across runtime variants",
    "[renderingpass][material-output][wp218]") {
    RenderingPassContainer container;
    const MaterialOutputSchema schema{
        .name = "project.runtime_gbuffer",
        .outputs = {
            {"base_color", MaterialOutputType::vec4,
             MaterialOutputSource::surface_base_color},
            {"object_id",
             MaterialOutputType::unsigned_integer,
             MaterialOutputSource::custom},
        },
    };
    PassDefinition flat;
    flat.name = "deferred_geometry";
    flat.pass_info = MaterialPassInfo{
        .contract =
            MaterialPassContract::
                deferred_geometry_v1,
        .output_schema = schema,
    };
    container.registerCompiledRenderingPass(
        CompiledRenderingPass{
            "flat", {{flat, PassId{0}}}, {}});
    REQUIRE(
        container.materialOutputSchema(
            MaterialRouteClass::deferred_geometry) ==
        schema);
    REQUIRE_FALSE(
        container.materialOutputSchema(
            MaterialRouteClass::forward_opaque));

    auto xr = flat;
    xr.materialInfo().output_schema->name =
        "project.xr_drift";
    container.registerCompiledRenderingPass(
        CompiledRenderingPass{
            "xr", {{xr, PassId{0}}}, {}});
    REQUIRE_THROWS_WITH(
        container.materialOutputSchema(
            MaterialRouteClass::deferred_geometry),
        Catch::Matchers::ContainsSubstring(
            "different material_outputs schemas"));
}

TEST_CASE("material pass info parser preserves defaults when material range is omitted", "[renderingpass]") {
    const auto pass_def = materialPassOf(nlohmann::json::object(), "geometry");

    REQUIRE(pass_def.materialInfo().material_start == 0);
    REQUIRE(pass_def.materialInfo().material_count == 0);
}

TEST_CASE("material pass info parser rejects malformed material range", "[renderingpass]") {

    const nlohmann::json pass_json{
        {"material_range", 1},
    };

    TestSupport::requireNamedRefusal([&] { (void)materialPassOf(pass_json, "geometry"); },
        "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "geometry"}, {"field", "material_range"}, {"reason", "not_an_object"}});
    const auto accepted = materialPassOf(nlohmann::json{{"material_range", {{"start", 3}, {"count", 7}}}}, "geometry");
    CHECK(accepted.materialInfo().material_start == 3);
    CHECK(accepted.materialInfo().material_count == 7);
}

TEST_CASE("rendering pass JSON helpers reject malformed values", "[renderingpass]") {
    REQUIRE_THROWS_AS(parseAuthoredImageFormat("UNKNOWN"), std::runtime_error);
    REQUIRE_THROWS_AS(parseAuthoredImageUsage(std::vector<std::string>{}), std::runtime_error);
    REQUIRE_THROWS_AS(typedFixtureOf("compute").pass_info, std::runtime_error);
    REQUIRE_THROWS_AS(typedFixtureOf("fullscreen", Json{{"push_constants", "camera"}}), std::runtime_error);
    REQUIRE_THROWS_AS(parseAuthoredAttachmentLoadOp("keep"), std::runtime_error);
    REQUIRE_THROWS_AS(parseAuthoredAttachmentStoreOp("load"), std::runtime_error);

    REQUIRE_THROWS_AS(validateName("", "Pass"), std::runtime_error);
    REQUIRE_THROWS_AS(parseStringField(nlohmann::json{{"name", 1}}, "name", "test"), std::runtime_error);
    REQUIRE_THROWS_AS(parseFloatField(nlohmann::json{{"scale", "large"}}, "scale", "test"), std::runtime_error);
    REQUIRE_THROWS_AS(parseStringArrayField(nlohmann::json{{"usage", nlohmann::json::array({"SAMPLED", 1})}},
                                            "usage", "test"),
                      std::runtime_error);
    REQUIRE_THROWS_AS(parseUint32Field(nlohmann::json{{"count", -1}}, "count", "test"), std::runtime_error);
    REQUIRE_THROWS_AS(typedFixtureOf("ui", Json{{"clear_color", Json::array({1.0f, 0.0f, 0.0f})}}), std::runtime_error);
    REQUIRE_THROWS_AS(Pelican::test_support::renderTargetDefinitionsOf(nlohmann::json{{"render_targets", 1}}),
                      std::runtime_error);
}

TEST_CASE("rendering pass runtime compiler pairs definitions with pass ids", "[renderingpass]") {
    PassDefinition material_pass;
    material_pass.name = "geometry";
    material_pass.pass_info = MaterialPassInfo{};

    PassDefinition ui_pass;
    ui_pass.name = "ui";
    ui_pass.pass_info = UiPassInfo{};

    RenderingPassDefinition definition;
    definition.name = "main";
    definition.passes = {material_pass, ui_pass};

    const auto compiled = compileRenderingPassRuntime(definition);

    REQUIRE(compiled.name == "main");
    REQUIRE(compiled.passes.size() == 2);
    REQUIRE(compiled.passes[0].definition.name == "geometry");
    REQUIRE(compiled.passes[0].pass_id.value == 0);
    REQUIRE(compiled.passes[1].definition.name == "ui");
    REQUIRE(compiled.passes[1].pass_id.value == 1);
    REQUIRE(compiled.passes[1].definition.isUi());
}

TEST_CASE("rendering pass runtime compiler compiles definition lists", "[renderingpass]") {
    PassDefinition material_pass;
    material_pass.name = "geometry";
    material_pass.pass_info = MaterialPassInfo{};

    RenderingPassDefinition main_definition;
    main_definition.name = "main";
    main_definition.passes = {material_pass};

    RenderingPassDefinition shadow_definition;
    shadow_definition.name = "shadow";
    shadow_definition.passes = {material_pass};

    const auto compiled = compileRenderingPassesRuntime({main_definition, shadow_definition});

    REQUIRE(compiled.size() == 2);
    REQUIRE(compiled[0].name == "main");
    REQUIRE(compiled[1].name == "shadow");
    REQUIRE(compiled[1].passes[0].pass_id.value == 0);
}

TEST_CASE("rendering pass runtime compiler requires fullscreen dependencies", "[renderingpass]") {
    PassDefinition fullscreen_pass;
    fullscreen_pass.name = "postprocess";
    fullscreen_pass.pass_info = FullscreenPassInfo{};
    fullscreen_pass.output_color = {swapchainRenderTargetId()};

    RenderingPassDefinition definition;
    definition.name = "main";
    definition.passes = {fullscreen_pass};

    REQUIRE_THROWS_AS(compileRenderingPassRuntime(definition), std::runtime_error);
}

TEST_CASE(
    "rendering pass runtime compiler applies per-attachment physical operations",
    "[renderingpass][physical-attachment]") {
    PassDefinition pass;
    pass.name = "geometry";
    pass.pass_info = MaterialPassInfo{};
    pass.output_color = {
        swapchainRenderTargetId()};

    RenderingPassDefinition definition;
    definition.name = "main";
    definition.passes = {pass};

    VulkanTargetPlan plan;
    plan.scopes = {
        {
            .id = "geometry",
            .nodes = {"geometry"},
        },
    };
    plan.attachments = {
        {
            .node = "geometry",
            .logical_resource = "swapchain",
            .aspect =
                AttachmentAspect::color,
            .operations =
                PassAttachmentOperations{
                    AttachmentLoadOp::dont_care,
                    AttachmentStoreOp::dont_care},
        },
    };

    const auto compiled =
        compileRenderingPassRuntime(
            definition,
            RenderingPassRuntimeDependencies{
                .target_plan = &plan,
            });
    const auto &physical =
        compiled.passes.front().definition;
    REQUIRE(
        physical.color_load_op ==
        AttachmentLoadOp::clear);
    REQUIRE((
        physical.colorAttachmentOperations(0) ==
        PassAttachmentOperations{
            AttachmentLoadOp::dont_care,
            AttachmentStoreOp::dont_care,
        }));

    plan.attachments.front().aspect =
        AttachmentAspect::depth;
    REQUIRE_THROWS_WITH(
        compileRenderingPassRuntime(
            definition,
            RenderingPassRuntimeDependencies{
                .target_plan = &plan,
            }),
        Catch::Matchers::ContainsSubstring(
            "attachment aspect disagrees"));
}

TEST_CASE(
    "runtime target format selects typed clear values independently of material schema",
    "[renderingpass][material-output][typed-clear][wp218]") {
    const GlobalRenderTargetId object_id{0};
    const RenderTargetMetadataResolver metadata{
        [=](GlobalRenderTargetId id) {
            REQUIRE(id == object_id);
            return RenderTargetMetadata{
                .name = "object_id",
                .usage =
                    ImageUsageBit::
                        color_attachment,
                .format = ImageFormat::r32_uint,
                .extent = {64, 64},
            };
        }};
    PassDefinition pass;
    pass.name = "clear_object_id";
    pass.pass_info = UiPassInfo{};
    pass.output_color = {object_id};
    pass.clear_color = {
        4294967295.0, 7.0, 0.0, 1.0};

    const auto compiled =
        compileRenderingPassRuntime(
            RenderingPassDefinition{
                .name = "typed_clear",
                .passes = {pass},
            },
            RenderingPassRuntimeDependencies{
                .render_target_metadata =
                    &metadata,
            });
    const auto &physical =
        compiled.passes.front().definition;
    REQUIRE(
        physical
            .physical_color_numeric_classes ==
        std::vector<MaterialOutputNumericClass>{
            MaterialOutputNumericClass::
                unsigned_integer});
    const auto clear =
        physical.colorClearValue(0);
    REQUIRE(
        clear.numeric_class ==
        MaterialOutputNumericClass::
            unsigned_integer);
    REQUIRE(
        clear.vulkan().uint32[0] ==
        std::numeric_limits<
            std::uint32_t>::max());
    REQUIRE(clear.vulkan().uint32[1] == 7u);

    PassDefinition floating;
    floating.output_color = {object_id};
    floating.clear_color = {
        std::numeric_limits<double>::max(),
        0.0, 0.0, 0.0};
    REQUIRE_THROWS_WITH(
        floating.colorClearValue(0),
        "floating color clear value is out of range");
}

TEST_CASE(
    "rendering pass runtime compiler expands fused attachment mappings",
    "[renderingpass][physical-scope][local-read]") {
    const GlobalRenderTargetId gbuffer{0};
    const GlobalRenderTargetId lit{1};
    const RenderTargetMetadataResolver metadata{
        [=](GlobalRenderTargetId id) {
            if (id == gbuffer) {
                return RenderTargetMetadata{
                    .name = "gbuffer",
                    .usage =
                        ImageUsageBit::
                            color_attachment,
                    .format =
                        ImageFormat::r8g8b8a8_unorm,
                    .extent = {64, 64},
                };
            }
            if (id == lit) {
                return RenderTargetMetadata{
                    .name = "lit",
                    .usage =
                        ImageUsageBit::
                            color_attachment,
                    .format =
                        ImageFormat::r16g16b16a16_sfloat,
                    .extent = {64, 64},
                };
            }
            throw std::runtime_error(
                "unknown test render target");
        }};

    PassDefinition geometry;
    geometry.name = "geometry";
    geometry.pass_info = MaterialPassInfo{};
    geometry.output_color = {gbuffer};
    geometry.clear_color =
        {0.25, 0.5, 0.75, 1.0};

    PassDefinition lighting;
    lighting.name = "lighting";
    lighting.pass_info = MaterialPassInfo{};
    lighting.input_targets = {gbuffer};
    lighting.input_target_history = {false};
    lighting.output_color = {lit};
    lighting.clear_color =
        {0.0, 0.0, 0.0, 1.0};

    const RenderingPassDefinition definition{
        .name = "main",
        .passes = {geometry, lighting},
    };
    VulkanTargetPlan plan;
    plan.resources = {
        {
            .logical_resource = "gbuffer",
            .representation =
                VulkanResourceRepresentation::
                    tile_local_attachment,
        },
        {
            .logical_resource = "lit",
            .representation =
                VulkanResourceRepresentation::
                    materialized_image,
        },
    };
    plan.scopes = {
        {
            .id = "geometry_lighting",
            .nodes = {"geometry", "lighting"},
            .single_rendering_instance = true,
            .local_reads = {"gbuffer"},
        },
    };
    plan.attachments = {
        {
            .node = "geometry",
            .logical_resource = "gbuffer",
            .operations =
                PassAttachmentOperations{
                    AttachmentLoadOp::dont_care,
                    AttachmentStoreOp::dont_care},
        },
        {
            .node = "lighting",
            .logical_resource = "lit",
            .operations =
                PassAttachmentOperations{
                    AttachmentLoadOp::clear,
                    AttachmentStoreOp::store},
        },
    };

    const auto compiled =
        compileRenderingPassRuntime(
            definition,
            RenderingPassRuntimeDependencies{
                .render_target_metadata = &metadata,
                .target_plan = &plan,
            });
    REQUIRE(compiled.passes.size() == 2);
    const auto &geometry_contract =
        compiled.passes.at(0).rendering;
    const auto &lighting_contract =
        compiled.passes.at(1).rendering;
    REQUIRE(geometry_contract.scope_index == 0);
    REQUIRE(
        geometry_contract.scope_id ==
        "geometry_lighting");
    REQUIRE(
        geometry_contract.color_attachments ==
        std::vector<RasterAttachmentView>{
            gbuffer, lit});
    REQUIRE(
        geometry_contract
            .color_attachment_locations ==
        std::vector<std::uint32_t>{
            0,
            unusedPhysicalAttachmentMapping});
    REQUIRE(
        geometry_contract
            .color_attachment_input_indices ==
        std::vector<std::uint32_t>{
            unusedPhysicalAttachmentMapping,
            unusedPhysicalAttachmentMapping});
    REQUIRE(
        lighting_contract
            .color_attachment_locations ==
        std::vector<std::uint32_t>{
            unusedPhysicalAttachmentMapping,
            0});
    REQUIRE(
        lighting_contract
            .color_attachment_input_indices ==
        std::vector<std::uint32_t>{
            0,
            unusedPhysicalAttachmentMapping});
    REQUIRE(
        lighting_contract.local_read_scope);
    REQUIRE(
        lighting_contract.local_read_extent ==
        vk::Extent2D{64, 64});
    const auto expected_scope_operations =
        std::vector<PassAttachmentOperations>{
            PassAttachmentOperations{
                AttachmentLoadOp::dont_care,
                AttachmentStoreOp::dont_care,
            },
            PassAttachmentOperations{
                AttachmentLoadOp::clear,
                AttachmentStoreOp::store,
            },
        };
    REQUIRE(
        geometry_contract
            .scope_color_attachment_operations ==
        expected_scope_operations);
    REQUIRE(
        lighting_contract
            .scope_color_attachment_operations ==
        expected_scope_operations);
    const auto expected_scope_clear_values =
        std::vector<PhysicalColorClearValue>{
            {
                .numeric_class =
                    MaterialOutputNumericClass::floating,
                .floating =
                    {0.25f, 0.5f, 0.75f, 1.0f},
                .signed_integer = {},
                .unsigned_integer = {},
            },
            {
                .numeric_class =
                    MaterialOutputNumericClass::floating,
                .floating =
                    {0.0f, 0.0f, 0.0f, 1.0f},
                .signed_integer = {},
                .unsigned_integer = {},
            },
        };
    REQUIRE(
        geometry_contract.scope_color_clear_values ==
        expected_scope_clear_values);
    REQUIRE(
        lighting_contract.scope_color_clear_values ==
        geometry_contract.scope_color_clear_values);

    auto repeated_definition = definition;
    repeated_definition.passes.front()
        .output_color.push_back(lit);
    auto repeated_plan = plan;
    repeated_plan.attachments.insert(
        repeated_plan.attachments.begin() + 1,
        VulkanPhysicalAttachmentPlan{
            .node = "geometry",
            .logical_resource = "lit",
            .operations =
                PassAttachmentOperations{
                    AttachmentLoadOp::clear,
                    AttachmentStoreOp::store},
        });
    REQUIRE_THROWS_WITH(
        compileRenderingPassRuntime(
            repeated_definition,
            RenderingPassRuntimeDependencies{
                .render_target_metadata = &metadata,
                .target_plan = &repeated_plan,
            }),
        Catch::Matchers::ContainsSubstring(
            "cannot preserve intermediate materialized color "
            "attachment operations"));
    repeated_plan.attachments.back().operations.load_op =
        AttachmentLoadOp::load;
    REQUIRE_NOTHROW(
        compileRenderingPassRuntime(
            repeated_definition,
            RenderingPassRuntimeDependencies{
                .render_target_metadata = &metadata,
                .target_plan = &repeated_plan,
            }));

    plan.scopes.front().local_reads.clear();
    plan.scopes.front()
        .single_rendering_instance = false;
    REQUIRE_THROWS_WITH(
        compileRenderingPassRuntime(
            definition,
            RenderingPassRuntimeDependencies{
                .render_target_metadata = &metadata,
                .target_plan = &plan,
            }),
        Catch::Matchers::ContainsSubstring(
            "tile-local pass input is not declared"));
}

TEST_CASE(
    "rendering pass runtime compiler carries raster subresources into the physical scope contract",
    "[renderingpass][attachment-subresource][wp235]") {
    const GlobalRenderTargetId probe{0};
    const RenderTargetMetadataResolver metadata{
        [=](GlobalRenderTargetId id) {
            if (id != probe) {
                throw std::runtime_error(
                    "unexpected target");
            }
            return RenderTargetMetadata{
                .name = "probe",
                .usage =
                    ImageUsageBit::
                        color_attachment,
                .format =
                    ImageFormat::r8g8b8a8_unorm,
                .extent = {64, 64},
                .samples = 1,
                .mip_levels = 4,
                .array_layers = 8,
            };
        }};
    const auto range = ImageSubresourceRange{
        .base_mip_level = 2,
        .base_array_layer = 3,
        .layer_count = 2,
    };
    PassDefinition pass;
    pass.name = "probe_pass";
    pass.output_color = {
        RasterAttachmentView{probe, range}};
    RenderingPassDefinition definition{
        .name = "probe_graph",
        .passes = {pass},
    };
    VulkanTargetPlan plan;
    plan.resources = {
        VulkanPhysicalResourcePlan{
            .logical_resource = "probe",
            .format = ImageFormat::r8g8b8a8_unorm,
            .mip_levels =
                ImageMipLevelCount{
                    .mode =
                        ImageMipLevelMode::fixed,
                    .count = 4,
                },
            .array_layers = 8,
        },
    };
    plan.scopes = {
        VulkanPhysicalScopePlan{
            .id = "scope:probe",
            .kind =
                VulkanPhysicalScopeKind::rendering,
            .nodes = {"probe_pass"},
            .view_count = 2,
            .execution_count = 2,
        },
    };
    plan.attachments = {
        VulkanPhysicalAttachmentPlan{
            .node = "probe_pass",
            .logical_resource = "probe",
            .subresource = range,
            .aspect =
                AttachmentAspect::color,
            .operations =
                PassAttachmentOperations{
                    AttachmentLoadOp::clear,
                    AttachmentStoreOp::store},
        },
    };

    const auto compiled =
        compileRenderingPassRuntime(
            definition,
            RenderingPassRuntimeDependencies{
                .render_target_metadata = &metadata,
                .target_plan = &plan,
            });
    REQUIRE(compiled.passes.size() == 1);
    REQUIRE(
        compiled.passes.front()
            .rendering.color_attachments ==
        std::vector<RasterAttachmentView>{
            RasterAttachmentView{probe, range}});

    auto invalid = definition;
    invalid.passes.front()
        .output_color.front()
        .subresource->layer_count = 1;
    REQUIRE_THROWS_WITH(
        compileRenderingPassRuntime(
            invalid,
            RenderingPassRuntimeDependencies{
                .render_target_metadata = &metadata,
                .target_plan = &plan,
            }),
        Catch::Matchers::ContainsSubstring(
            "layer_count must match"));
}

TEST_CASE(
    "rendering pass runtime compiler materializes one dynamic-rendering instance for fused scopes",
    "[renderingpass][physical-scope][materialized][fusion]") {
    const GlobalRenderTargetId color{0};
    const RenderTargetMetadataResolver metadata{
        [=](GlobalRenderTargetId id) {
            if (id != color) {
                throw std::runtime_error(
                    "unknown test render target");
            }
            return RenderTargetMetadata{
                .name = "scene_color",
                .usage =
                    ImageUsageBit::
                        color_attachment,
                .format =
                    ImageFormat::r8g8b8a8_unorm,
                .extent = {64, 64},
            };
        }};

    PassDefinition base;
    base.name = "base";
    base.pass_info = MaterialPassInfo{};
    base.output_color = {color};

    PassDefinition overlay;
    overlay.name = "overlay";
    overlay.pass_info = MaterialPassInfo{};
    overlay.output_color = {color};
    overlay.color_load_op =
        AttachmentLoadOp::load;

    const RenderingPassDefinition definition{
        .name = "main",
        .passes = {base, overlay},
    };
    VulkanTargetPlan plan;
    plan.scopes = {
        {
            .id = "base_overlay",
            .nodes = {"base", "overlay"},
            .single_rendering_instance = true,
        },
    };
    plan.attachments = {
        {
            .node = "base",
            .logical_resource =
                "scene_color",
            .operations =
                PassAttachmentOperations{
                    AttachmentLoadOp::clear,
                    AttachmentStoreOp::store},
        },
        {
            .node = "overlay",
            .logical_resource =
                "scene_color",
            .operations =
                PassAttachmentOperations{
                    AttachmentLoadOp::load,
                    AttachmentStoreOp::store},
        },
    };

    const auto compiled =
        compileRenderingPassRuntime(
            definition,
            RenderingPassRuntimeDependencies{
                .render_target_metadata =
                    &metadata,
                .target_plan = &plan,
            });
    REQUIRE(compiled.passes.size() == 2);
    REQUIRE(
        compiled.passes[0].rendering
            .fused_rendering_scope);
    REQUIRE(
        compiled.passes[1].rendering
            .fused_rendering_scope);
    REQUIRE_FALSE(
        compiled.passes[0].rendering
            .local_read_scope);
    REQUIRE(
        compiled.passes[0].rendering
            .scope_color_attachment_operations ==
        std::vector<PassAttachmentOperations>{
            PassAttachmentOperations{
                AttachmentLoadOp::clear,
                AttachmentStoreOp::store,
            }});

    auto multiview_plan = plan;
    multiview_plan.view_execution_plan
        .view_count = 2;
    multiview_plan.view_execution_plan
        .uses_multiview = true;
    multiview_plan.scopes.front()
        .view_execution =
        VulkanScopeViewExecution::multiview;
    multiview_plan.scopes.front()
        .view_count = 2;
    multiview_plan.scopes.front()
        .execution_count = 1;
    multiview_plan.scopes.front()
        .view_mask = 0b11;
    REQUIRE_THROWS_WITH(
        compileRenderingPassRuntime(
            definition,
            RenderingPassRuntimeDependencies{
                .render_target_metadata =
                    &metadata,
                .target_plan =
                    &multiview_plan,
            }),
        Catch::Matchers::ContainsSubstring(
            "unsupported production pass implementation: base"));

    auto invalid = plan;
    invalid.attachments[1].operations.load_op =
        AttachmentLoadOp::
            clear;
    REQUIRE_THROWS_WITH(
        compileRenderingPassRuntime(
            definition,
            RenderingPassRuntimeDependencies{
                .render_target_metadata =
                    &metadata,
                .target_plan = &invalid,
            }),
        Catch::Matchers::ContainsSubstring(
            "must Load every attachment"));
}

TEST_CASE(
    "view-family scheduler expands mixed physical scopes in scope order",
    "[renderingpass][view-execution][schedule]") {
    const std::vector<FrameGraphExecutionNode> nodes{
        {.name = "SharedShadow"},
        {.name = "GBuffer"},
        {.name = "Transparent"},
        {.name = "Present"},
    };
    VulkanTargetPlan plan;
    plan.view_execution_plan.view_count = 2;
    plan.view_execution_plan.uses_multiview = true;
    plan.view_execution_plan.mixed_execution = true;
    plan.scopes = {
        {
            .id = "shared",
            .nodes = {"SharedShadow"},
            .view_execution =
                VulkanScopeViewExecution::single_view,
            .view_count = 1,
            .execution_count = 1,
        },
        {
            .id = "gbuffer",
            .nodes = {"GBuffer"},
            .view_execution =
                VulkanScopeViewExecution::multiview,
            .view_count = 2,
            .execution_count = 1,
            .view_mask = 0b11,
        },
        {
            .id = "sequential",
            .nodes = {"Transparent"},
            .view_execution =
                VulkanScopeViewExecution::sequential,
            .view_count = 2,
            .execution_count = 2,
        },
        {
            .id = "present",
            .nodes = {"Present"},
            .view_execution =
                VulkanScopeViewExecution::multiview,
            .view_count = 2,
            .execution_count = 1,
            .view_mask = 0b11,
        },
    };

    const auto schedule =
        buildLogicalFrameViewFamilySchedule(
            nodes, plan, 2);
    REQUIRE(schedule.size() == 5);
    REQUIRE(schedule[0].node_index == 0);
    REQUIRE(schedule[0].execution ==
            VulkanScopeViewExecution::single_view);
    REQUIRE(schedule[1].node_index == 1);
    REQUIRE(schedule[1].execution ==
            VulkanScopeViewExecution::multiview);
    REQUIRE(schedule[2].node_index == 2);
    REQUIRE(schedule[2].view_index == 0);
    REQUIRE(schedule[2].firstExecution());
    REQUIRE_FALSE(schedule[2].lastExecution());
    REQUIRE(schedule[3].node_index == 2);
    REQUIRE(schedule[3].view_index == 1);
    REQUIRE_FALSE(schedule[3].firstExecution());
    REQUIRE(schedule[3].lastExecution());
    REQUIRE(schedule[4].node_index == 3);
    REQUIRE(schedule[4].execution ==
            VulkanScopeViewExecution::multiview);
    REQUIRE(schedule[4].logical_view_count == 2);
}

TEST_CASE(
    "view-family scheduler keeps fused sequential scopes together per view",
    "[renderingpass][view-execution][schedule][scope]") {
    const std::vector<FrameGraphExecutionNode> nodes{
        {.name = "Geometry"},
        {.name = "Lighting"},
    };
    VulkanTargetPlan plan;
    plan.view_execution_plan.view_count = 2;
    plan.scopes = {
        {
            .id = "fused",
            .nodes = {"Geometry", "Lighting"},
            .view_execution =
                VulkanScopeViewExecution::sequential,
            .view_count = 2,
            .execution_count = 2,
        },
    };

    const auto schedule =
        buildLogicalFrameViewFamilySchedule(
            nodes, plan, 2);
    REQUIRE(schedule.size() == 4);
    REQUIRE(schedule[0].node_index == 0);
    REQUIRE(schedule[0].view_index == 0);
    REQUIRE(schedule[0].beginsScopeExecution());
    REQUIRE_FALSE(
        schedule[0].endsScopeExecution());
    REQUIRE(schedule[1].node_index == 1);
    REQUIRE(schedule[1].view_index == 0);
    REQUIRE_FALSE(
        schedule[1].beginsScopeExecution());
    REQUIRE(schedule[1].endsScopeExecution());
    REQUIRE(schedule[2].node_index == 0);
    REQUIRE(schedule[2].view_index == 1);
    REQUIRE(schedule[2].beginsScopeExecution());
    REQUIRE(schedule[3].node_index == 1);
    REQUIRE(schedule[3].view_index == 1);
    REQUIRE(schedule[3].endsScopeExecution());
}

TEST_CASE(
    "view-family scheduler preserves registered FramePlan order across fused scopes",
    "[renderingpass][view-execution][schedule][reorder]") {
    VulkanTargetPlan plan;
    plan.graph = "registered";
    plan.view_execution_plan.view_count = 1;
    plan.scopes = {
        {
            .id = "first",
            .nodes = {"C"},
        },
        {
            .id = "adjacent_fused",
            .nodes = {"A", "D"},
            .single_rendering_instance = true,
        },
        {
            .id = "tail",
            .nodes = {"B"},
        },
    };

    FramePlan frame{.name = "registered"};
    for (const auto name : {"C", "A", "D", "B"})
        frame.nodes.push_back(FramePlanNode{.name = name, .kind = FramePlanNodeKind::anchor,
                                            .order = frame.nodes.size()});
    FrameGraphRuntimeContainer runtime;
    CompiledRenderingPass compiled;
    compiled.name = "registered";
    runtime.registerExecutionPlan(RenderingPassId{0}, compiled, frame, std::make_shared<CompiledRenderPipeline>(),
                                  std::make_shared<VulkanTargetPlan>(plan));
    const auto execution = runtime.find(RenderingPassId{0});
    REQUIRE(execution != nullptr);
    const auto schedule =
        buildLogicalFrameViewFamilySchedule(
            execution->nodes, *execution->target_plan, 1);
    REQUIRE(schedule.size() == 4);
    REQUIRE(
        std::vector<std::size_t>{
            schedule[0].node_index,
            schedule[1].node_index,
            schedule[2].node_index,
            schedule[3].node_index} ==
        std::vector<std::size_t>{0, 1, 2, 3});
    REQUIRE(
        schedule[1].beginsScopeExecution());
    REQUIRE_FALSE(
        schedule[1].endsScopeExecution());
    REQUIRE(
        schedule[2].endsScopeExecution());
    REQUIRE(
        schedule[1].scope_index ==
        schedule[2].scope_index);
}

TEST_CASE(
    "per-view scheduler preserves complete physical scopes",
    "[renderingpass][view-execution][schedule][scope]") {
    const std::vector<FrameGraphExecutionNode> nodes{
        {.name = "Shared"},
        {.name = "Geometry"},
        {.name = "Lighting"},
    };
    VulkanTargetPlan plan;
    plan.view_execution_plan.view_count = 2;
    plan.scopes = {
        {
            .id = "shared",
            .nodes = {"Shared"},
            .view_execution =
                VulkanScopeViewExecution::single_view,
            .view_count = 1,
            .execution_count = 1,
        },
        {
            .id = "fused",
            .nodes = {"Geometry", "Lighting"},
            .view_execution =
                VulkanScopeViewExecution::sequential,
            .view_count = 2,
            .execution_count = 2,
        },
    };

    const auto logical_frame_schedule =
        buildLogicalFrameViewFamilySchedule(
            nodes, plan, 2);
    const auto first_view =
        selectLogicalFrameSequentialViewSchedule(
            logical_frame_schedule, 0, 2);
    const auto second_view =
        selectLogicalFrameSequentialViewSchedule(
            logical_frame_schedule, 1, 2);

    REQUIRE(first_view.size() == 3);
    REQUIRE(first_view[0].node_index == 0);
    REQUIRE(first_view[1].node_index == 1);
    REQUIRE(first_view[1].beginsScopeExecution());
    REQUIRE(first_view[2].node_index == 2);
    REQUIRE(first_view[2].endsScopeExecution());
    REQUIRE(second_view.size() == 2);
    REQUIRE(second_view[0].node_index == 1);
    REQUIRE(second_view[0].beginsScopeExecution());
    REQUIRE(second_view[1].node_index == 2);
    REQUIRE(second_view[1].endsScopeExecution());
}

TEST_CASE(
    "view-family scheduler expands secondary families without repeating them per main view",
    "[renderingpass][view-execution][schedule][secondary-family]") {
    const std::vector<FrameGraphExecutionNode> nodes{
        {
            .name = "DirectionalShadow",
            .view_family =
                std::string{
                    directionalShadowRenderViewFamilyId},
        },
        {.name = "Geometry"},
    };
    VulkanTargetPlan plan;
    plan.view_execution_plan.view_count = 2;
    plan.scopes = {
        {
            .id = "shadow",
            .nodes = {"DirectionalShadow"},
            .view_execution =
                VulkanScopeViewExecution::single_view,
            .view_count = 1,
            .execution_count = 1,
        },
        {
            .id = "geometry",
            .nodes = {"Geometry"},
            .view_execution =
                VulkanScopeViewExecution::sequential,
            .view_count = 2,
            .execution_count = 2,
        },
    };
    const std::array families{
        LogicalFrameViewFamilyCardinality{
            mainRenderViewFamilyId, 2},
        LogicalFrameViewFamilyCardinality{
            directionalShadowRenderViewFamilyId,
            3},
    };

    const auto schedule =
        buildLogicalFrameViewFamilySchedule(
            nodes, plan, families);
    REQUIRE(schedule.size() == 5);
    for (std::uint32_t cascade = 0;
         cascade < 3; ++cascade) {
        INFO("cascade " << cascade);
        REQUIRE(
            schedule[cascade].view_family ==
            "$shadow/directional");
        REQUIRE(
            schedule[cascade].execution ==
            VulkanScopeViewExecution::sequential);
        REQUIRE(
            schedule[cascade].logical_view_count ==
            3);
        REQUIRE(
            schedule[cascade].view_index ==
            cascade);
        REQUIRE(
            schedule[cascade].execution_index ==
            cascade);
        REQUIRE(
            schedule[cascade].execution_count ==
            3);
    }
    REQUIRE(schedule[0].firstExecution());
    REQUIRE_FALSE(schedule[0].lastExecution());
    REQUIRE_FALSE(schedule[1].firstExecution());
    REQUIRE_FALSE(schedule[1].lastExecution());
    REQUIRE_FALSE(schedule[2].firstExecution());
    REQUIRE(schedule[2].lastExecution());
    REQUIRE(
        schedule[3].view_family ==
        "$main");
    REQUIRE(
        schedule[3].view_index == 0);
    REQUIRE(
        schedule[4].view_index == 1);

    const auto first_view =
        selectLogicalFrameSequentialViewSchedule(
            schedule, 0, 2);
    const auto second_view =
        selectLogicalFrameSequentialViewSchedule(
            schedule, 1, 2);
    REQUIRE(first_view.size() == 4);
    REQUIRE(
        first_view.front().view_family ==
        "$shadow/directional");
    REQUIRE(
        first_view[2].view_index == 2);
    REQUIRE(
        first_view.back().view_family ==
        "$main");
    REQUIRE(second_view.size() == 1);
    REQUIRE(
        second_view.front().view_family ==
        "$main");

    REQUIRE_THROWS_WITH(
        buildLogicalFrameViewFamilySchedule(
            nodes, plan,
            std::span{
                families.data(), 1}),
        Catch::Matchers::ContainsSubstring(
            "does not provide view family"));

    auto mixed = plan;
    mixed.scopes = {{
        .id = "mixed",
        .nodes =
            {"DirectionalShadow", "Geometry"},
        .view_execution =
            VulkanScopeViewExecution::single_view,
        .view_count = 1,
        .execution_count = 1,
    }};
    REQUIRE_THROWS_WITH(
        buildLogicalFrameViewFamilySchedule(
            nodes, mixed, families),
        Catch::Matchers::ContainsSubstring(
            "mixes view families"));

    auto non_template = plan;
    non_template.scopes.front().view_execution =
        VulkanScopeViewExecution::sequential;
    non_template.scopes.front().view_count = 3;
    non_template.scopes.front().execution_count = 3;
    REQUIRE_THROWS_WITH(
        buildLogicalFrameViewFamilySchedule(
            nodes, non_template, families),
        Catch::Matchers::ContainsSubstring(
            "must be a single-view template"));
}

TEST_CASE(
    "LP-Va view_select parses as axis names to non-negative integers",
    "[renderingpass][view-select][parse][lpva]") {
    const auto pass = nlohmann::json{
        {"name", "face_two"},
        {"view_family", "$capture/cube"},
        {"view_select", {{"face", 2}}},
    };
    CHECK(parseRenderViewSelect(pass, "Pass 'face_two'") ==
          RenderViewSelect{{"face", 2}});
    CHECK(parseRenderViewSelect(
              nlohmann::json{{"name", "all"}}, "Pass 'all'")
              .empty());
    for (const auto &bad : {
             nlohmann::json::object(),
             nlohmann::json::array({1}),
             nlohmann::json{{"face", -1}},
             nlohmann::json{{"face", 1.5}},
             nlohmann::json{{"face", "2"}},
             nlohmann::json{{"face", 4294967296ull}},
             nlohmann::json{{"", 0}},
         }) {
        INFO(bad.dump());
        auto invalid = pass;
        invalid["view_select"] = bad;
        CHECK_THROWS_WITH(
            parseRenderViewSelect(invalid, "Pass 'face_two'"),
            Catch::Matchers::StartsWith("Pass 'face_two' view_select"));
    }
}


namespace {

// LP-Va: a cube-shaped secondary family, six views carrying `face`.
std::vector<RenderViewAxisCoordinates> lpvaFaceAxes() {
    std::vector<RenderViewAxisCoordinates> axes;
    for (std::uint32_t face = 0; face < 6; ++face) {
        axes.push_back({{"face", face}});
    }
    return axes;
}

VulkanTargetPlan lpvaSelectedScopePlan(RenderViewSelect select) {
    VulkanTargetPlan plan;
    plan.view_execution_plan.view_count = 1;
    plan.scopes = {
        {
            .id = "capture",
            .nodes = {"Capture"},
            .view_execution =
                VulkanScopeViewExecution::single_view,
            .view_count = 1,
            .execution_count = 1,
            .view_select = std::move(select),
        },
        {
            .id = "present",
            .nodes = {"Present"},
            .view_execution =
                VulkanScopeViewExecution::single_view,
            .view_count = 1,
            .execution_count = 1,
        },
    };
    return plan;
}

std::vector<FrameGraphExecutionNode> lpvaSelectedNodes(
    RenderViewSelect select) {
    return {
        {
            .name = "Capture",
            .view_family = "$capture/cube",
            .view_select = std::move(select),
        },
        {.name = "Present"},
    };
}

} // namespace

TEST_CASE(
    "LP-Va view_select expands a secondary scope only on matching views",
    "[renderingpass][view-execution][schedule][view-select][lpva]") {
    const auto axes = lpvaFaceAxes();
    const std::array families{
        LogicalFrameViewFamilyCardinality{
            mainRenderViewFamilyId, 1},
        LogicalFrameViewFamilyCardinality{
            "$capture/cube", 6, axes},
    };

    SECTION("one matching face stays a sequential family invocation") {
        const RenderViewSelect select{{"face", 4}};
        const auto schedule =
            buildLogicalFrameViewFamilySchedule(
                lpvaSelectedNodes(select),
                lpvaSelectedScopePlan(select),
                families);
        REQUIRE(schedule.size() == 2);
        CHECK(schedule[0].node_index == 0);
        CHECK(schedule[0].view_family == "$capture/cube");
        // Not single_view: snapshot_copy would then copy layer 0.
        CHECK(schedule[0].execution ==
              VulkanScopeViewExecution::sequential);
        // The family size, so the attachment keeps all six layers and the
        // invocation renders into layer base + 4.
        CHECK(schedule[0].logical_view_count == 6);
        CHECK(schedule[0].view_index == 4);
        CHECK(schedule[0].execution_count == 1);
        CHECK(schedule[0].firstExecution());
        CHECK(schedule[0].lastExecution());
        CHECK_FALSE(schedule[0].no_selected_view);
        CHECK(schedule[1].view_family == "$main");
    }

    SECTION("a partial selection keeps the family's index order") {
        std::vector<RenderViewAxisCoordinates> lights;
        for (std::uint32_t light = 0; light < 2; ++light) {
            for (std::uint32_t cascade = 0; cascade < 3;
                 ++cascade) {
                lights.push_back(
                    {{"light", light}, {"cascade", cascade}});
            }
        }
        const std::array shadow_families{
            LogicalFrameViewFamilyCardinality{
                mainRenderViewFamilyId, 1},
            LogicalFrameViewFamilyCardinality{
                "$capture/cube", 6, lights},
        };
        const RenderViewSelect select{{"cascade", 1}};
        const auto schedule =
            buildLogicalFrameViewFamilySchedule(
                lpvaSelectedNodes(select),
                lpvaSelectedScopePlan(select),
                shadow_families);
        REQUIRE(schedule.size() == 3);
        CHECK(schedule[0].view_index == 1);
        CHECK(schedule[1].view_index == 4);
        CHECK(schedule[0].execution_index == 0);
        CHECK(schedule[1].execution_index == 1);
        CHECK(schedule[1].execution_count == 2);
        CHECK(schedule[1].logical_view_count == 6);
    }

    SECTION("no matching view yields a non-recording invocation") {
        std::vector<RenderViewAxisCoordinates> one_light{
            {{"light", 0}, {"cascade", 0}}};
        const std::array one_light_families{
            LogicalFrameViewFamilyCardinality{
                mainRenderViewFamilyId, 1},
            LogicalFrameViewFamilyCardinality{
                "$capture/cube", 1, one_light},
        };
        const RenderViewSelect select{{"light", 1}};
        const auto schedule =
            buildLogicalFrameViewFamilySchedule(
                lpvaSelectedNodes(select),
                lpvaSelectedScopePlan(select),
                one_light_families);
        REQUIRE(schedule.size() == 2);
        CHECK(schedule[0].node_index == 0);
        CHECK(schedule[0].no_selected_view);
        CHECK(schedule[0].logical_view_count == 1);
        CHECK(schedule[0].beginsScopeExecution());
        CHECK(schedule[0].endsScopeExecution());
        CHECK_FALSE(schedule[1].no_selected_view);
        // The per-view split keeps the placeholder with view 0, so the
        // execution still marks the node completed.
        const auto first_view =
            selectLogicalFrameSequentialViewSchedule(
                schedule, 0, 1);
        REQUIRE(first_view.size() == 2);
        CHECK(first_view[0].no_selected_view);
    }

    SECTION("an unknown axis on this frame's views is refused by name") {
        const RenderViewSelect select{{"cascade", 0}};
        try {
            (void)buildLogicalFrameViewFamilySchedule(
                lpvaSelectedNodes(select),
                lpvaSelectedScopePlan(select), families);
            FAIL("expected a refusal");
        } catch (const std::runtime_error &error) {
            const auto refusal =
                splitCodedRefusalMessage(error.what());
            REQUIRE(refusal);
            CHECK(refusal->code ==
                  "graph.view_select_axis_unknown@1");
            CHECK(refusal->fields ==
                  nlohmann::json{{"pass", "Capture"},
                                 {"family", "$capture/cube"},
                                 {"axis", "cascade"}});
        }
    }

    SECTION("an authored family without axes is refused by name") {
        const std::array authored{
            LogicalFrameViewFamilyCardinality{
                mainRenderViewFamilyId, 1},
            LogicalFrameViewFamilyCardinality{
                "$capture/cube", 6},
        };
        const RenderViewSelect select{{"face", 0}};
        REQUIRE_THROWS_WITH(
            buildLogicalFrameViewFamilySchedule(
                lpvaSelectedNodes(select),
                lpvaSelectedScopePlan(select), authored),
            Catch::Matchers::StartsWith(
                "graph.view_select_axis_unknown@1: "));
    }

    SECTION("a primary-family selection is refused by name") {
        const RenderViewSelect select{{"face", 0}};
        auto nodes = lpvaSelectedNodes(select);
        nodes[0].view_family = "$main";
        auto plan = lpvaSelectedScopePlan(select);
        REQUIRE_THROWS_WITH(
            buildLogicalFrameViewFamilySchedule(
                nodes, plan, families),
            Catch::Matchers::StartsWith(
                "graph.view_select_primary_family@1: "));
    }

    SECTION("a scope whose selection differs from its node is rejected") {
        auto plan =
            lpvaSelectedScopePlan({{"face", 1}});
        REQUIRE_THROWS_WITH(
            buildLogicalFrameViewFamilySchedule(
                lpvaSelectedNodes({{"face", 2}}), plan,
                families),
            Catch::Matchers::ContainsSubstring(
                "view_select does not match"));
    }
}

TEST_CASE(
    "view-family scheduler rejects incomplete or inconsistent physical contracts",
    "[renderingpass][view-execution][schedule][diagnostic]") {
    const std::vector<FrameGraphExecutionNode> nodes{
        {.name = "A"},
        {.name = "B"},
    };
    VulkanTargetPlan plan;
    plan.view_execution_plan.view_count = 2;
    plan.scopes = {
        {
            .id = "broken",
            .nodes = {"A"},
            .view_execution =
                VulkanScopeViewExecution::sequential,
            .view_count = 2,
            .execution_count = 1,
        },
    };
    const auto require_error =
        [&](std::uint32_t view_count,
            std::string_view expected) {
            try {
                (void)buildLogicalFrameViewFamilySchedule(
                    nodes, plan, view_count);
                FAIL("schedule did not reject an invalid contract");
            } catch (const std::runtime_error &error) {
                REQUIRE(std::string_view{error.what()}.find(expected) !=
                        std::string_view::npos);
            }
        };
    require_error(
        2, "inconsistent view-execution contract");

    plan.scopes.front().execution_count = 2;
    require_error(2, "no scope");
    require_error(3, "view count");
}

TEST_CASE(
    "view-layer copy planning preserves sequential and family image ranges",
    "[renderingpass][view-execution][layer-copy]") {
    REQUIRE(
        planViewLayerCopy(
            1, 1, 1, 1, 2, false) ==
        ViewLayerCopyPlan{
            .source_base_array_layer = 0,
            .destination_base_array_layer = 1,
            .array_layers = 1,
        });
    REQUIRE(
        planViewLayerCopy(
            2, 1, 1, 1, 2, false) ==
        ViewLayerCopyPlan{
            .source_base_array_layer = 1,
            .destination_base_array_layer = 1,
            .array_layers = 1,
        });
    REQUIRE(
        planViewLayerCopy(
            2, 0, 2, 0, 2, true) ==
        ViewLayerCopyPlan{
            .source_base_array_layer = 0,
            .destination_base_array_layer = 0,
            .array_layers = 2,
        });
    REQUIRE_THROWS_WITH(
        planViewLayerCopy(
            1, 0, 2, 0, 2, true),
        Catch::Matchers::ContainsSubstring(
            "does not preserve every logical view layer"));
}

} // namespace Pelican

TEST_CASE("MU-1 material substitute prefers a compatible contract then frame order and preserves no-pass",
          "[mu1][materialgenerationplanner]") {
    using namespace Pelican;
    RenderingPassContainer container;
    CHECK_FALSE(container.substituteMaterialPass(MaterialShaderContract::gbuffer_v1));
    PassDefinition first;
    first.name = "first_forward";
    first.pass_info = MaterialPassInfo{};
    first.materialInfo().contract = MaterialPassContract::forward_opaque_v1;
    PassDefinition second;
    second.name = "second_deferred";
    second.pass_info = MaterialPassInfo{};
    second.materialInfo().contract = MaterialPassContract::deferred_geometry_v1;
    container.registerCompiledRenderingPass(CompiledRenderingPass{
        "main", {{first, PassId{0}}, {second, PassId{1}}}, {}});
    const auto matching = container.substituteMaterialPass(MaterialShaderContract::gbuffer_v1);
    REQUIRE(matching);
    CHECK(matching->name == "second_deferred");
    CHECK(matching->route == MaterialRouteClass::deferred_geometry);
    const auto fallback = container.substituteMaterialPass(MaterialShaderContract::legacy_gbuffer_v1);
    REQUIRE(fallback);
    CHECK(fallback->name == "first_forward");
    CHECK(fallback->route == MaterialRouteClass::forward_opaque);
    CHECK(fallback->shader_contract == MaterialShaderContract::forward_scene_color_v1);
}
