#include "../src/project/samplecountplanning.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace Pelican {
namespace {

SampleCountResourceCapability capability(
    std::string resource, std::string format,
    std::vector<std::uint32_t> counts) {
    return {std::move(resource), std::move(format), std::move(counts)};
}

void requireThrowsContaining(const std::function<void()> &operation,
                             std::string_view expected) {
    try {
        operation();
        FAIL("operation did not throw");
    } catch (const std::runtime_error &error) {
        REQUIRE(std::string_view{error.what()}.find(expected) !=
                std::string_view::npos);
    }
}

} // namespace

TEST_CASE("sample-count planning selects one count for an attachment group",
          "[sample-count][planning]") {
    const std::vector groups{
        SampleCountGroupRequest{
            "main_geometry", true,
            {
                capability("gbuffer_albedo", "BGRA8", {1, 2, 4, 8}),
                capability("gbuffer_normal", "RGBA16F", {1, 2, 4}),
                capability("scene_depth", "D32F", {1, 2, 4}),
            }},
        SampleCountGroupRequest{
            "post", false,
            {capability("display", "BGRA8", {1, 2, 4, 8})}},
    };

    const auto plan = resolveSampleCountPlan(
        {SampleCountRequestMode::prefer, 4}, groups);
    REQUIRE(plan.groups.at(0).selected_samples == 4);
    REQUIRE_FALSE(plan.groups.at(0).fallback);
    REQUIRE(plan.groups.at(1).selected_samples == 1);
    REQUIRE(plan.resources.at(0).resource == "display");
    REQUIRE(plan.resources.at(0).samples == 1);
    REQUIRE(plan.resources.at(3).resource == "scene_depth");
    REQUIRE(plan.resources.at(3).samples == 4);
}

TEST_CASE("preferred sample-count fallback names every limiting resource",
          "[sample-count][planning]") {
    const std::vector groups{
        SampleCountGroupRequest{
            "custom_gbuffer", true,
            {
                capability("albedo", "BGRA8", {1, 2, 4, 8}),
                capability("custom_id", "R32_UINT", {1, 2}),
                capability("depth", "D32F", {1, 2}),
            }},
    };

    const auto plan = resolveSampleCountPlan(
        {SampleCountRequestMode::prefer, 4}, groups);
    const auto &group = plan.groups.front();
    REQUIRE(group.selected_samples == 2);
    REQUIRE(group.fallback);
    REQUIRE(group.reason == "preferred_common_fallback");
    REQUIRE(group.limiting_resources ==
            std::vector<std::string>{"custom_id (R32_UINT)",
                                     "depth (D32F)"});
}

TEST_CASE("exact sample-count rejection identifies the custom attachment",
          "[sample-count][planning]") {
    const std::vector groups{
        SampleCountGroupRequest{
            "custom_gbuffer", true,
            {
                capability("albedo", "BGRA8", {1, 2, 4}),
                capability("object_id", "R32_UINT", {1}),
            }},
    };

    requireThrowsContaining(
        [&] {
            (void)resolveSampleCountPlan(
                {SampleCountRequestMode::exact, 4}, groups);
        },
        "object_id (R32_UINT)");
}

TEST_CASE("sample-count plan dump is deterministic",
          "[sample-count][planning]") {
    const std::vector groups{
        SampleCountGroupRequest{
            "z", true,
            {capability("z_depth", "D32F", {4, 1, 2, 4})}},
        SampleCountGroupRequest{
            "a", true,
            {capability("a_color", "RGBA16F", {1, 4, 2})}},
    };

    const auto first = resolveSampleCountPlan(
        {SampleCountRequestMode::automatic, 4}, groups);
    auto reversed = groups;
    std::reverse(reversed.begin(), reversed.end());
    const auto second = resolveSampleCountPlan(
        {SampleCountRequestMode::automatic, 4}, reversed);
    REQUIRE(first == second);
    REQUIRE(resolvedSampleCountPlanToJson(first).dump() ==
            resolvedSampleCountPlanToJson(second).dump());
}

TEST_CASE("sample-count authoring compiles to a typed policy",
          "[sample-count][planning][authoring]") {
    const auto config = nlohmann::json{
        {"multisampling",
         {
             {"samples", 4},
             {"fallback", "lower_supported"},
             {"scope", "none"},
             {"targets",
              nlohmann::json::array({"lit_color", "gbuffer_albedo"})},
         }},
    };

    const auto policy = compileSampleCountPolicy(config);
    REQUIRE(policy.authored);
    REQUIRE(policy.request ==
            (SampleCountRequest{SampleCountRequestMode::prefer, 4}));
    REQUIRE(policy.scope == SampleCountScope::none);
    REQUIRE(policy.targets ==
            std::vector<std::string>{"gbuffer_albedo", "lit_color"});
    REQUIRE(sampleCountPolicyToJson(policy) ==
            (nlohmann::ordered_json{
                {"mode", "prefer"},
                {"samples", 4},
                {"scope", "none"},
                {"targets",
                 nlohmann::ordered_json::array(
                     {"gbuffer_albedo", "lit_color"})},
            }));
}

TEST_CASE("sample-count authoring defaults to exact single sample and rejects ambiguity",
          "[sample-count][planning][authoring]") {
    const auto defaults =
        compileSampleCountPolicy(nlohmann::json::object());
    REQUIRE_FALSE(defaults.authored);
    REQUIRE(defaults.request ==
            (SampleCountRequest{SampleCountRequestMode::exact, 1}));
    REQUIRE(defaults.scope == SampleCountScope::geometry);

    requireThrowsContaining(
        [] {
            (void)compileSampleCountPolicy(
                nlohmann::json{
                    {"multisampling",
                     {{"mode", "prefer"},
                      {"fallback", "lower_supported"}}},
                });
        },
        "both mode and fallback");
    requireThrowsContaining(
        [] {
            (void)compileSampleCountPolicy(
                nlohmann::json{
                    {"multisampling", {{"samples", 3}}},
                });
        },
        "power of two");
}

} // namespace Pelican
