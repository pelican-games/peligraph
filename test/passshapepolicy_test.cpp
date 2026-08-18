#include "../src/project/passshapepolicy.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <string_view>

namespace Pelican {
namespace {

bool hasViolation(const std::vector<PassShapeViolation> &violations,
                  PassShapeViolationKind kind) {
    return std::ranges::find(violations, kind,
                             &PassShapeViolation::kind) !=
           violations.end();
}

PassShapeObservation fullscreenObservation() {
    return PassShapeObservation{
        .type = RenderPassType::fullscreen,
        .color_outputs = {"color"},
        .depth_output = std::nullopt,
        .inputs = {},
    };
}

} // namespace

TEST_CASE(
    "WP324 project pass-shape authority distinguishes current history and swapchain roles",
    "[wp324][pass-shape][project]") {
    const auto &policy = defaultPassShapePolicy();
    auto observation = fullscreenObservation();

    REQUIRE(evaluatePassShape(policy, observation).empty());

    observation.inputs = {{"color", false, true}};
    const auto current = evaluatePassShape(policy, observation);
    REQUIRE(hasViolation(
        current,
        PassShapeViolationKind::current_frame_color_feedback));

    observation.inputs = {{"color", true, true}};
    const auto history = evaluatePassShape(policy, observation);
    REQUIRE_FALSE(hasViolation(
        history,
        PassShapeViolationKind::current_frame_color_feedback));
    REQUIRE(history.empty());

    observation = fullscreenObservation();
    observation.inputs = {{"swapchain", false, true}};
    REQUIRE(hasViolation(evaluatePassShape(policy, observation),
                         PassShapeViolationKind::swapchain_input));

    observation = fullscreenObservation();
    observation.color_outputs = {"swapchain"};
    REQUIRE(evaluatePassShape(policy, observation).empty());

    observation = fullscreenObservation();
    observation.depth_output = "swapchain";
    REQUIRE(hasViolation(evaluatePassShape(policy, observation),
                         PassShapeViolationKind::swapchain_depth_output));
}

TEST_CASE(
    "WP324 immutable injected policy changes the resolved input requirement without mutating the default",
    "[wp324][pass-shape][project][negative-contrast]") {
    const auto observation = fullscreenObservation();
    const auto &production = defaultPassShapePolicy();
    REQUIRE(evaluatePassShape(production, observation).empty());

    const PassShapePolicy requires_input = [&] {
        auto policy = production;
        auto &fullscreen = policy.types[
            static_cast<std::size_t>(RenderPassType::fullscreen)];
        fullscreen.minimum_inputs = 1;
        return policy;
    }();
    const auto injected = evaluatePassShape(requires_input, observation);
    REQUIRE(hasViolation(injected,
                         PassShapeViolationKind::input_count));
    REQUIRE(evaluatePassShape(production, observation).empty());
}

TEST_CASE(
    "WP324 authored observation keeps history and image identity explicit",
    "[wp324][pass-shape][project][observation]") {
    const nlohmann::json authored{
        {"name", "taa"},
        {"type", "fullscreen"},
        {"input", {"taa_accum@history", "light_buffer"}},
        {"output", {{"color", "taa_accum"}, {"depth", nullptr}}},
    };
    const std::array<std::string, 1> buffers{"light_buffer"};
    const auto observation = passShapeObservationFromJson(
        RenderPassType::fullscreen, authored, buffers);
    REQUIRE(observation.inputs.size() == 2);
    CHECK(observation.inputs[0].name == "taa_accum");
    CHECK(observation.inputs[0].history);
    CHECK(observation.inputs[0].image);
    CHECK(observation.inputs[1].name == "light_buffer");
    CHECK_FALSE(observation.inputs[1].history);
    CHECK_FALSE(observation.inputs[1].image);
}

} // namespace Pelican
