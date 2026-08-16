#include "frameplanmodel.hpp"
#include "executionplanwire.hpp"
#include "physicaltargetplanwire.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
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

std::size_t adoptedAliasCandidateCount(const FramePlanModel &model) {
    return static_cast<std::size_t>(std::count_if(
        model.physical_plan.alias_candidates.begin(),
        model.physical_plan.alias_candidates.end(),
        [](const FramePlanOpportunityPair &candidate) {
            return candidate.adopted;
        }));
}

const FramePlanOpportunityPair *findAliasCandidate(
    const FramePlanModel &model, std::string_view first,
    std::string_view second) {
    const auto found = std::find_if(
        model.physical_plan.alias_candidates.begin(),
        model.physical_plan.alias_candidates.end(),
        [&](const FramePlanOpportunityPair &candidate) {
            return candidate.first == first && candidate.second == second;
        });
    return found == model.physical_plan.alias_candidates.end() ? nullptr
                                                               : &*found;
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
    "Devstudio retains the complete physical wire projection and resolves every captured target extent",
    "[devstudio][frame-plan][wp305][physical-plan]") {
    const Json captured = Json::parse(capturedResponse());
    const Json &physical = captured.at("physical_target_plan");
    const FramePlanModel model = buildFramePlanModel(captured.dump());

    REQUIRE(model.execution_plan.available());
    REQUIRE(model.execution_plan.unavailable_reason.empty());
    REQUIRE(model.physical_plan.available());
    REQUIRE(model.physical_plan.unavailable_reason.empty());
    REQUIRE(model.physical_plan.graph == model.graph);
    REQUIRE(model.physical_plan.output_width == 160);
    REQUIRE(model.physical_plan.output_height == 90);
    REQUIRE(model.physical_plan.alias_groups.size() ==
            physical.at("alias_groups").size());
    REQUIRE(model.physical_plan.scopes.size() == physical.at("scopes").size());
    for (std::size_t index = 0;
         index < model.physical_plan.scopes.size(); ++index) {
        REQUIRE(model.physical_plan.scopes[index].local_reads ==
                physical.at("scopes")
                    .at(index)
                    .at("local_reads")
                    .get<std::vector<std::string>>());
    }
    REQUIRE(model.physical_plan.lowering_nodes.size() ==
            physical.at("lowering_graph").at("nodes").size());
    REQUIRE(model.dependencies.size() ==
            captured.at("execution_plan").at("dependencies").size());
    REQUIRE(model.physical_plan.resolution_plan.has_value());
    REQUIRE(model.physical_plan.resolution_plan->scene_resources ==
            physical.at("resolution_plan")
                .at("scene_resources")
                .get<std::vector<std::string>>());

    REQUIRE(model.gpu_resource_arena.has_value());
    REQUIRE(model.gpu_resource_arena->resource_count == 89);
    REQUIRE(model.gpu_resource_arena->scopes.size() ==
            captured.at("gpu_resource_arena").at("scopes").size());
    REQUIRE(model.gpu_resource_arena->scopes.front().resources.size() == 89);

    const FramePlanResource *display = findResource(model, "display");
    REQUIRE(display != nullptr);
    REQUIRE(display->extent.has_value());
    REQUIRE(display->extent->kind == "fixed");
    REQUIRE(display->width == 160);
    REQUIRE(display->height == 90);

    std::size_t physical_target_count = 0;
    std::size_t resolved_extent_count = 0;
    for (const auto &published : physical.at("resources")) {
        ++physical_target_count;
        const std::string name =
            published.at("logical_resource").get<std::string>();
        const FramePlanResource *resource = findResource(model, name);
        REQUIRE(resource != nullptr);
        REQUIRE(resource->extent.has_value());
        REQUIRE(resource->width.has_value());
        REQUIRE(resource->height.has_value());
        ++resolved_extent_count;

        const Json &extent = published.at("extent");
        REQUIRE(resource->extent->kind == extent.at("kind").get<std::string>());
        REQUIRE(resource->extent->scale_x ==
                extent.at("scale_x").get<double>());
        REQUIRE(resource->extent->scale_y ==
                extent.at("scale_y").get<double>());
        const std::size_t expected_width =
            resource->extent->kind == "fixed"
                ? extent.at("width").get<std::size_t>()
                : static_cast<std::size_t>(
                      160.0 * extent.at("scale_x").get<double>());
        const std::size_t expected_height =
            resource->extent->kind == "fixed"
                ? extent.at("height").get<std::size_t>()
                : static_cast<std::size_t>(
                      90.0 * extent.at("scale_y").get<double>());
        REQUIRE(*resource->width == expected_width);
        REQUIRE(*resource->height == expected_height);
    }
    REQUIRE(physical_target_count == 22);
    REQUIRE(resolved_extent_count == 22);

    REQUIRE(model.physical_plan.alias_groups.size() == 1);
    REQUIRE(model.physical_plan.alias_candidates.size() == 2);
    REQUIRE(adoptedAliasCandidateCount(model) == 1);
    const auto *adopted = findAliasCandidate(
        model, "Bloom_Threshold_RT", "g_emissive");
    const auto *not_adopted = findAliasCandidate(
        model, "Bloom_Threshold_RT", "gbuffer_albedo");
    REQUIRE(adopted != nullptr);
    REQUIRE(adopted->adopted);
    REQUIRE(not_adopted != nullptr);
    REQUIRE_FALSE(not_adopted->adopted);
    REQUIRE(findResource(model, "Bloom_Threshold_RT")->alias_group ==
            "alias:0");
    REQUIRE(findResource(model, "g_emissive")->alias_group == "alias:0");
    REQUIRE(findResource(model, "gbuffer_albedo")->alias_group.empty());

    // Key-set conformance: every current producer section is retained with its
    // exact value, while the WP305-promoted collections above are also typed.
    std::map<std::string, Json, std::less<>> retained;
    for (const auto &section : model.physical_plan.wire_sections) {
        REQUIRE(retained.emplace(section.name, Json::parse(section.json)).second);
    }
    REQUIRE(retained.size() == physical.size());
    for (const auto &[name, value] : physical.items()) {
        REQUIRE(retained.contains(name));
        REQUIRE(retained.at(name) == value);
    }
}

