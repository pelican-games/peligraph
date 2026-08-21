#include "frameplanmodel.hpp"
#include "executionplanwire.hpp"
#include "frameresolutionwire.hpp"
#include "physicaltargetplanwire.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/core/renderingpass/frameplanner.hpp"
#include "../src/project/renderpipeline.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <ranges>
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

std::string capturedResponseText() {
    std::ifstream stream{PELICAN_TEST_FRAME_PLAN_FIXTURE, std::ios::binary};
    if (!stream) {
        throw std::runtime_error(
            "failed to open captured get_frame_plan fixture");
    }
    return std::string{std::istreambuf_iterator<char>{stream},
                       std::istreambuf_iterator<char>{}};
}

Pelican::ResourceExtentPlan extentPlanFromJson(const Json &extent) {
    const auto kind = extent.at("kind").get<std::string>();
    if (kind == "fixed") {
        return Pelican::ResourceExtentPlan{
            .kind = Pelican::ResourceExtentKind::fixed,
            .width = extent.at("width").get<std::uint32_t>(),
            .height = extent.at("height").get<std::uint32_t>(),
        };
    }
    if (kind != "output_relative") {
        throw std::runtime_error(
            "captured physical resource has unknown extent kind");
    }
    return Pelican::ResourceExtentPlan{
        .kind = Pelican::ResourceExtentKind::output_relative,
        .scale_x = extent.at("scale_x").get<float>(),
        .scale_y = extent.at("scale_y").get<float>(),
    };
}

Pelican::FrameRuntimeResolutionWire capturedRuntimeResolution(
    const Json &captured) {
    const Json &physical = captured.at("physical_target_plan");
    const Json &resolution = physical.at("resolution_plan");
    constexpr Pelican::ResolvedResourceExtent output_extent{160, 90};

    Pelican::FrameRuntimeResolutionWire result;
    result.render_source_resource =
        resolution.at("render_source_resource").get<std::string>();
    result.output_source_resource =
        resolution.at("output_source_resource").get<std::string>();

    std::map<std::string, Pelican::ResolvedResourceExtent, std::less<>>
        resolved_resources;
    for (const auto &resource : physical.at("resources")) {
        const auto name =
            resource.at("logical_resource").get<std::string>();
        const auto extent = Pelican::resolveResourceExtent(
            extentPlanFromJson(resource.at("extent")), output_extent);
        resolved_resources.emplace(name, extent);
        result.resources.push_back({name, extent});
    }
    const auto resolve_source =
        [&](std::string_view source) {
            if (source == "swapchain") {
                return output_extent;
            }
            const auto found = resolved_resources.find(source);
            if (found == resolved_resources.end()) {
                throw std::runtime_error(
                    "captured resolution source has no runtime extent: " +
                    std::string{source});
            }
            return found->second;
        };
    result.render_extent = resolve_source(result.render_source_resource);
    result.output_extent = resolve_source(result.output_source_resource);
    return result;
}

std::string capturedResponse() {
    Json captured = Json::parse(capturedResponseText());
    captured["runtime_resolution"] =
        Pelican::frameRuntimeResolutionWireToJson(
            capturedRuntimeResolution(captured));
    return captured.dump();
}

std::filesystem::path sourceRoot() {
    auto path = std::filesystem::path{PELICAN_TEST_FRAME_PLAN_FIXTURE};
    for (int level = 0; level < 4; ++level) {
        path = path.parent_path();
    }
    return path;
}

Json readJson(const std::filesystem::path &path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        throw std::runtime_error("failed to open " + path.string());
    }
    return Json::parse(stream);
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

const Json *findResource(const Json &plan, std::string_view name) {
    const auto &resources = plan.at("resources");
    const auto found = std::find_if(
        resources.begin(), resources.end(),
        [&](const Json &resource) {
            return resource.value("name", std::string{}) == name;
        });
    return found == resources.end() ? nullptr : &*found;
}

Json *findResource(Json &plan, std::string_view name) {
    auto &resources = plan.at("resources");
    const auto found = std::find_if(
        resources.begin(), resources.end(),
        [&](const Json &resource) {
            return resource.value("name", std::string{}) == name;
        });
    return found == resources.end() ? nullptr : &*found;
}

