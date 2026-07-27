#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/renderer/debugtext.hpp"
#include "../src/core/renderer/frameresources.hpp"
#include "../src/core/renderer/uicontainer.hpp"
#include "../src/core/renderer/uirenderer.hpp"
#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/ui/module.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/rendertarget.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Pelican {
namespace {

constexpr vk::Extent2D fixture_extent{96, 80};
constexpr double fixture_scale = 2.0;
constexpr std::array<std::uint8_t, 4> fixture_color{17, 73, 201, 149};

struct LabelFixture {
    std::string id;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::string text;
};

class TempDirectory {
    std::filesystem::path path_;

  public:
    TempDirectory() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("pelican_debugtext_ui_compat_" + std::to_string(suffix));
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() { std::filesystem::remove_all(path_); }
    const std::filesystem::path &path() const noexcept { return path_; }
};

std::vector<LabelFixture> labelFixtures() {
    std::string controls{"A\r\nB\t"};
    controls.push_back('\x01'); // Unsupported byte: both paths must use '?'.
    controls.push_back('~');
    return {
        {"controls", 4, 5, std::move(controls)},
        {"clip_left", -2, 24, "L"},
        {"clip_top", 20, -2, "T"},
        {"clip_right", 46, 24, "R"},
        {"clip_bottom", 20, 38, "B"},
    };
}

nlohmann::json labelJson(const LabelFixture &label) {
    return {
        {"id", label.id},
        {"type", "label"},
        {"text", label.text},
        {"text_color", fixture_color},
        {"layout",
         {{"x", {{"mode", "content"}}},
          {"y", {{"mode", "content"}}},
          {"offsets", {label.x, label.y, 0, 0}}}},
    };
}

void writeUiFixture(const std::filesystem::path &root,
                    const std::vector<LabelFixture> &labels) {
    auto children = nlohmann::json::array();
    for (const auto &label : labels) children.push_back(labelJson(label));
    const auto document = nlohmann::json{
        {"schema", "pelican.ui"},
        {"version", 1},
        {"key", "wp169_debugtext_ui_compat"},
        {"revision", "wp169"},
        {"root", {{"id", "root"}, {"type", "panel"}, {"children", children}}},
    };
    std::ofstream stream{root / "ui.json", std::ios::binary};
    stream << document.dump(2);
    if (!stream) throw std::runtime_error("failed to write WP169 UI fixture");

    std::ofstream scene{root / "scene.json", std::ios::binary};
    scene << R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json";
    if (!scene) throw std::runtime_error("failed to write WP169 scene fixture");

    std::ofstream assets{root / "assets.json", std::ios::binary};
    assets << R"json({"models":[]})json";
    if (!assets) throw std::runtime_error("failed to write WP169 asset fixture");
}

FrameUniformData frameData(vk::Extent2D extent) {
    FrameUniformData data;
    data.resolution = glm::vec4{static_cast<float>(extent.width),
                                static_cast<float>(extent.height),
                                1.0f / static_cast<float>(extent.width),
                                1.0f / static_cast<float>(extent.height)};
    return data;
}

void selectFrameResources(const FrameRenderContext &frame, FrameResources &resources) {
    resources.selectView(frame.in_flight_frame_index, 0);
    resources.update(frameData(frame.extent));
}

std::vector<std::uint8_t> renderDebugText(
    RenderTarget &target, DebugText &debug_text, PassId pass_id,
    FrameResources &frame_resources, const std::vector<LabelFixture> &labels) {
    auto begun = target.beginFrame(nullptr);
    REQUIRE(begun.frame.has_value());
    auto token = std::move(*begun.frame);
    const auto frame = token.context();
    selectFrameResources(frame, frame_resources);
    const auto color = glm::vec4{
        fixture_color[0] / 255.0f, fixture_color[1] / 255.0f,
        fixture_color[2] / 255.0f, fixture_color[3] / 255.0f};
    for (const auto &label : labels) {
        debug_text.text(static_cast<int>(label.x * fixture_scale),
                        static_cast<int>(label.y * fixture_scale), label.text,
                        color, static_cast<int>(fixture_scale));
    }

    vk::RenderingAttachmentInfo attachment;
    attachment.imageView = frame.color_attachment;
    attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    attachment.loadOp = vk::AttachmentLoadOp::eClear;
    attachment.storeOp = vk::AttachmentStoreOp::eStore;
    attachment.clearValue.color =
        vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 0.0f}};
    vk::RenderingInfo rendering;
    rendering.renderArea = vk::Rect2D{{0, 0}, frame.extent};
    rendering.layerCount = 1;
    rendering.setColorAttachments(attachment);
    frame.cmd_buf.beginRendering(rendering);
    debug_text.render(frame.cmd_buf, pass_id, frame.extent, frame_resources);
    frame.cmd_buf.endRendering();
    target.submit(std::move(token));
    return target.readbackLastFrameRGBA8();
}

