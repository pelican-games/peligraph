#include "imgui/compiledplanviewer.hpp"

#include "../src/core/renderingpass/frameplanner.hpp"
#include "../src/core/renderingpass/renderingsamplecount.hpp"
#include "../src/core/renderingpass/rendertargetjsonparser.hpp"
#include "../src/project/samplecountplanning.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

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

RenderingTargetPlanDeviceFacts planningDeviceFacts() {
    return RenderingTargetPlanDeviceFacts{
        .multiview = true,
        .max_multiview_view_count = 8,
        .query_attachment_samples =
            [](const RenderTargetDefinition &) {
                return std::vector<std::uint32_t>{1};
            },
        .query_image_format_capability =
            [](const RenderTargetDefinition &) {
                return RenderingImageFormatCapability{
                    .image_usage_supported = true,
                    .supported_samples = {1},
                    .max_mip_levels = 16,
                    .max_array_layers = 8,
                    .transient_attachment_supported = true,
                    .local_read_attachment_supported = true,
                };
            },
        .transient_attachments = true,
        .dynamic_rendering_local_read = true,
    };
}

nlohmann::json compileIndependentOpportunityPlan(
    PlanningProfileKind profile_kind) {
    using Json = nlohmann::json;
    const Json config{
        {"render_targets",
         Json::array(
             {{{"name", "alpha_output"},
               {"extent_scale", 1.0},
               {"width", 32},
               {"height", 32},
               {"format", "R8G8B8A8_UNORM"},
               {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}},
              {{"name", "beta_output"},
               {"extent_scale", 1.0},
               {"width", 32},
               {"height", 32},
               {"format", "R8G8B8A8_UNORM"},
               {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}}})},
        {"rendering_passes",
         Json::array(
             {{{"name", "independent_opportunities"},
               {"passes",
                Json::array(
                    {{{"name", "alpha"},
                      {"type", "fullscreen"},
                      {"output",
                       {{"color", "alpha_output"}, {"depth", nullptr}}}},
                     {{"name", "beta"},
                      {"type", "fullscreen"},
                      {"output",
                       {{"color", "beta_output"},
                        {"depth", nullptr}}}}})}}})},
    };
    const auto graphs = parseFrameGraphDefinitionsFromConfigJson(config);
    const auto targets = parseRenderTargetDefinitionsFromJson(config);
    const auto compilation = compileRenderingTargetPlans(
        graphs, targets, compileSampleCountPolicy(config),
        vk::Format::eB8G8R8A8Unorm, planningDeviceFacts(), std::nullopt,
        std::nullopt,
        TargetPlanningPolicy{
            .profile = PlanningProfile{.kind = profile_kind},
        });
    if (graphs.size() != 1 || compilation.plans.size() != 1) {
        throw std::runtime_error(
            "independent opportunity producer must compile one graph and one "
            "physical plan");
    }
    return vulkanTargetPlanToJson(*compilation.plans.front());
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
            [](const auto &candidate) {
                return candidate.adoption ==
                       PlanningOpportunityAdoption::adopted;
            }));
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

class ScopedImGuiContext {
  public:
    ScopedImGuiContext() {
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.DisplaySize = {1800.0f, 1000.0f};
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        unsigned char *pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        (void)pixels;
        (void)width;
        (void)height;

        auto &platform = ImGui::GetPlatformIO();
        platform.Platform_ClipboardUserData = &clipboard;
        platform.Platform_SetClipboardTextFn =
            [](ImGuiContext *context, const char *text) {
                ImGui::SetCurrentContext(context);
                auto *destination = static_cast<std::string *>(
                    ImGui::GetPlatformIO().Platform_ClipboardUserData);
                *destination = text == nullptr ? std::string{} : text;
            };
    }

    ~ScopedImGuiContext() { ImGui::DestroyContext(); }

    ScopedImGuiContext(const ScopedImGuiContext &) = delete;
    ScopedImGuiContext &operator=(const ScopedImGuiContext &) = delete;

    std::string clipboard;
};

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

