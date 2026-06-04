#include "../src/core/renderingpass/renderingpassjsonhelpers.hpp"
#include "../src/core/renderingpass/renderingpassruntimecompiler.hpp"
#include "../src/core/renderingpass/rendertargetjsonparser.hpp"
#include <catch2/catch_test_macros.hpp>
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

    const auto definitions = parseRenderTargetDefinitionsFromJson(config, vk::Extent2D{1280, 720});

    REQUIRE(definitions.size() == 1);
    REQUIRE(definitions[0].name == "half_color");
    REQUIRE(definitions[0].extent.width == 640);
    REQUIRE(definitions[0].extent.height == 360);
    REQUIRE(definitions[0].format == vk::Format::eR8Unorm);
    REQUIRE(static_cast<bool>(definitions[0].usage & vk::ImageUsageFlagBits::eColorAttachment));
    REQUIRE(static_cast<bool>(definitions[0].usage & vk::ImageUsageFlagBits::eSampled));
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
    REQUIRE_THROWS_AS(parseRenderTargetDefinitionsFromJson(nlohmann::json{{"render_targets", 1}},
                                                           vk::Extent2D{1280, 720}),
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
