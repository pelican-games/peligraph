#include "../src/core/openxr/openxrmirrorsink.hpp"
#include "../src/project/featurecompose.hpp"
#include "../src/project/graphvariantpolicy.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

nlohmann::json envelope(std::string name) {
    return {
        {"schema", "pelican.render_feature"},
        {"version", 1},
        {"name", std::move(name)},
    };
}

const Pelican::CompiledGraphVariantPolicy &xrPolicy() {
    static const auto policy = Pelican::compileGraphVariantPolicy(
        Pelican::GraphVariantPolicyRequest{
            Pelican::RenderPipelineGraphVariant::xr});
    return policy;
}

bool includeInXrGraph(std::string_view feature_name,
                      const nlohmann::json &feature) {
    const auto decision = Pelican::decideGraphVariantFeature(
        xrPolicy(), feature_name, feature);
    if (decision.disposition ==
        Pelican::GraphVariantFeatureDisposition::reject) {
        throw std::runtime_error(
            Pelican::graphVariantFeatureRejectionMessage(
                xrPolicy(), decision));
    }
    return decision.disposition ==
           Pelican::GraphVariantFeatureDisposition::include;
}

} // namespace

TEST_CASE("OpenXR graph policy removes temporal velocity and UI features as units",
          "[wp133][openxr][feature-policy]") {
    auto taa = envelope("taa");
    taa["projection_jitter"] = {{"pattern", "halton23"}, {"phases", 8}};
    taa["render_targets"] = nlohmann::json::array({
        {{"name", "taa_accum"}, {"history", true}},
    });
    auto velocity = envelope("velocity");
    velocity["passes"] = nlohmann::json::array({
        {{"insert", "before:post_main"},
         {"pass", {{"name", "velocity_pass"}, {"type", "velocity"}}}},
    });
    auto ui = envelope("ui");
    ui["passes"] = nlohmann::json::array({
        {{"insert", "after:pelican_ui"},
         {"pass", {{"name", "pelican_ui"}, {"type", "ui"}}}},
    });
    const auto safe = envelope("safe_color");
    const std::unordered_map<std::string, nlohmann::json> features{
        {"taa.json", taa}, {"velocity.json", velocity}, {"ui.json", ui},
        {"safe.json", safe},
    };
    const nlohmann::json config{
        {"features", nlohmann::json::array(
                         {"taa.json", "velocity.json", "ui.json", "safe.json"})},
        {"render_targets", nlohmann::json::array()},
        {"rendering_passes", nlohmann::json::array()},
    };

    const auto composed = Pelican::composeRenderFeatureConfig(
        config,
        Pelican::RenderFeatureComposeDependencies{
            [&](std::string_view ref) { return features.at(std::string{ref}).dump(); },
            true,
            includeInXrGraph,
        });

    REQUIRE(composed.excluded_feature_names ==
            std::vector<std::string>{"taa", "velocity", "ui"});
    REQUIRE(composed.feature_names == std::vector<std::string>{"safe_color"});
    REQUIRE_FALSE(composed.projection_jitter.has_value());
    for (const auto &target : composed.config.at("render_targets")) {
        REQUIRE_FALSE(target.value("history", false));
    }
    Pelican::validateGraphVariantConfig(xrPolicy(),
                                        composed.config);
}

TEST_CASE("OpenXR activation rejects an unknown history feature by name",
          "[wp133][openxr][feature-policy]") {
    auto trails = envelope("custom_trails");
    trails["render_targets"] = nlohmann::json::array({
        {{"name", "trail_history"}, {"history", true}},
    });
    const nlohmann::json config{
        {"features", nlohmann::json::array({"trails.json"})},
        {"render_targets", nlohmann::json::array()},
        {"rendering_passes", nlohmann::json::array()},
    };

    REQUIRE_THROWS_WITH(
        Pelican::composeRenderFeatureConfig(
            config,
            Pelican::RenderFeatureComposeDependencies{
                [&](std::string_view) { return trails.dump(); },
                true,
                includeInXrGraph,
            }),
        "OpenXR activation rejected history feature 'custom_trails'");
}

TEST_CASE("OpenXR graph policy removes custom velocity and UI providers as units",
          "[wp133][openxr][feature-policy]") {
    auto motion = envelope("custom_motion");
    motion["render_targets"] = nlohmann::json::array({
        {{"name", "motion_vectors"}},
    });
    auto hud = envelope("custom_hud");
    hud["passes"] = nlohmann::json::array({
        {{"insert", "after:pelican_ui"},
         {"pass", {{"name", "hud"}, {"type", "fullscreen"}}}},
    });
    REQUIRE_FALSE(includeInXrGraph("custom_motion", motion));
    REQUIRE_FALSE(includeInXrGraph("custom_hud", hud));
}

TEST_CASE("OpenXR graph validation rejects authored history reads by pass name",
          "[wp133][openxr][feature-policy]") {
    const nlohmann::json config{
        {"render_targets", nlohmann::json::array()},
        {"rendering_passes",
         nlohmann::json::array({
             {{"name", "main"},
              {"passes", nlohmann::json::array({
                             {{"name", "custom_temporal"},
                              {"type", "fullscreen"},
                              {"input", nlohmann::json::array({"color@history"})}},
                         })}},
         })},
    };
    REQUIRE_THROWS_WITH(
        Pelican::validateGraphVariantConfig(xrPolicy(), config),
        "OpenXR activation rejected history feature '<base rendering config>:custom_temporal'");
}

TEST_CASE("OpenXR mirror letterboxes and treats minimized destinations as drops",
          "[wp133][openxr][mirror]") {
    const auto pillarbox = Pelican::OpenXr::mirrorLetterboxRect({100, 100}, {200, 100});
    REQUIRE(pillarbox.has_value());
    REQUIRE((pillarbox->offset == vk::Offset2D{50, 0}));
    REQUIRE((pillarbox->extent == vk::Extent2D{100, 100}));

    const auto letterbox = Pelican::OpenXr::mirrorLetterboxRect({200, 100}, {100, 100});
    REQUIRE(letterbox.has_value());
    REQUIRE((letterbox->offset == vk::Offset2D{0, 25}));
    REQUIRE((letterbox->extent == vk::Extent2D{100, 50}));

    REQUIRE_FALSE(Pelican::OpenXr::mirrorLetterboxRect({200, 100}, {0, 0}).has_value());
}
