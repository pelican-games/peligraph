#include <catch2/catch_test_macros.hpp>

#include "../src/core/imgui/imguiruntime.hpp"
#include "../src/core/imgui/imguisystem.hpp"
#include "../src/core/renderingpass/frameplanner.hpp"
#include "render_graph_ir_test_support.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/core/vkcore/rendertarget.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/log.hpp"
#include "renderer_testaccess.hpp"
#include <filesystem>
#include <fstream>

#include <algorithm>
#include <nlohmann/json.hpp>

using namespace Pelican;

namespace {

nlohmann::json canonicalGraph() {
    return {
        {"render_targets", nlohmann::json::array({
             // RC-3c1b: a config image is a whole declaration (the IR reads
             // it as the render target parser does).
             {{"name", "display"},
              {"extent_scale", 1.0},
              {"format", "B8G8R8A8_SRGB"},
              {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})}},
         })},
        {"rendering_passes", nlohmann::json::array({
             {{"name", "main"},
              {"passes", nlohmann::json::array({
                   {{"name", "display_source"},
                    {"type", "ui"},
                    {"output", {{"color", "display"}, {"depth", nullptr}}}},
                   {{"name", "output_transform"},
                    {"type", "output_transform"},
                    {"input", nlohmann::json::array({"display"})},
                    {"after", nlohmann::json::array({"display_source"})},
                    {"output", {{"color", "swapchain"}, {"depth", nullptr}}}},
               })}},
         })},
    };
}

std::vector<std::string> planOrder(const nlohmann::json &config) {
    auto definitions = TestSupport::frameGraphsOf(config);
    REQUIRE(definitions.size() == 1);
    return framePlanOrder(planFrameGraph(definitions.front()));
}

} // namespace

TEST_CASE("ImGui Vulkan callback rejects every non-success result",
          "[imgui][vulkan]") {
    REQUIRE_NOTHROW(internal::checkImGuiVulkanResult(VK_SUCCESS));
    REQUIRE_THROWS_AS(
        internal::checkImGuiVulkanResult(VK_ERROR_OUT_OF_DEVICE_MEMORY),
        std::runtime_error);
    REQUIRE_THROWS_AS(internal::checkImGuiVulkanResult(VK_TIMEOUT),
                      std::runtime_error);
}

TEST_CASE("deterministic drivers execute zero ImGui callbacks and keep input unchanged",
          "[imgui][isolation]") {
    std::vector<EngineLaunchConfig> isolated(5);
    isolated[0].headless = true;
    isolated[1].rpc = true;
    isolated[2].input_replay = true;
    isolated[3].golden_mode = true;
    isolated[4].xr_active = true;

    for (const auto &config : isolated) {
        CAPTURE(config.headless, config.rpc, config.input_replay, config.golden_mode,
                config.xr_active);
        auto graph = canonicalGraph();
        InputStateCore input;
        input.queueButtonEvent(KeyCode::A, true);
        input.queueButtonEvent(KeyCode::MouseLeft, true);
        input.beginFrame();
        const auto before = input.freezeActionsSnapshot();

        std::uint64_t public_api_calls = 0;
        const auto invoked = invokeImGuiRuntimeCallback(config, [&] {
            ++public_api_calls;
            appendImGuiPassToCanonicalGraphs(graph);
            applyImGuiCaptureForActions(input, true, true);
        });

        REQUIRE_FALSE(invoked);
        REQUIRE(public_api_calls == 0);
        const auto order = planOrder(graph);
        REQUIRE(std::find(order.begin(), order.end(), "imgui_pass") == order.end());
        REQUIRE(input.consumptionMask().controls == InputConsumptionMask{}.controls);
        REQUIRE_FALSE(input.consumptionMask().pointer_motion);
        REQUIRE(before.getKey(KeyCode::A));
        REQUIRE(before.getKey(KeyCode::MouseLeft));
    }
}