Json exampleFramePlanFromProducer() {
    const Json authored = readJson(
        sourceRoot() / "projects" / "example" / "passes" /
        "main_rendering_config.json");
    const auto resolved = Pelican::resolveRenderPipeline(
        Pelican::RenderPipelineRequest{
            .authored_config = authored,
            .source_name = "projects/example",
        },
        Pelican::RenderEnvironmentCapabilities{
            // This producer fixture tests resource-usage projection, not the
            // independently-covered shader availability gate.
            .runtime_shader_compiler_enabled = true,
        },
        Pelican::RenderPipelineResolveDependencies{
            .load_feature_json = [](std::string_view reference) {
                constexpr std::string_view prefix = "engine://";
                if (!reference.starts_with(prefix)) {
                    throw std::runtime_error(
                        "projects/example requested a non-engine feature");
                }
                return Pelican::engineResourceOrThrow(
                    reference.substr(prefix.size()));
            },
        });
    const auto graphs =
        Pelican::parseFrameGraphDefinitionsFromConfigJson(
            resolved.normalized_config);
    if (graphs.size() != 1) {
        throw std::runtime_error(
            "projects/example did not resolve to one frame graph");
    }
    const auto compiled = Pelican::compileRenderPipeline(resolved);
    return Pelican::framePlanToJson(
        Pelican::planFrameGraph(graphs.front()), &compiled);
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
    "WP323 carries resolved resource usage into the Studio frame-plan model",
    "[devstudio][frame-plan][usage][wp323][negative-contrast]") {
    const Json produced = exampleFramePlanFromProducer();
    const Json *wire_offscreen = findResource(produced, "offscreen_depth");
    const Json *wire_opaque = findResource(produced, "opaque_depth");
    const Json *wire_normal = findResource(produced, "gbuffer_normal");
    REQUIRE(wire_offscreen != nullptr);
    REQUIRE(wire_opaque != nullptr);
    REQUIRE(wire_normal != nullptr);
    REQUIRE(wire_offscreen->at("kind") == wire_opaque->at("kind"));
    REQUIRE(wire_offscreen->at("usage").get<std::vector<std::string>>() ==
            std::vector<std::string>{
                "DEPTH_STENCIL_ATTACHMENT", "TRANSFER_SRC"});
    REQUIRE(wire_opaque->at("usage").get<std::vector<std::string>>() ==
            std::vector<std::string>{"TRANSFER_DST", "SAMPLED"});
    REQUIRE(wire_normal->at("usage").get<std::vector<std::string>>() ==
            std::vector<std::string>{
                "COLOR_ATTACHMENT", "SAMPLED"});

    const FramePlanModel model = buildFramePlanModel(produced.dump());
    const FramePlanResource *offscreen =
        findResource(model, "offscreen_depth");
    const FramePlanResource *opaque =
        findResource(model, "opaque_depth");
    const FramePlanResource *normal =
        findResource(model, "gbuffer_normal");
    REQUIRE(offscreen != nullptr);
    REQUIRE(opaque != nullptr);
    REQUIRE(normal != nullptr);
    REQUIRE(offscreen->kind == opaque->kind);
    REQUIRE(offscreen->usage.has_value());
    REQUIRE(*offscreen->usage == std::vector<std::string>{
                                     "DEPTH_STENCIL_ATTACHMENT",
                                     "TRANSFER_SRC"});
    REQUIRE(opaque->usage.has_value());
    REQUIRE(*opaque->usage ==
            std::vector<std::string>{"TRANSFER_DST", "SAMPLED"});
    REQUIRE(normal->usage.has_value());
    REQUIRE(*normal->usage == std::vector<std::string>{
                                 "COLOR_ATTACHMENT", "SAMPLED"});

    const Json *wire_display = findResource(produced, "display");
    const Json *wire_swapchain = findResource(produced, "swapchain");
    REQUIRE(wire_display != nullptr);
    REQUIRE(wire_swapchain != nullptr);
    REQUIRE(wire_display->at("source") == "engine");
    REQUIRE(wire_display->at("usage").get<std::vector<std::string>>() ==
            std::vector<std::string>{
                "COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"});
    REQUIRE(wire_swapchain->at("source") == "engine");
    REQUIRE_FALSE(wire_swapchain->contains("usage"));
    const FramePlanResource *display = findResource(model, "display");
    const FramePlanResource *swapchain = findResource(model, "swapchain");
    REQUIRE(display != nullptr);
    REQUIRE(swapchain != nullptr);
    REQUIRE(display->usage.has_value());
    REQUIRE(*display->usage == std::vector<std::string>{
                                  "COLOR_ATTACHMENT", "SAMPLED",
                                  "TRANSFER_SRC"});
    REQUIRE_FALSE(swapchain->usage.has_value());

    Json legacy = produced;
    for (auto &resource : legacy.at("resources")) {
        resource.erase("usage");
    }
    const FramePlanModel legacy_model =
        buildFramePlanModel(legacy.dump());
    const FramePlanResource *legacy_offscreen =
        findResource(legacy_model, "offscreen_depth");
    REQUIRE(legacy_offscreen != nullptr);
    REQUIRE_FALSE(legacy_offscreen->usage.has_value());

    Json explicit_empty = legacy;
    Json *empty_offscreen =
        findResource(explicit_empty, "offscreen_depth");
    REQUIRE(empty_offscreen != nullptr);
    (*empty_offscreen)["usage"] = Json::array();
    const FramePlanModel empty_model =
        buildFramePlanModel(explicit_empty.dump());
    const FramePlanResource *modeled_empty =
        findResource(empty_model, "offscreen_depth");
    REQUIRE(modeled_empty != nullptr);
    REQUIRE(modeled_empty->usage.has_value());
    REQUIRE(modeled_empty->usage->empty());

    Json malformed = produced;
    Json *malformed_offscreen =
        findResource(malformed, "offscreen_depth");
    REQUIRE(malformed_offscreen != nullptr);
    (*malformed_offscreen)["usage"] = "SAMPLED";
    REQUIRE_THROWS_WITH(
        buildFramePlanModel(malformed.dump()),
        ContainsSubstring(
            "field 'usage' must be an array of strings"));
}

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

    const auto runtime_resolution =
        Pelican::frameRuntimeResolutionWireFromJson(
            captured.at("runtime_resolution"));
    std::map<std::string, Pelican::ResolvedResourceExtent, std::less<>>
        runtime_extents;
    for (const auto &resource : runtime_resolution.resources) {
        REQUIRE(runtime_extents.emplace(
                    resource.resource, resource.extent)
                    .second);
    }

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
        const auto runtime_extent = runtime_extents.find(name);
        REQUIRE(runtime_extent != runtime_extents.end());
        REQUIRE(*resource->width == runtime_extent->second.width);
        REQUIRE(*resource->height == runtime_extent->second.height);
    }
    REQUIRE(physical_target_count == 22);
    REQUIRE(resolved_extent_count == 22);
    REQUIRE(runtime_extents.size() == 22);

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
    "Shipping four-project target corpus remains 22 runtime-resolvable targets",
    "[devstudio][frame-plan][wp315][shipping-regression]") {
    const auto root = sourceRoot();
    const Json animgraph = readJson(
        root / "projects" / "animgraph_demo" / "passes" / "main.json");
    REQUIRE(animgraph.at("pipeline").at("preset") ==
            "engine://render_pipelines/hybrid_v1.json");

    const Json hybrid = readJson(
        root / "src" / "core" / "resources" / "render_pipelines" /
        "hybrid_v1.json");
    const std::array<Json, 4> target_documents{
        hybrid.at("config"),
        readJson(root / "projects" / "example" / "passes" /
                 "main_rendering_config.json"),
        readJson(root / "projects" / "sprite_demo" / "passes" /
                 "main.json"),
        readJson(root / "projects" / "vrm_xr_demo" / "passes" /
                 "main.json"),
    };

    std::map<std::string, float, std::less<>> target_scales;
    constexpr std::array allowed_scales{
        1.0f, 0.5f, 0.25f, 0.125f, 0.0625f};
    for (const auto &document : target_documents) {
        for (const auto &target : document.at("render_targets")) {
            const auto name = target.at("name").get<std::string>();
            const auto scale = target.at("extent_scale").get<float>();
            REQUIRE(std::ranges::find(allowed_scales, scale) !=
                    allowed_scales.end());
            const auto [found, inserted] =
                target_scales.emplace(name, scale);
            if (!inserted) {
                REQUIRE(found->second == scale);
            }
        }
    }
    REQUIRE(target_scales.size() == 21);
    REQUIRE(target_scales.emplace("swapchain", 1.0f).second);
    REQUIRE(target_scales.size() == 22);

    for (const auto &[name, scale] : target_scales) {
        INFO("shipping target: " << name);
        const auto extent = Pelican::resolveResourceExtent(
            Pelican::ResourceExtentPlan{
                .kind = Pelican::ResourceExtentKind::output_relative,
                .scale_x = scale,
                .scale_y = scale,
            },
            Pelican::ResolvedResourceExtent{160, 90});
        REQUIRE(extent.width > 0);
        REQUIRE(extent.height > 0);
    }
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