TEST_CASE(
    "Devstudio distinguishes a missing execution plan from an available zero-dependency plan",
    "[devstudio][frame-plan][wp317][negative-contrast]") {
    const Json captured = Json::parse(capturedResponse());

    Json missing = captured;
    missing.erase("execution_plan");
    const FramePlanModel unavailable = buildFramePlanModel(missing.dump());
    REQUIRE_FALSE(unavailable.execution_plan.available());
    REQUIRE(unavailable.execution_plan.unavailable_reason_code ==
            "execution_plan_missing");
    REQUIRE_THAT(unavailable.execution_plan.unavailable_reason,
                 ContainsSubstring("execution_plan was not published"));
    REQUIRE(unavailable.dependencies.empty());

    Json null_plan = captured;
    null_plan["execution_plan"] = nullptr;
    const FramePlanModel null_unavailable =
        buildFramePlanModel(null_plan.dump());
    REQUIRE_FALSE(null_unavailable.execution_plan.available());
    REQUIRE(null_unavailable.execution_plan.unavailable_reason_code ==
            unavailable.execution_plan.unavailable_reason_code);

    Json zero_dependencies = captured;
    zero_dependencies["execution_plan"]["dependencies"] = Json::array();
    const FramePlanModel available =
        buildFramePlanModel(zero_dependencies.dump());
    REQUIRE(available.execution_plan.available());
    REQUIRE(available.execution_plan.unavailable_reason.empty());
    REQUIRE(available.dependencies.empty());

    const auto shared_missing = Pelican::validateExecutionPlanWire(nullptr);
    REQUIRE_FALSE(shared_missing.available());
    REQUIRE(shared_missing.reason_code ==
            unavailable.execution_plan.unavailable_reason_code);
}

TEST_CASE(
    "Devstudio distinguishes a missing physical plan from an available zero-alias plan",
    "[devstudio][frame-plan][wp305][negative-contrast]") {
    const Json captured = Json::parse(capturedResponse());

    Json missing = captured;
    missing.erase("physical_target_plan");
    const FramePlanModel unavailable = buildFramePlanModel(missing.dump());
    REQUIRE_FALSE(unavailable.physical_plan.available());
    REQUIRE(unavailable.physical_plan.unavailable_reason_code ==
            "physical_plan_missing");
    REQUIRE_THAT(unavailable.physical_plan.unavailable_reason,
                 ContainsSubstring("physical_target_plan was not published"));

    Json zero_alias = captured;
    zero_alias["physical_target_plan"]["alias_groups"] = Json::array();
    const FramePlanModel available = buildFramePlanModel(zero_alias.dump());
    REQUIRE(available.physical_plan.available());
    REQUIRE(available.physical_plan.unavailable_reason.empty());
    REQUIRE(available.physical_plan.alias_groups.empty());
    REQUIRE(available.physical_plan.alias_candidates.size() == 2);
    REQUIRE(adoptedAliasCandidateCount(available) == 0);

    const auto shared_missing =
        Pelican::validatePhysicalTargetPlanWire(nullptr);
    REQUIRE_FALSE(shared_missing.available());
    REQUIRE(shared_missing.reason_code ==
            unavailable.physical_plan.unavailable_reason_code);
}

