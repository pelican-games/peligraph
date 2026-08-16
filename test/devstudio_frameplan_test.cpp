#include "frameplanmodel.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace PelicanStudio;
using Catch::Matchers::ContainsSubstring;

#ifndef PELICAN_TEST_FRAME_PLAN_FIXTURE
#error "PELICAN_TEST_FRAME_PLAN_FIXTURE must name the captured get_frame_plan response"
#endif

namespace {

using Json = nlohmann::json;

std::string capturedResponse() {
    std::ifstream stream{PELICAN_TEST_FRAME_PLAN_FIXTURE, std::ios::binary};
    if (!stream) {
        throw std::runtime_error(
            "failed to open captured get_frame_plan fixture");
    }
    return std::string{std::istreambuf_iterator<char>{stream},
                       std::istreambuf_iterator<char>{}};
}

std::size_t decisionEntryCount(const Json &value) {
    std::size_t result = 0;
    if (value.is_array()) {
        for (const auto &entry : value) {
            result += decisionEntryCount(entry);
        }
        return result;
    }
    if (!value.is_object()) {
        return result;
    }
    for (const auto &[key, child] : value.items()) {
        if (key == "decisions") {
            if (!child.is_array()) {
                throw std::runtime_error(
                    "captured decisions field is not an array");
            }
            result += child.size();
        }
        result += decisionEntryCount(child);
    }
    return result;
}

void clearDecisionArrays(Json &value) {
    if (value.is_array()) {
        for (auto &entry : value) {
            clearDecisionArrays(entry);
        }
        return;
    }
    if (!value.is_object()) {
        return;
    }
    for (auto &[key, child] : value.items()) {
        if (key == "decisions") {
            child = Json::array();
        } else {
            clearDecisionArrays(child);
        }
    }
}

std::size_t modelDecisionCount(const FramePlanModel &model) {
    std::size_t result = 0;
    for (const auto &group : model.decision_groups) {
        result += group.decisions.size();
    }
    return result;
}

const FramePlanResource *findResource(const FramePlanModel &model,
                                      std::string_view name) {
    const auto found = std::find_if(
        model.resources.begin(), model.resources.end(),
        [&](const FramePlanResource &resource) { return resource.name == name; });
    return found == model.resources.end() ? nullptr : &*found;
}

const FramePlanNode *findNode(const FramePlanModel &model,
                              std::string_view name) {
    const auto found = std::find_if(
        model.nodes.begin(), model.nodes.end(),
        [&](const FramePlanNode &node) { return node.name == name; });
    return found == model.nodes.end() ? nullptr : &*found;
}

const FramePlanBackendCandidate *findBackendCandidate(
    const FramePlanModel &model, std::string_view name) {
    const auto found = std::find_if(
        model.backend_candidates.begin(), model.backend_candidates.end(),
        [&](const FramePlanBackendCandidate &candidate) {
            return candidate.candidate == name;
        });
    return found == model.backend_candidates.end() ? nullptr : &*found;
}

bool hasDecision(const FramePlanModel &model, std::string_view id) {
    return std::any_of(
        model.decision_groups.begin(), model.decision_groups.end(),
        [&](const FramePlanDecisionGroup &group) {
            return std::any_of(
                group.decisions.begin(), group.decisions.end(),
                [&](const FramePlanDecision &decision) {
                    return decision.id == id;
                });
        });
}

} // namespace

TEST_CASE(
    "Devstudio reads every decision from a captured get_frame_plan response and invents none",
    "[devstudio][frame-plan][wp299][negative-contrast]") {
    const std::string response = capturedResponse();
    const Json captured = Json::parse(response);
    const FramePlanModel model = buildFramePlanModel(response);

    REQUIRE(captured.at("schema") == "pelican.frame_plan");
    REQUIRE(captured.at("version") == 1);
    REQUIRE(model.graph == captured.at("graph").get<std::string>());
    REQUIRE(model.response_bytes == response.size());
    REQUIRE(Json::parse(model.raw_json) == captured);
    REQUIRE(model.nodes.size() == captured.at("nodes").size());
    REQUIRE(model.barriers.size() == captured.at("barriers").size());

    const std::size_t published_decisions = decisionEntryCount(captured);
    REQUIRE(published_decisions == 131);
    REQUIRE(modelDecisionCount(model) == published_decisions);
    REQUIRE(hasDecision(model, "pelican.plan.multiview_auto_gate@1"));
    REQUIRE(hasDecision(model, "pelican.plan.backend_candidate_selected@1"));

    std::set<std::string, std::less<>> grouped_subjects;
    for (const auto &group : model.decision_groups) {
        REQUIRE(grouped_subjects.insert(group.subject).second);
        REQUIRE_FALSE(group.decisions.empty());
        for (const auto &decision : group.decisions) {
            REQUIRE(decision.subject == group.subject);
            REQUIRE_FALSE(decision.id.empty());
            REQUIRE_FALSE(decision.detail.empty());
        }
    }

    Json without_decisions = captured;
    clearDecisionArrays(without_decisions);
    REQUIRE(decisionEntryCount(without_decisions) == 0);
    const FramePlanModel empty =
        buildFramePlanModel(without_decisions.dump());
    REQUIRE(empty.decision_groups.empty());
    REQUIRE(modelDecisionCount(empty) == 0);
}

