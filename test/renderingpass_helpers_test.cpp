#include "../src/core/renderingpass/fullscreenpassinfojsonparser.hpp"
#include "../src/core/renderingpass/materialpassattachments.hpp"
#include "../src/core/renderingpass/materialpassinfojsonparser.hpp"
#include "../src/core/renderingpass/passattachmentoptionsjsonparser.hpp"
#include "../src/core/renderingpass/passdefinitionjsonparser.hpp"
#include "../src/core/renderingpass/passinfojsonparser.hpp"
#include "../src/core/renderingpass/passsequencejsonparser.hpp"
#include "../src/core/renderingpass/renderingpassjsonhelpers.hpp"
#include "../src/core/renderingpass/renderingpasscontainer.hpp"
#include "../src/core/renderingpass/renderingpassruntimecompiler.hpp"
#include "../src/core/renderingpass/renderingpasstargetjsonparser.hpp"
#include "../src/core/renderingpass/rendertargetmetadataresolver.hpp"
#include "../src/core/renderingpass/rendertargetnameresolver.hpp"
#include "../src/core/renderingpass/rendertargetjsonparser.hpp"
#include "../src/core/renderingpass/viewexecutionscheduler.hpp"
#include "../src/core/userpublic/render/pass_implementation_abi_v1.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <variant>
#include <vector>

namespace Pelican {

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
    REQUIRE(stringToFormat("R8_UNORM") == vk::Format::eR8Unorm);
    REQUIRE(stringToFormat("D32_SFLOAT") == vk::Format::eD32Sfloat);

    const auto usage = stringToUsageFlags({"COLOR_ATTACHMENT", "SAMPLED"});
    REQUIRE(static_cast<bool>(usage & vk::ImageUsageFlagBits::eColorAttachment));
    REQUIRE(static_cast<bool>(usage & vk::ImageUsageFlagBits::eSampled));

    REQUIRE(std::holds_alternative<MaterialPassInfo>(makePassInfo("material")));
    REQUIRE(std::holds_alternative<FullscreenPassInfo>(makePassInfo("fullscreen")));
    REQUIRE(std::holds_alternative<ShadowDepthPassInfo>(makePassInfo("shadow_depth")));
    REQUIRE(std::holds_alternative<UiPassInfo>(makePassInfo("ui")));

    REQUIRE(stringToFullscreenPushConstantData("projection_view") == FullscreenPushConstantData::eProjectionView);
    REQUIRE(stringToLoadOp("dont_care") == vk::AttachmentLoadOp::eDontCare);
    REQUIRE(stringToStoreOp("store") == vk::AttachmentStoreOp::eStore);

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
    REQUIRE(
        parseOptionalRegionTags(
            nlohmann::json{
                {"regions",
                 nlohmann::json::array(
                     {"region.post", "region.debug"})}},
            "test") ==
        std::vector<std::string>{
            "region.post", "region.debug"});
    REQUIRE_THROWS_WITH(
        parseOptionalRegionTags(
            nlohmann::json{
                {"regions",
                 nlohmann::json::array(
                     {"region.post", "region.post"})}},
            "test"),
        Catch::Matchers::ContainsSubstring(
            "duplicate region tag"));
    REQUIRE_THROWS_WITH(
        parseOptionalRegionTags(
            nlohmann::json{
                {"regions",
                 nlohmann::json::array({""})}},
            "test"),
        Catch::Matchers::ContainsSubstring(
            "empty or too long"));

    const auto clear_color = jsonToClearColor(nlohmann::json::array({1.0f, 0.5f, 0.25f, 1.0f}));
    REQUIRE(clear_color.float32[0] == 1.0f);
    REQUIRE(clear_color.float32[1] == 0.5f);
    REQUIRE(clear_color.float32[2] == 0.25f);
    REQUIRE(clear_color.float32[3] == 1.0f);
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

    const auto definitions = parseRenderTargetDefinitionsFromJson(config);

    REQUIRE(definitions.size() == 1);
    REQUIRE(definitions[0].name == "half_color");
    REQUIRE(definitions[0].extent_scale == 0.5f);
    REQUIRE_FALSE(definitions[0].fixed_extent.has_value());
    REQUIRE(definitions[0].format == vk::Format::eR8Unorm);
    REQUIRE(
        definitions[0].format_candidates ==
        std::vector<vk::Format>{
            vk::Format::eR8Unorm,
            vk::Format::eR16G16Sfloat});
    REQUIRE(static_cast<bool>(definitions[0].usage & vk::ImageUsageFlagBits::eColorAttachment));
    REQUIRE(static_cast<bool>(definitions[0].usage & vk::ImageUsageFlagBits::eSampled));
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
        parseRenderTargetDefinitionsFromJson(
            config),
        Catch::Matchers::ContainsSubstring(
            "format_candidates must be a string array"));
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

    const auto definitions = parseRenderTargetDefinitionsFromJson(config);

    REQUIRE(definitions.size() == 1);
    REQUIRE(definitions[0].fixed_extent.has_value());
    REQUIRE(definitions[0].fixed_extent->width == 2048);
    REQUIRE(definitions[0].fixed_extent->height == 2048);
}

