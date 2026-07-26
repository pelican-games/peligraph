#include "imgui/compiledplanviewer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <nlohmann/json.hpp>

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
        {"planning_opportunities", nlohmann::json::array()},
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

TEST_CASE("Planning opportunities render as readable rows",
          "[imgui][compiled-plan]") {
    SECTION("string entries pass through") {
        const auto plan = nlohmann::json{
            {"planning_opportunities",
             nlohmann::json::array({"merge_scopes", "alias_depth"})}};
        const auto rows = buildCompiledPlanOpportunities(plan);
        REQUIRE(rows == std::vector<std::string>{"merge_scopes", "alias_depth"});
    }

    SECTION("object entries are flattened to key=value text") {
        const auto plan = nlohmann::json{
            {"planning_opportunities",
             nlohmann::json::array({nlohmann::json{{"kind", "alias"},
                                                  {"resource", "gbuffer_albedo"}}})}};
        const auto rows = buildCompiledPlanOpportunities(plan);
        REQUIRE(rows.size() == 1);
        REQUIRE(rows[0].find("kind=alias") != std::string::npos);
        REQUIRE(rows[0].find("resource=gbuffer_albedo") != std::string::npos);
    }

    SECTION("absent or malformed collections yield no rows") {
        REQUIRE(buildCompiledPlanOpportunities(nlohmann::json::object()).empty());
        REQUIRE(buildCompiledPlanOpportunities(
                    nlohmann::json{{"planning_opportunities", 7}})
                    .empty());
    }
}
