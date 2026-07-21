#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/imageloader.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/core/log.hpp"
#include "../src/core/material/materialcontainer.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/core/vkcore/rendertarget.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include "ktx2_test_writer.hpp"

namespace Pelican {

namespace {

std::filesystem::path makeTempProjectDir() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path() / ("pelican_headless_render_" + std::to_string(suffix));
    std::filesystem::create_directories(dir);
    return dir;
}

void writeTextFile(const std::filesystem::path &path, const std::string &contents) {
    std::ofstream file{path, std::ios::binary};
    file << contents;
}

nlohmann::json makeProjectConfig(const std::filesystem::path &scene_path, const std::filesystem::path &asset_path) {
    return nlohmann::json{
        {"basic_config",
         {
             {"window_size", {{"width", 32}, {"height", 32}}},
             {"scene_data_json", scene_path.generic_string()},
             {"asset_data_json", asset_path.generic_string()},
         }},
    };
}

void renderClearFrame(RenderTarget &render_target, vk::ClearColorValue clear_color) {
    auto frame = render_target.render_begin();

    vk::RenderingAttachmentInfo color_attachment;
    color_attachment.imageView = frame.color_attachment;
    color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_attachment.loadOp = vk::AttachmentLoadOp::eClear;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;
    color_attachment.clearValue.color = clear_color;

    vk::RenderingInfo rendering_info;
    rendering_info.renderArea = vk::Rect2D{{0, 0}, frame.extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(color_attachment);

    frame.cmd_buf.beginRendering(rendering_info);
    frame.cmd_buf.endRendering();

    render_target.render_end();
}

} // namespace

TEST_CASE("headless render target renders and reads back RGBA8 frames", "[headless][render]") {
    setupLogger();
    std::filesystem::path temp_dir;

    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        const auto scene_path = temp_dir / "scene.json";
        const auto asset_path = temp_dir / "assets.json";
        writeTextFile(scene_path, "{}");
        writeTextFile(asset_path, "{}");

        GET_MODULE(ProjectSource).setSourceByData(makeProjectConfig(scene_path, asset_path).dump());

        auto &launch_config = GET_MODULE(EngineLaunchConfig);
        launch_config.headless = true;
        launch_config.headless_extent = vk::Extent2D{32, 32};
        launch_config.headless_frames = 3;

        auto &engine_time = GET_MODULE(EngineTime);
        engine_time.setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);

        auto &render_target = GET_MODULE(RenderTarget);
        const std::array<uint8_t, 4> dual_use_pixel{188, 188, 188, 255};
        const auto dual_use_texture = GET_MODULE(MaterialContainer).registerTexture(
            vk::Extent3D{1, 1, 1}, dual_use_pixel.data());
        const auto [data_view, color_view] =
            GET_MODULE(MaterialContainer).textureViewsForTesting(dual_use_texture);
        REQUIRE(data_view);
        REQUIRE(color_view);
        REQUIRE(data_view != color_view);
        const auto ktx_bytes = TestKtx2::makeRgba8Srgb188();
        const auto ktx_loaded = loadImageMemory(ktx_bytes, "headless-known-188.ktx2");
        REQUIRE(static_cast<unsigned>(ktx_loaded.pixels.front()) == 188);
        const auto ktx_texture = GET_MODULE(MaterialContainer).registerTexture(
            ktx_loaded, "headless-known-188.ktx2");
        const auto [ktx_data_view, ktx_color_view] =
            GET_MODULE(MaterialContainer).textureViewsForTesting(ktx_texture);
        REQUIRE(ktx_data_view);
        REQUIRE(ktx_color_view);
        REQUIRE(ktx_data_view != ktx_color_view);
        REQUIRE(GET_MODULE(MaterialContainer).textureMipLevelsForTesting(ktx_texture) == 2);
        const std::array clears{
            vk::ClearColorValue{std::array{1.0f, 0.0f, 0.0f, 1.0f}},
            vk::ClearColorValue{std::array{0.0f, 0.0f, 1.0f, 1.0f}},
            vk::ClearColorValue{std::array{0.0f, 1.0f, 0.0f, 1.0f}},
        };

        for (const auto clear : clears) {
            engine_time.advance();
            renderClearFrame(render_target, clear);
        }

        const auto pixels = render_target.readbackLastFrameRGBA8();
        const auto extent = render_target.getExtent();
        REQUIRE(extent.width == 32);
        REQUIRE(extent.height == 32);
        REQUIRE(pixels.size() == static_cast<size_t>(extent.width) * extent.height * 4);
        REQUIRE(engine_time.frameIndex() == 3);

