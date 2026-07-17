#include <catch2/catch_test_macros.hpp>

#include "../src/core/imgui/imguiruntime.hpp"
#include "../src/core/renderingpass/frameplanner.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>

using namespace Pelican;

namespace {

nlohmann::json canonicalGraph() {
    return {
        {"render_targets", nlohmann::json::array({
             {{"name", "display"}},
         })},
        {"rendering_passes", nlohmann::json::array({
             {{"name", "main"},
              {"passes", nlohmann::json::array({
                   {{"name", "display_source"},
                    {"type", "ui"},
                    {"output", {{"color", "display"}, {"depth", nullptr}}}},
                   {{"name", "__anchor_imgui"},
                    {"type", "canonical_anchor"},
                    {"anchor", "imgui"},
                    {"after", nlohmann::json::array({"display_source"})}},
                   {{"name", "output_transform"},
                    {"type", "output_transform"},
                    {"input", nlohmann::json::array({"display"})},
                    {"after", nlohmann::json::array({"__anchor_imgui"})},
                    {"output", {{"color", "swapchain"}, {"depth", nullptr}}}},
               })}},
         })},
    };
}

std::vector<std::string> planOrder(const nlohmann::json &config) {
    auto definitions = parseFrameGraphDefinitionsFromConfigJson(config);
    REQUIRE(definitions.size() == 1);
    return framePlanOrder(planFrameGraph(definitions.front()));
}

} // namespace

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

TEST_CASE("interactive graph places ImGui at its canonical anchor", "[imgui][frame-plan]") {
    EngineLaunchConfig config;
    auto graph = canonicalGraph();
    std::uint64_t public_api_calls = 0;
    REQUIRE(invokeImGuiRuntimeCallback(config, [&] {
        ++public_api_calls;
        appendImGuiPassToCanonicalGraphs(graph);
    }));
    REQUIRE(public_api_calls == 1);

    const auto order = planOrder(graph);
    const auto anchor = std::find(order.begin(), order.end(), "__anchor_imgui");
    const auto pass = std::find(order.begin(), order.end(), "imgui_pass");
    const auto output = std::find(order.begin(), order.end(), "output_transform");
    REQUIRE(anchor != order.end());
    REQUIRE(pass != order.end());
    REQUIRE(output != order.end());
    REQUIRE(anchor < pass);
    REQUIRE(pass < output);
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
