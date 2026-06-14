#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/rendertarget.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

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

        GET_MODULE(VulkanManageCore).waitIdle();
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &ex) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(temp_dir);
        }
        SKIP(std::string{"Vulkan headless rendering unavailable: "} + ex.what());
    }
}

} // namespace Pelican