TEST_CASE(
    "Physical plan conformance rejects named mismatches duplicates and missing references",
    "[devstudio][frame-plan][wp305][validation]") {
    const Json captured = Json::parse(capturedResponse());

    const auto unavailable_code = [&](Json input) {
        const FramePlanModel model = buildFramePlanModel(input.dump());
        REQUIRE_FALSE(model.physical_plan.available());
        return model.physical_plan.unavailable_reason_code;
    };

    SECTION("schema") {
        Json input = captured;
        input["physical_target_plan"]["schema"] = "pelican.wrong";
        REQUIRE(unavailable_code(std::move(input)) ==
                "physical_plan_schema_mismatch");
    }
    SECTION("version") {
        Json input = captured;
        input["physical_target_plan"]["version"] = 2;
        REQUIRE(unavailable_code(std::move(input)) ==
                "physical_plan_version_mismatch");
    }
    SECTION("graph") {
        Json input = captured;
        input["physical_target_plan"]["graph"] = "another_graph";
        REQUIRE(unavailable_code(std::move(input)) ==
                "physical_plan_graph_mismatch");
    }
    SECTION("fingerprint") {
        Json input = captured;
        input["physical_target_plan"]["ejectable_complete_physical_plan"]
             ["logical_graph_fingerprint"] = "fnv1a64:0000000000000000";
        REQUIRE(unavailable_code(std::move(input)) ==
                "physical_plan_fingerprint_mismatch");
    }
    SECTION("duplicate physical resource") {
        Json input = captured;
        input["physical_target_plan"]["resources"].push_back(
            input["physical_target_plan"]["resources"].front());
        REQUIRE(unavailable_code(std::move(input)) ==
                "physical_plan_duplicate");
    }
    SECTION("unknown attachment node") {
        Json input = captured;
        input["physical_target_plan"]["attachments"][0]["node"] =
            "missing_node";
        REQUIRE(unavailable_code(std::move(input)) ==
                "physical_plan_missing_reference");
    }
    SECTION("unknown scope local-read resource") {
        Json input = captured;
        input["physical_target_plan"]["scopes"][0]["local_reads"] =
            Json::array({"missing_resource"});
        REQUIRE(unavailable_code(std::move(input)) ==
                "physical_plan_missing_reference");
    }
    SECTION("alias membership conflict") {
        Json input = captured;
        input["physical_target_plan"]["alias_groups"].push_back(
            Json{{"id", "alias:conflict"},
                 {"resources",
                  {"Bloom_Threshold_RT", "gbuffer_albedo"}}});
        REQUIRE(unavailable_code(std::move(input)) ==
                "physical_plan_alias_membership_conflict");
    }
    SECTION("alias resource contract conflict") {
        Json input = captured;
        for (auto &resource :
             input["physical_target_plan"]["resources"]) {
            if (resource["logical_resource"] == "g_emissive") {
                resource["format"] = "R8_UNORM";
            }
        }
        REQUIRE(unavailable_code(std::move(input)) ==
                "physical_plan_alias_membership_conflict");
    }
    SECTION("alias lifetime conflict") {
        Json input = captured;
        Json lifetime;
        for (const auto &resource :
             input["physical_target_plan"]["resources"]) {
            if (resource["logical_resource"] == "Bloom_Threshold_RT") {
                lifetime = resource["lifetime"];
            }
        }
        for (auto &resource :
             input["physical_target_plan"]["resources"]) {
            if (resource["logical_resource"] == "g_emissive") {
                resource["lifetime"] = lifetime;
            }
        }
        REQUIRE(unavailable_code(std::move(input)) ==
                "physical_plan_alias_membership_conflict");
    }
}

TEST_CASE(
    "Execution plan wire conformance reports named unavailable results",
    "[devstudio][frame-plan][wp305][wp317][validation]") {
    const Json captured = Json::parse(capturedResponse());

    const auto unavailable_code = [&](Json input) {
        const FramePlanModel model = buildFramePlanModel(input.dump());
        REQUIRE_FALSE(model.execution_plan.available());
        REQUIRE(model.dependencies.empty());
        return model.execution_plan.unavailable_reason_code;
    };

    SECTION("unknown execution node") {
        Json input = captured;
        input["execution_plan"]["nodes"][0]["name"] = "missing_node";
        REQUIRE(unavailable_code(std::move(input)) ==
                "execution_plan_missing_reference");
    }
    SECTION("duplicate execution node") {
        Json input = captured;
        input["execution_plan"]["nodes"][1]["name"] =
            input["execution_plan"]["nodes"][0]["name"];
        REQUIRE(unavailable_code(std::move(input)) ==
                "execution_plan_duplicate");
    }
    SECTION("unknown dependency node") {
        Json input = captured;
        input["execution_plan"]["dependencies"][0]["to"] = "missing_node";
        REQUIRE(unavailable_code(std::move(input)) ==
                "execution_plan_missing_reference");
    }
    SECTION("schema") {
        Json input = captured;
        input["execution_plan"]["schema"] = "pelican.wrong";
        REQUIRE(unavailable_code(std::move(input)) ==
                "execution_plan_schema_mismatch");
    }
    SECTION("version") {
        Json input = captured;
        input["execution_plan"]["schema_version"] = 2;
        REQUIRE(unavailable_code(std::move(input)) ==
                "execution_plan_version_mismatch");
    }
    SECTION("graph") {
        Json input = captured;
        input["execution_plan"]["graph"] = "another_graph";
        REQUIRE(unavailable_code(std::move(input)) ==
                "execution_plan_graph_mismatch");
    }
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