TEST_CASE(
    "Planning opportunities contrast actual optimized and conservative compiles",
    "[imgui][compiled-plan][wp310][negative-contrast]") {
    const auto optimized_plan = compileIndependentOpportunityPlan(
        PlanningProfileKind::optimized);
    const auto optimized = buildCompiledPlanOpportunities(optimized_plan);

    REQUIRE(optimized.available);
    REQUIRE(optimized.profile == "optimized");
    REQUIRE(candidateCount(optimized) > 0);
    REQUIRE_FALSE(optimized.alias_candidates.empty());
    REQUIRE_FALSE(optimized.fusion_candidates.empty());
    REQUIRE_FALSE(optimized.parallel_candidates.empty());
    REQUIRE(findCandidate(optimized.alias_candidates, "alpha_output",
                          "beta_output")
                .adoption != PlanningOpportunityAdoption::unknown);
    REQUIRE(findCandidate(optimized.fusion_candidates, "alpha", "beta")
                .adoption != PlanningOpportunityAdoption::unknown);
    REQUIRE(findCandidate(optimized.parallel_candidates, "alpha", "beta")
                .adoption == PlanningOpportunityAdoption::unknown);
    REQUIRE(std::all_of(
        optimized.parallel_candidates.begin(),
        optimized.parallel_candidates.end(), [](const auto &candidate) {
            return candidate.adoption ==
                   PlanningOpportunityAdoption::unknown;
        }));
    REQUIRE(optimized.empty_state.empty());

    // Same producer entry and graph; only the planning profile changes.
    const auto conservative_plan = compileIndependentOpportunityPlan(
        PlanningProfileKind::conservative_debug);
    const auto conservative = buildCompiledPlanOpportunities(conservative_plan);
    REQUIRE(conservative.available);
    REQUIRE(conservative.profile == "conservative_debug");
    REQUIRE(candidateCount(conservative) == 0);
    REQUIRE(adoptedCount(conservative) == 0);
    REQUIRE(conservative.empty_state == "no candidates");

    REQUIRE(optimized_plan.at("planning_opportunities") !=
            conservative_plan.at("planning_opportunities"));
}

TEST_CASE(
    "Planning opportunities reject malformed required collections with named errors",
    "[imgui][compiled-plan][wp310][validation]") {
    const auto valid = compileIndependentOpportunityPlan(
        PlanningProfileKind::optimized);

    const auto unavailable_code = [](nlohmann::json input) {
        const auto result = buildCompiledPlanOpportunities(input);
        REQUIRE_FALSE(result.available);
        REQUIRE(result.alias_candidates.empty());
        REQUIRE(result.fusion_candidates.empty());
        REQUIRE(result.parallel_candidates.empty());
        REQUIRE_FALSE(result.unavailable_reason.empty());
        return result.unavailable_reason_code;
    };

    SECTION("missing opportunity collection") {
        auto input = valid;
        input.at("planning_opportunities").erase("parallel_candidates");
        REQUIRE(unavailable_code(std::move(input)) ==
                "physical_plan_missing_field");
    }
    SECTION("wrong opportunity collection type") {
        auto input = valid;
        input.at("planning_opportunities")["fusion_candidates"] =
            nlohmann::json::object();
        REQUIRE(unavailable_code(std::move(input)) ==
                "physical_plan_type_error");
    }
    SECTION("broken alias adoption evidence") {
        auto input = valid;
        input["alias_groups"] = "not an array";
        REQUIRE(unavailable_code(std::move(input)) ==
                "physical_plan_type_error");
    }
    SECTION("non-object physical plan") {
        REQUIRE(unavailable_code(nlohmann::json::array()) ==
                "physical_plan_type_error");
    }
}

TEST_CASE(
    "Compiled plan draw labels parallel adoption as unknown",
    "[imgui][compiled-plan][wp310][ui]") {
    const auto plan = compileIndependentOpportunityPlan(
        PlanningProfileKind::optimized);
    const auto opportunities = buildCompiledPlanOpportunities(plan);
    REQUIRE(opportunities.available);
    REQUIRE(findCandidate(opportunities.parallel_candidates, "alpha", "beta")
                .adoption == PlanningOpportunityAdoption::unknown);

    CompiledPlanProgram program;
    program.variant = "optimized";
    program.has_target_plan = true;
    program.planning_opportunities = opportunities;
    program.target_plan_json = plan;
    CompiledPlanModel model;
    model.programs.push_back(std::move(program));

    ScopedImGuiContext imgui;
    CompiledPlanViewer viewer{std::move(model)};
    bool open = true;
    ImGui::NewFrame();
    ImGui::LogToClipboard(20);
    viewer.draw(&open);
    ImGui::LogFinish();
    ImGui::Render();

    REQUIRE(imgui.clipboard.find("planning opportunities") !=
            std::string::npos);
    const auto parallel_heading =
        imgui.clipboard.find("parallel candidates");
    REQUIRE(parallel_heading != std::string::npos);
    const auto raw_heading = imgui.clipboard.find(
        "raw pelican.vulkan_target_plan", parallel_heading);
    const auto unknown_row = imgui.clipboard.find(
        "[unknown] alpha + beta", parallel_heading);
    REQUIRE(unknown_row != std::string::npos);
    REQUIRE((raw_heading == std::string::npos || unknown_row < raw_heading));
    const auto false_row = imgui.clipboard.find(
        "[not adopted] alpha + beta", parallel_heading);
    REQUIRE((false_row == std::string::npos ||
             (raw_heading != std::string::npos && false_row > raw_heading)));
}