std::vector<std::uint8_t> renderUi(RenderTarget &target, UiRenderer &renderer,
                                   const UiRendererDependencies &dependencies,
                                   FrameResources &frame_resources) {
    auto begun = target.beginFrame(nullptr);
    REQUIRE(begun.frame.has_value());
    auto token = std::move(*begun.frame);
    const auto frame = token.context();
    selectFrameResources(frame, frame_resources);
    renderer.render(
        frame.cmd_buf,
        UiDrawRequest{
            .target_view = frame.color_attachment,
            .target_extent = frame.extent,
            .target_format = target.getSwapchainFormat(),
            .load_op = vk::AttachmentLoadOp::eClear,
            .store_op = vk::AttachmentStoreOp::eStore,
            .clear_color =
                vk::ClearColorValue{
                    std::array{0.0f, 0.0f, 0.0f, 0.0f}},
            .ui_scale = fixture_scale,
        },
        dependencies);
    target.submit(std::move(token));
    return target.readbackLastFrameRGBA8();
}

} // namespace

TEST_CASE("production DebugText and UI framebuffers are byte-exact",
          "[wp169][debug-text][ui][gpu][byte-exact]") {
    setupLogger();
    TempDirectory temp;
    const auto labels = labelFixtures();
    writeUiFixture(temp.path(), labels);

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(temp.path(), false);
    GET_MODULE(ProjectSource).setSourceByData(
        nlohmann::json{{"basic_config",
                        {{"ui_config_json", "ui.json"},
                         {"scene_data_json", "scene.json"},
                         {"asset_data_json", "assets.json"}}}}
            .dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = fixture_extent;

    try {
        (void)GET_MODULE(VulkanManageCore);
    } catch (const std::exception &error) {
        const std::string message = error.what();
        if (message.find("No suitable Vulkan physical device found") != std::string::npos) {
            SKIP("WP169 production framebuffer A/B requires a Vulkan device: " << message);
        }
        throw;
    }

    auto &target = GET_MODULE(RenderTarget);
    auto &frame_resources = GET_MODULE(FrameResources);
    auto &shader_library = GET_MODULE(ShaderLibrary);
    const auto vert_bytes = engineResourceOrThrow("debug_text.vert.spv");
    const auto frag_bytes = engineResourceOrThrow("debug_text.frag.spv");
    const auto vert = shader_library.loadFromBytes(
        vert_bytes.size(), vert_bytes.data(), "engine://debug_text.vert.spv");
    const auto frag = shader_library.loadFromBytes(
        frag_bytes.size(), frag_bytes.data(), "engine://debug_text.frag.spv");
    auto &debug_text = GET_MODULE(DebugText);
    const auto debug_pass =
        debug_text.registerPass(target.getSwapchainFormat(), vert, frag);

    auto &ui_module = GET_MODULE(ui::UiModule);
    auto &ui_container = GET_MODULE(UIContainer);
    auto &ui_renderer = GET_MODULE(UiRenderer);
    const UiRendererDependencies ui_dependencies{ui_container, ui_module,
                                                  frame_resources};

    const auto debug_bytes = renderDebugText(target, debug_text, debug_pass,
                                             frame_resources, labels);
    const auto ui_bytes =
        renderUi(target, ui_renderer, ui_dependencies, frame_resources);

    REQUIRE(debug_bytes.size() ==
            static_cast<std::size_t>(fixture_extent.width) * fixture_extent.height * 4);
    REQUIRE(ui_bytes.size() == debug_bytes.size());
    const auto nonzero = std::count_if(debug_bytes.begin(), debug_bytes.end(),
                                       [](auto value) { return value != 0; });
    REQUIRE(nonzero > 0);
    const auto mismatch = std::mismatch(debug_bytes.begin(), debug_bytes.end(),
                                        ui_bytes.begin(), ui_bytes.end());
    if (mismatch.first != debug_bytes.end()) {
        const auto offset = static_cast<std::size_t>(mismatch.first - debug_bytes.begin());
        INFO("first framebuffer mismatch byte=" << offset
             << " debug=" << static_cast<unsigned>(*mismatch.first)
             << " ui=" << static_cast<unsigned>(*mismatch.second));
    }
    REQUIRE(debug_bytes == ui_bytes);
    REQUIRE(ui_renderer.hasGpuBuffersForTesting());
    GET_MODULE(VulkanManageCore).waitIdle();
}

} // namespace Pelican