TEST_CASE(
    "Devstudio exposes captured compose-time provenance and invents none",
    "[devstudio][frame-plan][wp300][provenance][negative-contrast]") {
    const Json captured = Json::parse(capturedResponse());
    const FramePlanModel model = buildFramePlanModel(captured.dump());

    const FramePlanNode *project = findNode(model, "gbuffer_pass");
    REQUIRE(project != nullptr);
    REQUIRE(project->source == "project");
    REQUIRE(project->provider_feature.empty());
    REQUIRE(project->provider_reference.empty());

    const FramePlanNode *feature = findNode(model, "pelican_ui");
    REQUIRE(feature != nullptr);
    REQUIRE(feature->source == "feature:ui");
    REQUIRE(feature->provider_feature == "ui");
    REQUIRE(feature->provider_reference ==
            "engine://features/ui.json");

    const FramePlanNode *engine = findNode(model, "output_transform");
    REQUIRE(engine != nullptr);
    REQUIRE(engine->source == "engine");

    const FramePlanResource *project_target =
        findResource(model, "gbuffer_albedo");
    REQUIRE(project_target != nullptr);
    REQUIRE(project_target->kind == "render_target");
    REQUIRE(project_target->source == "project");
    const FramePlanResource *engine_target =
        findResource(model, "display");
    REQUIRE(engine_target != nullptr);
    REQUIRE(engine_target->source == "engine");

    Json without_provenance = captured;
    without_provenance.erase("resources");
    for (auto &node : without_provenance["nodes"]) {
        node.erase("source");
        node.erase("provider_feature");
        node.erase("provider_ref");
    }
    const FramePlanModel absent =
        buildFramePlanModel(without_provenance.dump());
    const FramePlanNode *absent_project =
        findNode(absent, "gbuffer_pass");
    const FramePlanNode *absent_feature =
        findNode(absent, "pelican_ui");
    const FramePlanResource *absent_target =
        findResource(absent, "gbuffer_albedo");
    REQUIRE(absent_project != nullptr);
    REQUIRE(absent_feature != nullptr);
    REQUIRE(absent_target != nullptr);
    REQUIRE(absent_project->source.empty());
    REQUIRE(absent_feature->source.empty());
    REQUIRE(absent_target->source.empty());
}

TEST_CASE(
    "Devstudio exposes physical reasons and rejected backend failures from the captured response",
    "[devstudio][frame-plan][wp299][negative-contrast]") {
    const Json captured = Json::parse(capturedResponse());
    const FramePlanModel model = buildFramePlanModel(captured.dump());
    const Json &physical = captured.at("physical_target_plan");

    std::size_t reason_count = 0;
    for (const auto &resource : physical.at("resources")) {
        const auto reason = resource.find("reason");
        if (reason == resource.end()) {
            continue;
        }
        ++reason_count;
        const FramePlanResource *modeled = findResource(
            model, resource.at("logical_resource").get<std::string>());
        REQUIRE(modeled != nullptr);
        REQUIRE(modeled->reason == reason->get<std::string>());
    }
    REQUIRE(reason_count == 22);

    const Json &published_candidates =
        physical.at("backend_selection").at("candidates");
    REQUIRE(model.backend_candidates.size() == published_candidates.size());
    std::size_t rejected_count = 0;
    std::size_t failure_count = 0;
    for (const auto &candidate : published_candidates) {
        const auto candidate_name = candidate.at("candidate").get<std::string>();
        const FramePlanBackendCandidate *modeled =
            findBackendCandidate(model, candidate_name);
        REQUIRE(modeled != nullptr);
        REQUIRE(modeled->feasible == candidate.at("feasible").get<bool>());
        REQUIRE(modeled->failures.size() == candidate.at("failures").size());
        if (!modeled->feasible) {
            ++rejected_count;
            failure_count += modeled->failures.size();
            for (const auto &failure : modeled->failures) {
                REQUIRE_FALSE(failure.id.empty());
                REQUIRE_FALSE(failure.detail.empty());
            }
        }
    }
    REQUIRE(rejected_count == 1);
    REQUIRE(failure_count == 2);

    Json without_explanations = captured;
    for (auto &resource :
         without_explanations["physical_target_plan"]["resources"]) {
        resource.erase("reason");
    }
    for (auto &candidate : without_explanations["physical_target_plan"]
                                               ["backend_selection"]
                                               ["candidates"]) {
        candidate["feasible"] = true;
        candidate["failures"] = Json::array();
        candidate["diagnostics"] = Json::array();
    }
    const FramePlanModel unexplained =
        buildFramePlanModel(without_explanations.dump());
    REQUIRE(std::none_of(
        unexplained.resources.begin(), unexplained.resources.end(),
        [](const FramePlanResource &resource) { return !resource.reason.empty(); }));
    REQUIRE(std::none_of(
        unexplained.backend_candidates.begin(),
        unexplained.backend_candidates.end(),
        [](const FramePlanBackendCandidate &candidate) {
            return !candidate.feasible || !candidate.failures.empty();
        }));
}

