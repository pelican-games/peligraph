#include "imgui/compiledplanviewer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

#ifndef PELICAN_TEST_FRAME_PLAN_FIXTURE
#error "PELICAN_TEST_FRAME_PLAN_FIXTURE must name the captured get_frame_plan response"
#endif

using namespace Pelican;

namespace {

std::string factValue(const std::vector<CompiledPlanFact> &facts,
                      std::string_view key) {
    const auto it = std::find_if(
        facts.begin(), facts.end(),
        [&](const CompiledPlanFact &fact) { return fact.key == key; });
    return it == facts.end() ? std::string{} : it->value;
}

bool hasFact(const std::vector<CompiledPlanFact> &facts, std::string_view key) {
    return std::any_of(
        facts.begin(), facts.end(),
        [&](const CompiledPlanFact &fact) { return fact.key == key; });
}

nlohmann::json capturedPhysicalTargetPlan() {
    std::ifstream input{PELICAN_TEST_FRAME_PLAN_FIXTURE, std::ios::binary};
    if (!input) {
        throw std::runtime_error("failed to open captured frame-plan fixture");
    }
    return nlohmann::json::parse(input).at("physical_target_plan");
}

std::size_t candidateCount(const CompiledPlanOpportunities &opportunities) {
    return opportunities.alias_candidates.size() +
           opportunities.fusion_candidates.size() +
           opportunities.parallel_candidates.size();
}

std::size_t adoptedCount(const CompiledPlanOpportunities &opportunities) {
    const auto count_adopted = [](const auto &candidates) {
        return static_cast<std::size_t>(std::count_if(
            candidates.begin(), candidates.end(),
            [](const auto &candidate) { return candidate.adopted; }));
    };
    return count_adopted(opportunities.alias_candidates) +
           count_adopted(opportunities.fusion_candidates) +
           count_adopted(opportunities.parallel_candidates);
}

const CompiledPlanOpportunityPair &findCandidate(
    const std::vector<CompiledPlanOpportunityPair> &candidates,
    std::string_view first, std::string_view second) {
    const auto candidate = std::find_if(
        candidates.begin(), candidates.end(), [&](const auto &row) {
            return row.first == first && row.second == second;
        });
    if (candidate == candidates.end()) {
        throw std::runtime_error("expected opportunity candidate was not found");
    }
    return *candidate;
}

} // namespace

TEST_CASE("Compiled plan facts promote the decisions a person checks first",
          "[imgui][compiled-plan]") {
    const auto plan = nlohmann::json{
        {"schema", "pelican.vulkan_target_plan"},
        {"version", 1},
        {"graph", "xr"},
        {"logical_graph_fingerprint", "0x1122334455667788"},
        {"automatic_plan_fingerprint", "0x99aabbccddeeff00"},
        {"view_count", 2},
        {"uses_multiview", true},
        {"endpoint_supports_multiview", true},
        {"max_multiview_view_count", 2},
        {"mixed_execution", false},
        {"view_execution_plan", nlohmann::json::array({1, 2, 3})},
        {"required_physical_features", nlohmann::json::array({"multiview"})},
        {"ejectable_pin_package", nlohmann::json::object({{"pins", 1}})},
        {"ejectable_physical_fragment", nullptr},
    };

    const auto facts = buildCompiledPlanFacts(plan);

    REQUIRE(factValue(facts, "graph") == "xr");
    REQUIRE(factValue(facts, "view_count") == "2");
    REQUIRE(factValue(facts, "uses_multiview") == "true");
    REQUIRE(factValue(facts, "mixed_execution") == "false");
    REQUIRE(factValue(facts, "logical_graph_fingerprint") ==
            "0x1122334455667788");
    // Collections are summarised by count so the table stays readable.
    REQUIRE(factValue(facts, "view_execution_plan") == "3 entries");
    REQUIRE(factValue(facts, "required_physical_features") == "1 entries");
    // Eject packages report availability, not their whole body.
    REQUIRE(factValue(facts, "ejectable_pin_package") == "available");
    REQUIRE(factValue(facts, "ejectable_physical_fragment") == "none");
}

TEST_CASE("Compiled plan facts tolerate an unknown or partial plan document",
          "[imgui][compiled-plan]") {
    // The render-graph compiler keeps evolving its plan document. A viewer that
    // only knows v1 keys must not fail on a plan that adds or drops fields: the
    // promoted table degrades and the raw tree keeps showing everything.
    SECTION("missing keys are skipped instead of throwing") {
        const auto plan = nlohmann::json{{"graph", "flat"}};
        const auto facts = buildCompiledPlanFacts(plan);
        REQUIRE(factValue(facts, "graph") == "flat");
        REQUIRE_FALSE(hasFact(facts, "view_count"));
        REQUIRE_FALSE(hasFact(facts, "uses_multiview"));
    }

    SECTION("a non-object document yields no facts") {
        REQUIRE(buildCompiledPlanFacts(nlohmann::json::array()).empty());
        REQUIRE(buildCompiledPlanFacts(nlohmann::json{}).empty());
    }

    SECTION("unknown future fields do not disturb the promoted table") {
        const auto plan = nlohmann::json{
            {"graph", "preview"},
            {"some_future_decision", nlohmann::json{{"nested", true}}},
        };
        const auto facts = buildCompiledPlanFacts(plan);
        REQUIRE(factValue(facts, "graph") == "preview");
        REQUIRE_FALSE(hasFact(facts, "some_future_decision"));
    }
}

TEST_CASE("Planning opportunities read the captured producer document and its empty control",
          "[imgui][compiled-plan]") {
    const auto optimized_plan = capturedPhysicalTargetPlan();
    const auto optimized = buildCompiledPlanOpportunities(optimized_plan);

    REQUIRE(optimized.available);
    REQUIRE(optimized.profile == "optimized");
    REQUIRE(candidateCount(optimized) == 2);
    REQUIRE(adoptedCount(optimized) == 1);
    REQUIRE(optimized.alias_candidates.size() == 2);
    REQUIRE(optimized.fusion_candidates.empty());
    REQUIRE(optimized.parallel_candidates.empty());
    REQUIRE(findCandidate(optimized.alias_candidates, "Bloom_Threshold_RT",
                          "g_emissive")
                .adopted);
    REQUIRE_FALSE(findCandidate(optimized.alias_candidates, "Bloom_Threshold_RT",
                                "gbuffer_albedo")
                      .adopted);
    REQUIRE(optimized.empty_state.empty());

    // The control keeps the captured producer shape but applies the profile's
    // no-opportunity result at the same reader entry point.
    auto conservative_plan = optimized_plan;
    auto &report = conservative_plan.at("planning_opportunities");
    report["profile"] = "conservative_debug";
    report.at("alias_candidates").clear();
    report.at("fusion_candidates").clear();
    report.at("parallel_candidates").clear();
    conservative_plan.at("alias_groups").clear();

    const auto conservative =
        buildCompiledPlanOpportunities(conservative_plan);
    REQUIRE(conservative.available);
    REQUIRE(conservative.profile == "conservative_debug");
    REQUIRE(candidateCount(conservative) == 0);
    REQUIRE(adoptedCount(conservative) == 0);
    REQUIRE(conservative.empty_state == "no candidates");
}
