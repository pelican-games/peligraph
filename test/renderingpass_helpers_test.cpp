#include "../src/core/renderingpass/fullscreenpassinfojsonparser.hpp"
#include "../src/core/renderingpass/materialpassinfojsonparser.hpp"
#include "../src/core/renderingpass/passattachmentoptionsjsonparser.hpp"
#include "../src/core/renderingpass/passdefinitionjsonparser.hpp"
#include "../src/core/renderingpass/passinfojsonparser.hpp"
#include "../src/core/renderingpass/passsequencejsonparser.hpp"
#include "../src/core/renderingpass/renderingpassjsonhelpers.hpp"
#include "../src/core/renderingpass/renderingpassruntimecompiler.hpp"
#include "../src/core/renderingpass/renderingpasstargetjsonparser.hpp"
#include "../src/core/renderingpass/rendertargetmetadataresolver.hpp"
#include "../src/core/renderingpass/rendertargetnameresolver.hpp"
#include "../src/core/renderingpass/rendertargetjsonparser.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <variant>

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
             {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
         }})},
    };

    const auto definitions = parseRenderTargetDefinitionsFromJson(config);

    REQUIRE(definitions.size() == 1);
    REQUIRE(definitions[0].name == "half_color");
    REQUIRE(definitions[0].extent_scale == 0.5f);
    REQUIRE_FALSE(definitions[0].fixed_extent.has_value());
    REQUIRE(definitions[0].format == vk::Format::eR8Unorm);
    REQUIRE(static_cast<bool>(definitions[0].usage & vk::ImageUsageFlagBits::eColorAttachment));
    REQUIRE(static_cast<bool>(definitions[0].usage & vk::ImageUsageFlagBits::eSampled));
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

} // namespace Pelican