TEST_CASE(
    "Devstudio keeps unmatched material-filter paths in pass details",
    "[devstudio][frame-plan][wp299][negative-contrast]") {
    Json captured = Json::parse(capturedResponse());
    const FramePlanModel baseline = buildFramePlanModel(captured.dump());
    REQUIRE(std::none_of(
        baseline.nodes.begin(), baseline.nodes.end(),
        [](const FramePlanNode &node) { return node.material_filter.has_value(); }));

    Json &published_node = captured.at("nodes").at(0);
    const std::string node_name = published_node.at("name").get<std::string>();
    published_node["material_filter"] = Json{
        {"include", {"opaque", "declared-but-missing"}},
        {"exclude", {"transparent", "excluded-but-missing"}},
        {"filter_id", "wp299-captured-shape"},
        {"resolved_draw_count", 3},
        {"resolution_state", "resolved"},
        {"resolution_provenance", "runtime_draw_queue"},
        {"unmatched_include", {"declared-but-missing"}},
        {"unmatched_exclude", {"excluded-but-missing"}},
    };
    const FramePlanModel with_filter = buildFramePlanModel(captured.dump());
    const FramePlanNode *modeled = findNode(with_filter, node_name);
    REQUIRE(modeled != nullptr);
    REQUIRE(modeled->material_filter.has_value());
    REQUIRE(modeled->material_filter->unmatched_include ==
            std::vector<std::string>{"declared-but-missing"});
    REQUIRE(modeled->material_filter->unmatched_exclude ==
            std::vector<std::string>{"excluded-but-missing"});
}

TEST_CASE(
    "Devstudio raw JSON escape hatch retains keys unknown to the model",
    "[devstudio][frame-plan][wp299][negative-contrast]") {
    const Json captured = Json::parse(capturedResponse());
    const FramePlanModel baseline = buildFramePlanModel(captured.dump());
    const Json baseline_raw = Json::parse(baseline.raw_json);
    REQUIRE_FALSE(baseline_raw.contains("wp299_unknown_payload"));

    Json with_unknown = captured;
    with_unknown["wp299_unknown_payload"] = {
        {"nested_reason", "must remain visible"},
        {"future_versioned_id", "pelican.plan.future_reason@7"},
    };
    const FramePlanModel modeled = buildFramePlanModel(with_unknown.dump());
    const Json raw = Json::parse(modeled.raw_json);
    REQUIRE(raw == with_unknown);
    REQUIRE(raw.at("wp299_unknown_payload").at("nested_reason") ==
            "must remain visible");
    REQUIRE(modeled.raw_json != baseline.raw_json);
}

TEST_CASE("Devstudio frame plan model accepts only the current public contract",
          "[devstudio][frame-plan]") {
    const Json captured = Json::parse(capturedResponse());

    Json wrong_version = captured;
    wrong_version["version"] = 2;
    REQUIRE_THROWS_WITH(
        buildFramePlanModel(wrong_version.dump()),
        ContainsSubstring("requires pelican.frame_plan version 1"));

    Json duplicate_order = captured;
    duplicate_order["nodes"][1]["order"] =
        duplicate_order["nodes"][0]["order"];
    REQUIRE_THROWS_WITH(buildFramePlanModel(duplicate_order.dump()),
                        ContainsSubstring("duplicate node order"));

    Json unknown_barrier_node = captured;
    unknown_barrier_node["barriers"][0]["from"] = "missing";
    REQUIRE_THROWS_WITH(buildFramePlanModel(unknown_barrier_node.dump()),
                        ContainsSubstring("references an unknown node"));

    REQUIRE_THROWS_WITH(buildFramePlanModel("not json"),
                        ContainsSubstring("is not valid JSON"));
}