TEST_CASE("interactive graph loads display and places ImGui before output", "[imgui][frame-plan][rc5s]") {
    EngineLaunchConfig config;
    auto graph = canonicalGraph();
    std::uint64_t public_api_calls = 0;
    REQUIRE(invokeImGuiRuntimeCallback(config, [&] {
        ++public_api_calls;
        appendImGuiPassToCanonicalGraphs(graph);
    }));
    REQUIRE(public_api_calls == 1);

    const auto order = planOrder(graph);
    const auto source = std::find(order.begin(), order.end(), "display_source");
    const auto pass = std::find(order.begin(), order.end(), "imgui_pass");
    const auto output = std::find(order.begin(), order.end(), "output_transform");
    REQUIRE(source != order.end());
    REQUIRE(pass != order.end());
    REQUIRE(output != order.end());
    REQUIRE(source < pass);
    REQUIRE(pass < output);
    const auto &imgui = graph.at("rendering_passes").at(0).at("passes").at(1);
    CHECK(imgui.at("color_load_op") == "load");
    CHECK(imgui.at("output").at("color").at(0).at("target") == "display");
    CHECK(std::none_of(order.begin(), order.end(), [](const auto &name) { return name.rfind("__anchor_", 0) == 0; }));
}

TEST_CASE("input priority is ImGui then pelican UI then gameplay", "[imgui][input]") {
    InputStateCore input;
    input.queueButtonEvent(KeyCode::A, true);
    input.queueButtonEvent(KeyCode::MouseLeft, true);
    input.queueCursorMove(10.0f, 20.0f);
    input.beginFrame();

    // ImGui captures keyboard but leaves this pointer frame to pelican.ui.
    applyImGuiCaptureForActions(input, true, false);
    REQUIRE(input.consumptionMask().consumesControl(KeyCode::A));
    REQUIRE_FALSE(input.consumptionMask().consumesControl(KeyCode::MouseLeft));

    // pelican.ui hit/capture is the second consumer.
    input.consumePointerForActions();
    const auto gameplay = input.freezeActionsSnapshot();
    REQUIRE_FALSE(gameplay.getKey(KeyCode::A));
    REQUIRE_FALSE(gameplay.getKey(KeyCode::MouseLeft));
    REQUIRE(gameplay.mouse_delta_x == 0.0f);
    REQUIRE(gameplay.mouse_delta_y == 0.0f);
}

TEST_CASE("RC-5s interactive startup includes ImGui in captured final output", "[imgui][rc5s][gpu]") {
    setupLogger();
    FastModuleContainer modules;
    const auto project_root = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects/sprite_demo";
    GET_MODULE(PathResolver).setup(project_root, false);
    std::ifstream stream{project_root / "project.json"};
    const auto project = nlohmann::json::parse(stream);
    GET_MODULE(ProjectSource).setProjectData(project.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.embedded = true;
    launch.shader_hot_reload = false;
    REQUIRE(isImGuiRuntimeEnabled(launch));
    auto &renderer = GET_MODULE(Renderer);
    RendererTestAccess::setExecutionTracing(renderer, true);
    auto &input = GET_MODULE(InputState);
    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    const auto capture = [&] {
        time.advance();
        input.beginFrame();
        GET_MODULE(ImGuiSystem).routeInputAndBeginFrame(input);
        renderer.render();
        GET_MODULE(VulkanManageCore).waitIdle();
        return GET_MODULE(RenderTarget).readbackLastFrameRGBA8();
    };
    const auto visible = capture();
    InputEvent toggle;
    toggle.type = InputEvent::Type::button;
    toggle.code = KeyCode::F1;
    toggle.pressed = true;
    input.queueEvent(toggle);
    const auto hidden = capture();
    CHECK_FALSE(visible.empty());
    CHECK(visible != hidden);
    const auto order = RendererTestAccess::currentFramePlanOrder(renderer);
    REQUIRE(order.size() >= 2);
    CHECK(order[order.size() - 2] == "imgui_pass");
    CHECK(order.back() == "output_transform");
    CHECK(std::none_of(order.begin(), order.end(), [](const auto &name) { return name.starts_with("__anchor_"); }));
    CHECK(GET_MODULE(ImGuiSystem).publicApiCallCountForTesting() > 0);
}