TEST_CASE("render target JSON parser accepts history and its declarative clear",
          "[renderingpass][temporal]") {
    const auto definitions = parseRenderTargetDefinitionsFromJson(nlohmann::json{
        {"render_targets", nlohmann::json::array({{
            {"name", "temporal_accum"}, {"extent_scale", 1.0},
            {"format", "R16G16B16A16_SFLOAT"},
            {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
            {"history", true}, {"clear_color", {0.1f, 0.2f, 0.3f, 1.0f}}
        }})}
    });
    REQUIRE(definitions.size() == 1);
    REQUIRE(definitions.front().history);
    REQUIRE(definitions.front().history_clear_color.float32[0] == Catch::Approx(0.1f));
    REQUIRE(definitions.front().history_clear_color.float32[3] == Catch::Approx(1.0f));
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
    REQUIRE(parseRenderTargetDefinitionsFromJson(resolved).at(0).format ==
            vk::Format::eB8G8R8A8Srgb);
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

    const auto info = parseFullscreenPassInfoFromJson(pass_json, "lighting_pass");

    REQUIRE(info.push_constants == FullscreenPushConstantData::eCameraPosition);
    REQUIRE(info.uses_light_data);
    REQUIRE(info.vert_shader.ref == "fullscreen");
    REQUIRE(info.vert_shader.kind == ShaderReferenceKind::stem);
    REQUIRE(info.frag_shader.ref == "lighting");
    REQUIRE(info.frag_shader.kind == ShaderReferenceKind::stem);
}

TEST_CASE(
    "fullscreen pass JSON parser keeps per-input sampling typed",
    "[renderingpass][upscale][sampling]") {
    const nlohmann::json pass_json{
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
        parseFullscreenPassInfoFromJson(
            pass_json, "upscale");

    REQUIRE(info.input_sampling ==
            std::vector<FullscreenInputSampling>{
                {FullscreenInputFilter::nearest,
                 FullscreenInputAddressMode::clamp_to_edge},
                {FullscreenInputFilter::linear,
                 FullscreenInputAddressMode::mirrored_repeat},
            });
    auto invalid = pass_json;
    invalid["input_sampling"][0]["address"] =
        "wrap_somehow";
    REQUIRE_THROWS_AS(
        parseFullscreenPassInfoFromJson(
            invalid, "upscale"),
        std::runtime_error);
}

TEST_CASE("fullscreen pass JSON parser rejects explicit shader files and names the stem form", "[renderingpass]") {
    const nlohmann::json pass_json{
        {"shader", {{"vertex", "fullscreen.vert.spv"}, {"fragment", "lighting"}}},
    };

    std::string message;
    try {
        (void)parseFullscreenPassInfoFromJson(pass_json, "lighting_pass");
    } catch (const std::exception &ex) {
        message = ex.what();
    }
    REQUIRE(message.find("fullscreen.vert.spv") != std::string::npos);
    REQUIRE(message.find("extensionless") != std::string::npos);
    REQUIRE(message.find("vertex") != std::string::npos);
}

TEST_CASE("fullscreen pass JSON parser does not infer options from pass name", "[renderingpass]") {
    const nlohmann::json pass_json{
        {"shader", {{"vertex", "fullscreen"}, {"fragment", "lighting"}}},
    };

    const auto info = parseFullscreenPassInfoFromJson(pass_json, "lighting_pass");

    REQUIRE(info.push_constants == FullscreenPushConstantData::eNone);
    REQUIRE_FALSE(info.uses_light_data);
}

TEST_CASE("pass info JSON parser reads shadow depth shader option", "[renderingpass]") {
    PassDefinition pass_def;
    pass_def.name = "shadow_depth";
    parsePassTypeFromJson(pass_def, nlohmann::json{{"type", "shadow_depth"}});

    parseShadowDepthPassInfoIntoDefinition(pass_def, nlohmann::json::object());

    REQUIRE(pass_def.isShadowDepth());
    REQUIRE(pass_def.shadowDepthInfo().vert_shader.ref == "engine://shadow_depth");
    REQUIRE(pass_def.shadowDepthInfo().vert_shader.kind == ShaderReferenceKind::stem);

    parseShadowDepthPassInfoIntoDefinition(
        pass_def,
        nlohmann::json{{"shader", {{"vertex", "shaders/custom_shadow"}}}});

    REQUIRE(pass_def.shadowDepthInfo().vert_shader.ref == "shaders/custom_shadow");
}

TEST_CASE("fullscreen pass JSON parser rejects deprecated projection matrix flag", "[renderingpass]") {
    const nlohmann::json pass_json{
        {"needs_projection_matrix", true},
        {"shader", {{"vertex", "fullscreen"}, {"fragment", "ssao"}}},
    };

    REQUIRE_THROWS_AS(parseFullscreenPassInfoFromJson(pass_json, "ssao_pass"), std::runtime_error);
}

TEST_CASE("pass info JSON parser reads pass type and applies UI defaults", "[renderingpass]") {
    PassDefinition pass_def;
    pass_def.name = "ui";

    parsePassTypeFromJson(pass_def, nlohmann::json{{"type", "ui"}});

    REQUIRE(pass_def.isUi());
    REQUIRE(pass_def.color_load_op == vk::AttachmentLoadOp::eLoad);
}

TEST_CASE("pass info JSON parser applies fullscreen info only to fullscreen passes", "[renderingpass]") {
    PassDefinition fullscreen_pass;
    fullscreen_pass.name = "debug_texture";
    parsePassTypeFromJson(fullscreen_pass, nlohmann::json{{"type", "fullscreen"}});

    const nlohmann::json fullscreen_json{
        {"shader", {{"vertex", "fullscreen"}, {"fragment", "debug_texture"}}},
        {"push_constants", "projection_view"},
    };
    parseFullscreenPassInfoIntoDefinition(fullscreen_pass, fullscreen_json);

    REQUIRE(fullscreen_pass.fullscreenInfo().frag_shader.ref == "debug_texture");
    REQUIRE(fullscreen_pass.fullscreenInfo().push_constants == FullscreenPushConstantData::eProjectionView);

    PassDefinition material_pass;
    material_pass.name = "geometry";
    material_pass.pass_info = MaterialPassInfo{};
    parseFullscreenPassInfoIntoDefinition(material_pass, nlohmann::json::object());

    REQUIRE(material_pass.isMaterial());
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

    PassDefinition pass_def;
    pass_def.name = "postprocess";

    const nlohmann::json pass_json{
        {"output", {{"color", "color_target"}, {"depth", "depth_target"}}},
        {"input", "input_target"},
    };

    parsePassOutputTargetsFromJson(pass_def, resolver, pass_json);
    parsePassInputTargetsFromJson(pass_def, resolver, pass_json);

    REQUIRE(pass_def.output_color.size() == 1);
    REQUIRE(pass_def.output_color[0] == GlobalRenderTargetId{3});
    REQUIRE(pass_def.output_depth == GlobalRenderTargetId{4});
    REQUIRE(pass_def.input_targets.size() == 1);
    REQUIRE(pass_def.input_targets[0] == GlobalRenderTargetId{5});
}

TEST_CASE("rendering pass target JSON parser handles swapchain and omitted input", "[renderingpass]") {
    const auto resolver = RenderTargetNameResolver{[](const std::string &) { return noRenderTargetId(); }};

    PassDefinition pass_def;
    pass_def.name = "present";

    const nlohmann::json pass_json{
        {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
    };

    parsePassOutputTargetsFromJson(pass_def, resolver, pass_json);
    parsePassInputTargetsFromJson(pass_def, resolver, pass_json);

    REQUIRE(pass_def.output_color.size() == 1);
    REQUIRE(isSwapchainRenderTarget(pass_def.output_color[0]));
    REQUIRE(pass_def.output_depth == noRenderTargetId());
    REQUIRE(pass_def.input_targets.empty());
}

TEST_CASE("rendering pass target JSON parser rejects malformed pass outputs", "[renderingpass]") {
    const auto resolver = RenderTargetNameResolver{[](const std::string &) { return noRenderTargetId(); }};

    PassDefinition pass_def;
    pass_def.name = "bad_pass";

    REQUIRE_THROWS_AS(parsePassOutputTargetsFromJson(pass_def, resolver, nlohmann::json::object()),
                      std::runtime_error);
    REQUIRE_THROWS_AS(parsePassOutputTargetsFromJson(pass_def, resolver, nlohmann::json{{"output", 1}}),
                      std::runtime_error);
    REQUIRE_THROWS_AS(parsePassOutputTargetsFromJson(pass_def, resolver,
                                                     nlohmann::json{{"output", {{"color", "swapchain"}}}}),
                      std::runtime_error);
}

TEST_CASE("pass definition JSON parser builds a fullscreen pass definition", "[renderingpass]") {
    const auto name_resolver = RenderTargetNameResolver{[](const std::string &name) {
        if (name == "half_color") {
            return GlobalRenderTargetId{3};
        }
        return noRenderTargetId();
    }};
    const auto metadata_resolver = RenderTargetMetadataResolver{[](GlobalRenderTargetId id) {
        if (id == GlobalRenderTargetId{3}) {
            return RenderTargetMetadata{"half_color", vk::ImageUsageFlagBits::eColorAttachment |
                                                          vk::ImageUsageFlagBits::eSampled,
                                        vk::Format::eR8G8B8A8Unorm, vk::Extent2D{1280, 720}};
        }
        throw std::runtime_error("unexpected render target metadata lookup");
    }};

    const nlohmann::json pass_json{
        {"name", "debug_texture"},
        {"type", "fullscreen"},
        {"output", {{"color", "half_color"}, {"depth", nullptr}}},
        {"shader", {{"vertex", "fullscreen"}, {"fragment", "debug_texture"}}},
        {"implementation", {{"provider", "fixture.fullscreen"}}},
        {"regions",
         nlohmann::json::array(
             {"region.post.fixture", "region.post"})},
        {"push_constants", "projection_view"},
        {"clear_color", nlohmann::json::array({0.0f, 0.0f, 0.0f, 1.0f})},
    };

    const auto pass_def = parsePassDefinitionFromJson(pass_json, name_resolver, metadata_resolver);

    REQUIRE(pass_def.name == "debug_texture");
    REQUIRE(pass_def.isFullscreen());
    REQUIRE(pass_def.output_color.size() == 1);
    REQUIRE(pass_def.output_color[0] == GlobalRenderTargetId{3});
    REQUIRE(pass_def.output_depth == noRenderTargetId());
    REQUIRE(pass_def.fullscreenInfo().frag_shader.ref == "debug_texture");
    REQUIRE(pass_def.fullscreenInfo().push_constants == FullscreenPushConstantData::eProjectionView);
    REQUIRE(
        pass_def.requested_implementation_provider ==
        std::optional<std::string>{"fixture.fullscreen"});
    REQUIRE_FALSE(pass_def.implementation_selection.has_value());
    REQUIRE(
        pass_def.region_tags ==
        std::vector<std::string>{
            "region.post.fixture", "region.post"});
}

TEST_CASE(
    "pass definition JSON parser keeps implementation selection typed and fullscreen-only",
    "[renderingpass][render-pass][wp200]") {
    const auto name_resolver =
        RenderTargetNameResolver{
            [](const std::string &name) {
                return name == "half_color"
                           ? GlobalRenderTargetId{3}
                           : noRenderTargetId();
            }};
    const auto metadata_resolver =
        RenderTargetMetadataResolver{
            [](GlobalRenderTargetId id) {
                if (id != GlobalRenderTargetId{3}) {
                    throw std::runtime_error(
                        "unexpected render target metadata lookup");
                }
                return RenderTargetMetadata{
                    "half_color",
                    vk::ImageUsageFlagBits::eColorAttachment |
                        vk::ImageUsageFlagBits::eSampled,
                    vk::Format::eR8G8B8A8Unorm,
                    vk::Extent2D{1280, 720}};
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
    REQUIRE_THROWS_WITH(
        parsePassDefinitionFromJson(
            material_json, name_resolver,
            metadata_resolver),
        Catch::Matchers::ContainsSubstring(
            "fullscreen passes only"));

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
    REQUIRE_THROWS_WITH(
        parsePassDefinitionFromJson(
            malformed, name_resolver,
            metadata_resolver),
        Catch::Matchers::ContainsSubstring(
            "containing only a provider string"));

    malformed["implementation"] =
        {{"provider", ""}};
    REQUIRE_THROWS_WITH(
        parsePassDefinitionFromJson(
            malformed, name_resolver,
            metadata_resolver),
        Catch::Matchers::ContainsSubstring(
            "empty or too long"));
    malformed["implementation"] = {
        {"provider",
         std::string(
             RenderPass::maximumProviderNameBytesV1 + 1,
             'x')}};
    REQUIRE_THROWS_WITH(
        parsePassDefinitionFromJson(
            malformed, name_resolver,
            metadata_resolver),
        Catch::Matchers::ContainsSubstring(
            "empty or too long"));
}

TEST_CASE("fullscreen pass JSON parser accepts shader stem references", "[renderingpass]") {
    const nlohmann::json pass_json{
        {"shader", {{"vertex", "shaders/fullscreen"}, {"fragment", "engine://bloom_blur_h"}}},
    };

    const auto info = parseFullscreenPassInfoFromJson(pass_json, "stem_pass");

    REQUIRE(info.vert_shader.ref == "shaders/fullscreen");
    REQUIRE(info.vert_shader.stage == ShaderStage::vertex);
    REQUIRE(info.vert_shader.kind == ShaderReferenceKind::stem);
    REQUIRE_FALSE(info.vert_shader.backend_specific);
    REQUIRE(info.frag_shader.ref == "engine://bloom_blur_h");
    REQUIRE(info.frag_shader.stage == ShaderStage::fragment);
    REQUIRE(info.frag_shader.kind == ShaderReferenceKind::stem);
}

TEST_CASE("pass definition JSON parser rejects invalid pass object", "[renderingpass]") {
    const auto name_resolver = RenderTargetNameResolver{[](const std::string &) { return noRenderTargetId(); }};
    const auto metadata_resolver = RenderTargetMetadataResolver{[](GlobalRenderTargetId) -> RenderTargetMetadata {
        throw std::runtime_error("unexpected render target metadata lookup");
    }};

    REQUIRE_THROWS_AS(parsePassDefinitionFromJson(nlohmann::json::array(), name_resolver, metadata_resolver),
                      std::runtime_error);
}

TEST_CASE("pass sequence JSON parser validates produced input order", "[renderingpass]") {
    const auto name_resolver = RenderTargetNameResolver{[](const std::string &name) {
        if (name == "source_color") {
            return GlobalRenderTargetId{3};
        }
        if (name == "final_color") {
            return GlobalRenderTargetId{4};
        }
        return noRenderTargetId();
    }};
    const auto metadata_resolver = RenderTargetMetadataResolver{[](GlobalRenderTargetId id) {
        if (id == GlobalRenderTargetId{3}) {
            return RenderTargetMetadata{"source_color", vk::ImageUsageFlagBits::eColorAttachment |
                                                            vk::ImageUsageFlagBits::eSampled,
                                        vk::Format::eR8G8B8A8Unorm, vk::Extent2D{1280, 720}};
        }
        if (id == GlobalRenderTargetId{4}) {
            return RenderTargetMetadata{"final_color", vk::ImageUsageFlagBits::eColorAttachment,
                                        vk::Format::eR8G8B8A8Unorm, vk::Extent2D{1280, 720}};
        }
        throw std::runtime_error("unexpected render target metadata lookup");
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

    const auto passes = parsePassSequenceFromJson(pass_set_json, "main", name_resolver, metadata_resolver);

    REQUIRE(passes.size() == 2);
    REQUIRE(passes[0].name == "source");
    REQUIRE(passes[1].input_targets.size() == 1);
    REQUIRE(passes[1].input_targets[0] == GlobalRenderTargetId{3});
}

TEST_CASE("pass sequence JSON parser allows depth output as later input", "[renderingpass]") {
    const auto name_resolver = RenderTargetNameResolver{[](const std::string &name) {
        if (name == "shadow_map") {
            return GlobalRenderTargetId{3};
        }
        if (name == "lit_color") {
            return GlobalRenderTargetId{4};
        }
        return noRenderTargetId();
    }};
    const auto metadata_resolver = RenderTargetMetadataResolver{[](GlobalRenderTargetId id) {
        if (id == GlobalRenderTargetId{3}) {
            return RenderTargetMetadata{"shadow_map", vk::ImageUsageFlagBits::eDepthStencilAttachment |
                                                          vk::ImageUsageFlagBits::eSampled,
                                        vk::Format::eD32Sfloat, vk::Extent2D{2048, 2048}};
        }
        if (id == GlobalRenderTargetId{4}) {
            return RenderTargetMetadata{"lit_color", vk::ImageUsageFlagBits::eColorAttachment,
                                        vk::Format::eR8G8B8A8Unorm, vk::Extent2D{1280, 720}};
        }
        throw std::runtime_error("unexpected render target metadata lookup");
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

    const auto passes = parsePassSequenceFromJson(pass_set_json, "main", name_resolver, metadata_resolver);

    REQUIRE(passes.size() == 2);
    REQUIRE(passes[0].isShadowDepth());
    REQUIRE(passes[1].input_targets == std::vector<GlobalRenderTargetId>{GlobalRenderTargetId{3}});
}

TEST_CASE("pass sequence JSON parser rejects duplicate pass names", "[renderingpass]") {
    const auto name_resolver = RenderTargetNameResolver{[](const std::string &) { return noRenderTargetId(); }};
    const auto metadata_resolver = RenderTargetMetadataResolver{[](GlobalRenderTargetId) -> RenderTargetMetadata {
        throw std::runtime_error("unexpected render target metadata lookup");
    }};

    const nlohmann::json pass_set_json{
        {"passes",
         nlohmann::json::array({
             {{"name", "same"}, {"type", "fullscreen"}, {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
              {"shader", {{"vertex", "fullscreen"}, {"fragment", "one"}}}},
             {{"name", "same"}, {"type", "fullscreen"}, {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
              {"shader", {{"vertex", "fullscreen"}, {"fragment", "two"}}}},
         })},
    };

    REQUIRE_THROWS_AS(parsePassSequenceFromJson(pass_set_json, "main", name_resolver, metadata_resolver),
                      std::runtime_error);
}

TEST_CASE("pass sequence JSON parser rejects malformed pass list", "[renderingpass]") {
    const auto name_resolver = RenderTargetNameResolver{[](const std::string &) { return noRenderTargetId(); }};
    const auto metadata_resolver = RenderTargetMetadataResolver{[](GlobalRenderTargetId) -> RenderTargetMetadata {
        throw std::runtime_error("unexpected render target metadata lookup");
    }};

    REQUIRE_THROWS_AS(parsePassSequenceFromJson(nlohmann::json::object(), "main", name_resolver, metadata_resolver),
                      std::runtime_error);
    REQUIRE_THROWS_AS(parsePassSequenceFromJson(nlohmann::json{{"passes", 1}}, "main", name_resolver,
                                                metadata_resolver),
                      std::runtime_error);
}

TEST_CASE("pass attachment options parser applies explicit color attachment options", "[renderingpass]") {
    PassDefinition pass_def;
    pass_def.name = "bloom_composite";

    const nlohmann::json pass_json{
        {"clear_color", nlohmann::json::array({0.25f, 0.5f, 0.75f, 1.0f})},
        {"color_load_op", "Load"},
        {"color_store_op", "DontCare"},
        {"depth_load_op", "Clear"},
        {"depth_store_op", "Store"},
    };

    parsePassAttachmentOptionsFromJson(pass_def, pass_json);

    REQUIRE(pass_def.clear_color.float32[0] == 0.25f);
    REQUIRE(pass_def.clear_color.float32[1] == 0.5f);
    REQUIRE(pass_def.clear_color.float32[2] == 0.75f);
    REQUIRE(pass_def.clear_color.float32[3] == 1.0f);
    REQUIRE(pass_def.color_load_op == vk::AttachmentLoadOp::eLoad);
    REQUIRE(pass_def.color_store_op == vk::AttachmentStoreOp::eDontCare);
    REQUIRE(pass_def.depth_load_op == vk::AttachmentLoadOp::eClear);
    REQUIRE(pass_def.depth_store_op == vk::AttachmentStoreOp::eStore);
}

TEST_CASE("pass attachment options parser preserves defaults when fields are omitted", "[renderingpass]") {
    PassDefinition pass_def;
    pass_def.name = "geometry";

    parsePassAttachmentOptionsFromJson(pass_def, nlohmann::json::object());

    REQUIRE(pass_def.clear_color.float32[0] == 0.0f);
    REQUIRE(pass_def.clear_color.float32[1] == 0.0f);
    REQUIRE(pass_def.clear_color.float32[2] == 0.0f);
    REQUIRE(pass_def.clear_color.float32[3] == 1.0f);
    REQUIRE(pass_def.color_load_op == vk::AttachmentLoadOp::eClear);
    REQUIRE(pass_def.color_store_op == vk::AttachmentStoreOp::eStore);
}

TEST_CASE("material pass info parser applies explicit material range", "[renderingpass]") {
    PassDefinition pass_def;
    pass_def.name = "geometry";
    pass_def.pass_info = MaterialPassInfo{};

    const nlohmann::json pass_json{
        {"material_range", {{"start", 3}, {"count", 7}}},
    };

    parseMaterialPassInfoFromJson(pass_def, pass_json);

    REQUIRE(pass_def.materialInfo().material_start == 3);
    REQUIRE(pass_def.materialInfo().material_count == 7);
}

TEST_CASE("material pass screen inputs resolve named typed targets and reject mismatches",
          "[renderingpass][material-screen-input][rpe6b1]") {
    const RenderTargetNameResolver names{[](const std::string &name) {
        if (name == "opaque_color") return GlobalRenderTargetId{10};
        if (name == "opaque_depth") return GlobalRenderTargetId{11};
        return noRenderTargetId();
    }};
    const RenderTargetMetadataResolver metadata{
        [](GlobalRenderTargetId target) {
            if (target == GlobalRenderTargetId{10}) {
                return RenderTargetMetadata{
                    "opaque_color",
                    vk::ImageUsageFlagBits::eTransferDst |
                        vk::ImageUsageFlagBits::eSampled,
                    vk::Format::eR16G16B16A16Sfloat,
                    vk::Extent2D{1280, 720}};
            }
            if (target == GlobalRenderTargetId{11}) {
                return RenderTargetMetadata{
                    "opaque_depth",
                    vk::ImageUsageFlagBits::eTransferDst |
                        vk::ImageUsageFlagBits::eSampled,
                    vk::Format::eD32Sfloat, vk::Extent2D{1280, 720}};
            }
            throw std::runtime_error("unexpected target");
        }};

    PassDefinition pass;
    pass.name = "forward_transparent";
    pass.pass_info = MaterialPassInfo{};
    parseMaterialPassScreenInputsFromJson(
        pass,
        nlohmann::json{{"screen_inputs",
                        {{"opaque_color", "opaque_color"},
                         {"opaque_depth", "opaque_depth"},
                         {"scene_depth", "opaque_depth"},
                         {"linear_view_depth", "opaque_depth"}}}},
        names, metadata);
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

    PassDefinition mismatch;
    mismatch.name = "bad_forward";
    mismatch.pass_info = MaterialPassInfo{};
    REQUIRE_THROWS_WITH(
        parseMaterialPassScreenInputsFromJson(
            mismatch,
            nlohmann::json{{"screen_inputs",
                            {{"opaque_color", "opaque_depth"}}}},
            names, metadata),
        Catch::Matchers::ContainsSubstring("opaque_color") &&
            Catch::Matchers::ContainsSubstring("source type"));

    PassDefinition unknown;
    unknown.name = "unknown_forward";
    unknown.pass_info = MaterialPassInfo{};
    REQUIRE_THROWS_WITH(
        parseMaterialPassScreenInputsFromJson(
            unknown,
            nlohmann::json{{"screen_inputs",
                            {{"history_magic", "opaque_color"}}}},
            names, metadata),
        Catch::Matchers::ContainsSubstring("history_magic"));
}

TEST_CASE("material pass surface resources resolve the feature-owned directional shadow contract",
          "[renderingpass][material-surface-resource][wp205]") {
    const RenderTargetNameResolver names{[](const std::string &name) {
        if (name == "shadow_map") return GlobalRenderTargetId{20};
        if (name == "scene_color") return GlobalRenderTargetId{21};
        return noRenderTargetId();
    }};
    const RenderTargetMetadataResolver metadata{
        [](GlobalRenderTargetId target) {
            if (target == GlobalRenderTargetId{20}) {
                return RenderTargetMetadata{
                    "shadow_map",
                    vk::ImageUsageFlagBits::eDepthStencilAttachment |
                        vk::ImageUsageFlagBits::eSampled,
                    vk::Format::eD32Sfloat,
                    vk::Extent2D{2048, 2048}};
            }
            if (target == GlobalRenderTargetId{21}) {
                return RenderTargetMetadata{
                    "scene_color",
                    vk::ImageUsageFlagBits::eColorAttachment |
                        vk::ImageUsageFlagBits::eSampled,
                    vk::Format::eR16G16B16A16Sfloat,
                    vk::Extent2D{1280, 720}};
            }
            throw std::runtime_error("unexpected target");
        }};

    PassDefinition pass;
    pass.name = "forward_opaque";
    pass.pass_info = MaterialPassInfo{};
    parseMaterialPassSurfaceResourcesFromJson(
        pass,
        nlohmann::json{
            {"surface_resources",
             {{"directional_shadow", "shadow_map"}}}},
        names, metadata);

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
            MaterialPassInputViewPolicy::shared_2d);
    REQUIRE(binding.contract.fallback ==
            MaterialPassInputFallback::fully_lit);
    REQUIRE(binding.contract.relation);
    REQUIRE(binding.contract.relation->light_index == 0);
    REQUIRE(binding.contract.relation->transform ==
            "pelican.light.shadow_view_projection@1");
    REQUIRE(pass.input_targets ==
            std::vector<GlobalRenderTargetId>{
                GlobalRenderTargetId{20}});

    PassDefinition wrong_type;
    wrong_type.name = "bad_forward";
    wrong_type.pass_info = MaterialPassInfo{};
    REQUIRE_THROWS_WITH(
        parseMaterialPassSurfaceResourcesFromJson(
            wrong_type,
            nlohmann::json{
                {"surface_resources",
                 {{"directional_shadow", "scene_color"}}}},
            names, metadata),
        Catch::Matchers::ContainsSubstring(
            "directional_shadow") &&
            Catch::Matchers::ContainsSubstring("source type"));

    PassDefinition user_owned;
    user_owned.name = "bad_forward";
    user_owned.pass_info = MaterialPassInfo{};
    REQUIRE_THROWS_WITH(
        parseMaterialPassSurfaceResourcesFromJson(
            user_owned,
            nlohmann::json{
                {"surface_resources",
                 {{"opaque_depth", "shadow_map"}}}},
            names, metadata),
        Catch::Matchers::ContainsSubstring("opaque_depth") &&
            Catch::Matchers::ContainsSubstring("not feature-owned"));
}

TEST_CASE("material pass contracts parse and filter independently of local pass ids",
          "[renderingpass][material-routing]") {
    REQUIRE(forwardMaterialPassColorAttachmentFormat ==
            vk::Format::eR16G16B16A16Sfloat);
    PassDefinition pass_def;
    pass_def.name = "forward_opaque";
    pass_def.pass_info = MaterialPassInfo{};
    parseMaterialPassInfoFromJson(
        pass_def, nlohmann::json{{"material_contract", "forward_opaque_v1"}});
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
    REQUIRE_THROWS_WITH(
        parseMaterialPassInfoFromJson(
            pass_def, nlohmann::json{{"material_contract", "unknown_v1"}}),
        Catch::Matchers::ContainsSubstring("Unknown material_contract"));

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

TEST_CASE("material pass info parser preserves defaults when material range is omitted", "[renderingpass]") {
    PassDefinition pass_def;
    pass_def.name = "geometry";
    pass_def.pass_info = MaterialPassInfo{};

    parseMaterialPassInfoFromJson(pass_def, nlohmann::json::object());

    REQUIRE(pass_def.materialInfo().material_start == 0);
    REQUIRE(pass_def.materialInfo().material_count == 0);
}

TEST_CASE("material pass info parser rejects malformed material range", "[renderingpass]") {
    PassDefinition pass_def;
    pass_def.name = "geometry";
    pass_def.pass_info = MaterialPassInfo{};

    const nlohmann::json pass_json{
        {"material_range", 1},
    };

    REQUIRE_THROWS_AS(parseMaterialPassInfoFromJson(pass_def, pass_json), std::runtime_error);
}

TEST_CASE("rendering pass JSON helpers reject malformed values", "[renderingpass]") {
    REQUIRE_THROWS_AS(stringToFormat("UNKNOWN"), std::runtime_error);
    REQUIRE_THROWS_AS(stringToUsageFlags({}), std::runtime_error);
    REQUIRE_THROWS_AS(makePassInfo("compute"), std::runtime_error);
    REQUIRE_THROWS_AS(stringToFullscreenPushConstantData("camera"), std::runtime_error);
    REQUIRE_THROWS_AS(stringToLoadOp("keep"), std::runtime_error);
    REQUIRE_THROWS_AS(stringToStoreOp("load"), std::runtime_error);

    REQUIRE_THROWS_AS(validateName("", "Pass"), std::runtime_error);
    REQUIRE_THROWS_AS(parseStringField(nlohmann::json{{"name", 1}}, "name", "test"), std::runtime_error);
    REQUIRE_THROWS_AS(parseFloatField(nlohmann::json{{"scale", "large"}}, "scale", "test"), std::runtime_error);
    REQUIRE_THROWS_AS(parseStringArrayField(nlohmann::json{{"usage", nlohmann::json::array({"SAMPLED", 1})}},
                                            "usage", "test"),
                      std::runtime_error);
    REQUIRE_THROWS_AS(parseUint32Field(nlohmann::json{{"count", -1}}, "count", "test"), std::runtime_error);
    REQUIRE_THROWS_AS(jsonToClearColor(nlohmann::json::array({1.0f, 0.0f, 0.0f})), std::runtime_error);
    REQUIRE_THROWS_AS(parseRenderTargetDefinitionsFromJson(nlohmann::json{{"render_targets", 1}}),
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
                VulkanPhysicalAttachmentAspect::color,
            .load_op =
                VulkanPhysicalAttachmentLoadOp::discard,
            .store_op =
                VulkanPhysicalAttachmentStoreOp::discard,
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
        vk::AttachmentLoadOp::eClear);
    REQUIRE((
        physical.colorAttachmentOperations(0) ==
        PassAttachmentOperations{
            vk::AttachmentLoadOp::eDontCare,
            vk::AttachmentStoreOp::eDontCare,
        }));

    plan.attachments.front().aspect =
        VulkanPhysicalAttachmentAspect::depth;
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
                        vk::ImageUsageFlagBits::
                            eColorAttachment,
                    .format =
                        vk::Format::eR8G8B8A8Unorm,
                    .extent = {64, 64},
                };
            }
            if (id == lit) {
                return RenderTargetMetadata{
                    .name = "lit",
                    .usage =
                        vk::ImageUsageFlagBits::
                            eColorAttachment,
                    .format =
                        vk::Format::
                            eR16G16B16A16Sfloat,
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
        vk::ClearColorValue{
            std::array{0.25f, 0.5f, 0.75f, 1.0f}};

    PassDefinition lighting;
    lighting.name = "lighting";
    lighting.pass_info = MaterialPassInfo{};
    lighting.input_targets = {gbuffer};
    lighting.input_target_history = {false};
    lighting.output_color = {lit};
    lighting.clear_color =
        vk::ClearColorValue{
            std::array{0.0f, 0.0f, 0.0f, 1.0f}};

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
            .load_op =
                VulkanPhysicalAttachmentLoadOp::
                    discard,
            .store_op =
                VulkanPhysicalAttachmentStoreOp::
                    discard,
        },
        {
            .node = "lighting",
            .logical_resource = "lit",
            .load_op =
                VulkanPhysicalAttachmentLoadOp::
                    clear,
            .store_op =
                VulkanPhysicalAttachmentStoreOp::
                    store,
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
        std::vector<GlobalRenderTargetId>{
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
    const auto expected_scope_operations =
        std::vector<PassAttachmentOperations>{
            {
                vk::AttachmentLoadOp::eDontCare,
                vk::AttachmentStoreOp::eDontCare,
            },
            {
                vk::AttachmentLoadOp::eClear,
                vk::AttachmentStoreOp::eStore,
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
        std::vector<std::array<float, 4>>{
            {0.25f, 0.5f, 0.75f, 1.0f},
            {0.0f, 0.0f, 0.0f, 1.0f},
        };
    REQUIRE(
        geometry_contract.scope_color_clear_values ==
        expected_scope_clear_values);
    REQUIRE(
        lighting_contract.scope_color_clear_values ==
        geometry_contract.scope_color_clear_values);

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
                    vk::ImageUsageFlagBits::
                        eColorAttachment,
                .format =
                    vk::Format::eR8G8B8A8Unorm,
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
        vk::AttachmentLoadOp::eLoad;

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
            .load_op =
                VulkanPhysicalAttachmentLoadOp::
                    clear,
            .store_op =
                VulkanPhysicalAttachmentStoreOp::
                    store,
        },
        {
            .node = "overlay",
            .logical_resource =
                "scene_color",
            .load_op =
                VulkanPhysicalAttachmentLoadOp::
                    load,
            .store_op =
                VulkanPhysicalAttachmentStoreOp::
                    store,
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
            {
                vk::AttachmentLoadOp::eClear,
                vk::AttachmentStoreOp::eStore,
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
    invalid.attachments[1].load_op =
        VulkanPhysicalAttachmentLoadOp::
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
    "view-family scheduler follows dependency-verified physical order",
    "[renderingpass][view-execution][schedule][reorder]") {
    const std::vector<FrameGraphExecutionNode> nodes{
        {.name = "A"},
        {.name = "B"},
        {.name = "C"},
        {.name = "D"},
    };
    VulkanTargetPlan plan;
    plan.view_execution_plan.view_count = 1;
    plan.scopes = {
        {
            .id = "reordered",
            .nodes = {"C"},
        },
        {
            .id = "non_contiguous_fused",
            .nodes = {"A", "D"},
            .single_rendering_instance = true,
        },
        {
            .id = "tail",
            .nodes = {"B"},
        },
    };

    const auto schedule =
        buildLogicalFrameViewFamilySchedule(
            nodes, plan, 1);
    REQUIRE(schedule.size() == 4);
    REQUIRE(
        std::vector<std::size_t>{
            schedule[0].node_index,
            schedule[1].node_index,
            schedule[2].node_index,
            schedule[3].node_index} ==
        std::vector<std::size_t>{2, 0, 3, 1});
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