        size_t mismatched_pixels = 0;
        for (size_t i = 0; i < pixels.size(); i += 4) {
            if (pixels[i + 0] != 0 || pixels[i + 1] != 255 || pixels[i + 2] != 0 || pixels[i + 3] != 255) {
                ++mismatched_pixels;
            }
        }
        REQUIRE(mismatched_pixels == 0);

        renderClearFrame(render_target,
                         vk::ClearColorValue{std::array{0.5f, 0.5f, 0.5f, 0.25f}});
        const auto known_value = render_target.readbackLastFrameRGBA8();
        size_t known_value_mismatches = 0;
        for (size_t i = 0; i < known_value.size(); i += 4) {
            if (known_value[i + 0] != 188 || known_value[i + 1] != 188 ||
                known_value[i + 2] != 188 || known_value[i + 3] != 64) {
                ++known_value_mismatches;
            }
        }
        REQUIRE(known_value_mismatches == 0);

        GET_MODULE(VulkanManageCore).waitIdle();
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &ex) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(temp_dir);
        }
        SKIP(std::string{"Vulkan headless rendering unavailable: "} + ex.what());
    }
}

TEST_CASE("hybrid_v1 preset registers and renders a headless frame",
          "[headless][render][hybrid]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    std::filesystem::path temp_dir;

    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        const auto scene_path = temp_dir / "scene.json";
        const auto asset_path = temp_dir / "assets.json";
        writeTextFile(
            scene_path,
            R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(asset_path, R"json({"models":[]})json");
        writeTextFile(temp_dir / "hybrid.json",
                      nlohmann::json{{"pipeline", {{"preset",
                          "engine://render_pipelines/hybrid_v1.json"}}}}.dump(2));

        auto project = makeProjectConfig("scene.json", "assets.json");
        project["basic_config"]["default_scene_id"] = "default_scene";
        project["basic_config"]["rendering_config_json"] = "hybrid.json";
        project["basic_config"]["default_rendering_pass"] = "main_render";
        GET_MODULE(ProjectSource).setSourceByData(project.dump());
        GET_MODULE(PathResolver).setup(temp_dir, false);

        auto &launch_config = GET_MODULE(EngineLaunchConfig);
        launch_config.headless = true;
        launch_config.headless_extent = vk::Extent2D{32, 32};
        launch_config.headless_frames = 1;
        GET_MODULE(EngineTime).setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);

        auto &renderer = GET_MODULE(Renderer);
        auto &standard = GET_MODULE(StandardMaterialResource);
        const auto surface_reference =
            std::string{"engine://surfaces/openpbr/opaque_single.surface"};
        const auto surface = parseSurfaceFormat(
            engineResourceOrThrow("surfaces/openpbr/opaque_single.surface"),
            surface_reference);
        MaterialSurfaceCatalog surfaces{{surface_reference, surface}};
        const auto material_document = parseMaterialFormatJson(
            nlohmann::json::parse(R"json({
              "schema":"pelican.material","version":1,"materials":[{
                "name":"hybrid_coat","surface":"engine://surfaces/openpbr/opaque_single.surface",
                "values":{"coat_weight":0.5},
                "routing":{"alpha_mode":"opaque","double_sided":false}
              }]
            })json"),
            surfaces);
        const auto lowered = lowerMaterial(material_document.materials.front(), surface);
        REQUIRE(lowered.route == MaterialRouteClass::forward_opaque);
        const auto shaders = GET_MODULE(ShaderLibrary).loadFromSurfaceForMaterial(
            surface, surface_reference, lowered);
        MaterialInfo material{
            .vert_shader = shaders.vertex,
            .frag_shader = shaders.fragment,
            .base_color_texture = standard.whiteTexture(),
            .metallic_roughness_texture = standard.metallicRoughnessDefaultTexture(),
            .normal_texture = standard.normalDefaultTexture(),
            .emissive_texture = standard.emissiveDefaultTexture(),
        };
        applyLoweredMaterialForRoute(material, lowered);
        const auto material_id = GET_MODULE(MaterialContainer).registerMaterial(
            std::move(material));
        REQUIRE(isValidMaterialId(material_id));
        renderer.render();
        GET_MODULE(VulkanManageCore).waitIdle();

        const auto pixels = GET_MODULE(RenderTarget).readbackLastFrameRGBA8();
        REQUIRE(pixels.size() == 32u * 32u * 4u);
        const auto plan = renderer.currentFramePlanJson();
        REQUIRE(plan.dump().find("deferred_geometry") != std::string::npos);
        REQUIRE(plan.dump().find("forward_transparent") != std::string::npos);
        REQUIRE(plan.dump().find("scene_present") != std::string::npos);

        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &ex) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(temp_dir);
        }
        SKIP(std::string{"Vulkan hybrid rendering unavailable: "} + ex.what());
    }
#endif
}

} // namespace Pelican
