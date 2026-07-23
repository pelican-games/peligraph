#include "../src/project/materialscreeninput.hpp"
#include "../src/project/targetplanning.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace Pelican {
namespace {

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

CompilerProviderDescriptor loweringProvider(
    std::string id = "pelican.test.vulkan_lowering@1",
    std::uint64_t owner_identity = 0,
    std::uint32_t owner_generation = 0,
    std::string content_hash = "builtin:v1") {
    return {std::move(id), CompilerProviderKind::target_lowering,
            owner_identity, owner_generation, std::move(content_hash)};
}

TargetTopologySnapshot mockTopology(bool forward_link,
                                    bool reverse_link = false) {
    TargetTopologySnapshot result{
        "mock",
        {
            TargetEndpoint{
                "device:0", TargetEndpointKind::vulkan_device,
                {"pelican.vulkan.sampled_image@1",
                 "pelican.vulkan.graphics@1"},
                {{"pelican.vulkan.profile@1", "desktop"}}},
            TargetEndpoint{
                "host", TargetEndpointKind::host,
                {"pelican.memory.host_visible@1"}, {}},
        },
        {},
    };
    if (forward_link) {
        result.links.push_back(TargetEndpointLink{
            "host_to_device", "host", "device:0",
            {"pelican.bridge.copy@1"},
            {"pelican.bridge.host_upload@1"}});
    }
    if (reverse_link) {
        result.links.push_back(TargetEndpointLink{
            "device_to_host", "device:0", "host",
            {"pelican.bridge.copy@1"},
            {"pelican.bridge.readback@1"}});
    }
    return result;
}

BackendProbeInput bridgeCandidate(std::string name,
                                  std::uint32_t scopes = 1) {
    return BackendProbeInput{
        .candidate = std::move(name),
        .provider = "pelican.test.vulkan_lowering@1",
        .endpoint = "device:0",
        .required_endpoint_capabilities =
            {"pelican.vulkan.graphics@1"},
        .bridge =
            TargetBridgeRequest{
                "host", "device:0", {"pelican.bridge.copy@1"}},
        .required_physical_features =
            {"pelican.vulkan.graphics@1"},
        .cost = BackendCostEstimate{.rendering_scopes = scopes},
    };
}

bool containsPair(std::span<const PlanningNamePair> pairs,
                  std::string_view first, std::string_view second) {
    PlanningNamePair expected{std::string{first}, std::string{second}};
    if (expected.second < expected.first) {
        std::swap(expected.first, expected.second);
    }
    return std::find(pairs.begin(), pairs.end(), expected) != pairs.end();
}

CompiledLogicalRenderGraph planningGraph() {
    CompiledLogicalRenderGraph graph;
    graph.name = "planning_fixture";
    graph.resources = {
        LogicalResourceDesc{.name = "value"},
        LogicalResourceDesc{.name = "alias_a"},
        LogicalResourceDesc{.name = "alias_b"},
        LogicalResourceDesc{.name = "alias_c"},
        LogicalResourceDesc{.name = "alias_d"},
    };

    LogicalGraphNode producer;
    producer.name = "producer";
    producer.kind = LogicalGraphNodeKind::render;
    producer.uses.push_back(
        makeLogicalWriteUse("out", LogicalValueId{"value", 1}));

    LogicalGraphNode consumer;
    consumer.name = "consumer";
    consumer.kind = LogicalGraphNodeKind::render;
    consumer.uses.push_back(makeLogicalReadUse(
        "in", LogicalValueId{"value", 1},
        LogicalReadFootprint{LogicalReadFootprintKind::same_pixel,
                             std::nullopt}));

    for (const auto *name : {"alpha", "beta", "gamma"}) {
        LogicalGraphNode node;
        node.name = name;
        node.kind = LogicalGraphNodeKind::render;
        graph.nodes.push_back(std::move(node));
    }
    graph.nodes.push_back(std::move(consumer));
    graph.nodes.push_back(std::move(producer));
    return graph;
}

std::vector<PlanningNamePair> aliasCandidates() {
    return {
        {"alias_a", "alias_b"},
        {"alias_a", "alias_c"},
        {"alias_a", "alias_d"},
        {"alias_b", "alias_c"},
        {"alias_b", "alias_d"},
        {"alias_c", "alias_d"},
    };
}

} // namespace

TEST_CASE("target topology canonicalizes data-only directed links",
          "[target-planning][topology]") {
    auto first = mockTopology(true);
    auto second = first;
    std::reverse(second.endpoints.begin(), second.endpoints.end());
    std::reverse(second.endpoints.back().capabilities.begin(),
                 second.endpoints.back().capabilities.end());

    const auto canonical = canonicalizeTargetTopology(first);
    REQUIRE(canonical.endpoints.front().id == "device:0");
    REQUIRE(canonical.links.front().source_endpoint == "host");
    REQUIRE(canonical.links.front().destination_endpoint == "device:0");
    REQUIRE(targetTopologySnapshotToJson(first).dump() ==
            targetTopologySnapshotToJson(second).dump());
    REQUIRE(findTargetEndpoint(canonical, "host") != nullptr);
    REQUIRE(findTargetEndpointLink(canonical, "host_to_device") != nullptr);

    auto invalid = first;
    invalid.links.front().destination_endpoint = "missing";
    requireThrowsContaining(
        [&] { (void)canonicalizeTargetTopology(invalid); },
        "unknown destination endpoint 'missing'");
}

TEST_CASE("compiler provider snapshots retain immutable generation leases",
          "[target-planning][registry]") {
    CompilerProviderRegistry registry;
    registry.registerProvider(
        {"pelican.test.depth_conversion@1",
         CompilerProviderKind::conversion, 0, 0, "builtin:depth"});

    std::weak_ptr<const int> retired_generation;
    {
        auto generation = std::make_shared<const int>(17);
        retired_generation = generation;
        registry.registerProvider(
            {"pelican.test.reloadable_conversion@1",
             CompilerProviderKind::conversion, 91, 7, "generation:7"},
            generation);
        const auto old_snapshot = registry.snapshot();
        REQUIRE(old_snapshot.providers().size() == 2);
        REQUIRE(old_snapshot
                    .require(CompilerProviderKind::conversion,
                             "pelican.test.reloadable_conversion@1")
                    .holdsGenerationLease());

        REQUIRE(registry.unregisterProvider(
            CompilerProviderKind::conversion,
            "pelican.test.reloadable_conversion@1"));
        generation.reset();
        REQUIRE_FALSE(retired_generation.expired());

        auto replacement = std::make_shared<const int>(18);
        registry.registerProvider(
            {"pelican.test.reloadable_conversion@1",
             CompilerProviderKind::conversion, 91, 8, "generation:8"},
            replacement);
        const auto new_snapshot = registry.snapshot();
        REQUIRE(old_snapshot
                    .require(CompilerProviderKind::conversion,
                             "pelican.test.reloadable_conversion@1")
                    .descriptor()
                    .owner_generation == 7);
        REQUIRE(new_snapshot
                    .require(CompilerProviderKind::conversion,
                             "pelican.test.reloadable_conversion@1")
                    .descriptor()
                    .owner_generation == 8);
    }
    REQUIRE(retired_generation.expired());

    requireThrowsContaining(
        [&] {
            registry.registerProvider(
                loweringProvider("pelican.test.unleased@1", 12, 1,
                                 "generation:1"));
        },
        "requires a generation lease");
    requireThrowsContaining(
        [&] {
            registry.registerProvider(
                loweringProvider("pelican.test.half_owner@1", 12, 0,
                                 "generation:1"));
        },
        "identity and generation");
}

TEST_CASE("backend probe requires a directed bridge link independently of endpoint facts",
          "[target-planning][probe]") {
    CompilerProviderRegistry registry;
    registry.registerProvider(loweringProvider());
    const auto providers = registry.snapshot();

    const auto linked = probeVulkanBackend(
        mockTopology(true), providers, bridgeCandidate("linked"));
    REQUIRE(linked.feasible);
    REQUIRE(linked.selected_link == "host_to_device");
    REQUIRE(linked.bridge_offers ==
            std::vector<std::string>{"pelican.bridge.host_upload@1"});

    const auto unlinked = probeVulkanBackend(
        mockTopology(false), providers, bridgeCandidate("unlinked"));
    REQUIRE_FALSE(unlinked.feasible);
    REQUIRE(unlinked.failures.size() == 1);
    REQUIRE(unlinked.failures.front().id ==
            "pelican.plan.directed_link_missing@1");
    REQUIRE(compilerProviderFingerprint(unlinked.provider).find(
                "pelican.test.vulkan_lowering@1") != std::string::npos);

    const auto reverse_only = probeVulkanBackend(
        mockTopology(false, true), providers,
        bridgeCandidate("reverse_only"));
    REQUIRE_FALSE(reverse_only.feasible);
    REQUIRE(reverse_only.failures.front().id ==
            "pelican.plan.directed_link_missing@1");
}

TEST_CASE("finite backend selection is order-independent and records rejected fingerprints",
          "[target-planning][selection]") {
    CompilerProviderRegistry registry;
    registry.registerProvider(loweringProvider());
    const auto providers = registry.snapshot();

    auto rejected = probeVulkanBackend(
        mockTopology(false), providers,
        bridgeCandidate("fast_but_blocked", 1));
    auto fallback_input = bridgeCandidate("portable_fallback", 4);
    fallback_input.bridge.reset();
    auto fallback = probeVulkanBackend(
        mockTopology(false), providers, std::move(fallback_input));
    REQUIRE(fallback.feasible);

    const auto first = selectBackendCandidate({rejected, fallback});
    const auto second = selectBackendCandidate({fallback, rejected});
    REQUIRE(first.selected_candidate == "portable_fallback");
    REQUIRE(backendSelectionToJson(first).dump() ==
            backendSelectionToJson(second).dump());

    const auto dump = backendSelectionToJson(first).dump();
    REQUIRE(dump.find("fast_but_blocked") != std::string::npos);
    REQUIRE(dump.find("pelican.plan.directed_link_missing@1") !=
            std::string::npos);
    REQUIRE(dump.find(compilerProviderFingerprint(rejected.provider)) !=
            std::string::npos);

    requireThrowsContaining(
        [&] { (void)selectBackendCandidate({rejected}); },
        "pelican.plan.directed_link_missing@1");
}

TEST_CASE("planning warnings stay advisory unless their stable id is strict",
          "[target-planning][diagnostics]") {
    CompilerProviderRegistry registry;
    registry.registerProvider(loweringProvider());
    const auto providers = registry.snapshot();

    auto input = bridgeCandidate("portable");
    input.diagnostics = {
        {"pelican.warning.portability@1",
         PlanningDiagnosticSeverity::warning,
         "portable", "uses a fallback representation"},
        {"pelican.warning.portability@1",
         PlanningDiagnosticSeverity::warning,
         "portable", "uses a fallback representation"},
    };
    const auto candidate =
        probeVulkanBackend(mockTopology(true), providers, std::move(input));

    const auto advisory = selectBackendCandidate({candidate});
    REQUIRE(advisory.diagnostics.size() == 1);
    REQUIRE(advisory.diagnostics.front().severity ==
            PlanningDiagnosticSeverity::warning);

    const auto unrelated_strict = selectBackendCandidate(
        {candidate},
        PlanningDiagnosticPolicy{{"pelican.warning.unrelated@1"}});
    REQUIRE(unrelated_strict.selected_candidate == "portable");

    requireThrowsContaining(
        [&] {
            (void)selectBackendCandidate(
                {candidate},
                PlanningDiagnosticPolicy{{
                    "pelican.warning.portability@1"}});
        },
        "pelican.warning.portability@1");
}

TEST_CASE("optimized planning exposes independent work while explicit constraints narrow it",
          "[target-planning][policy]") {
    const auto graph = planningGraph();
    LogicalPlanningOpportunityInput optimized_input;
    optimized_input.legal_alias_candidates = aliasCandidates();
    const auto optimized =
        analyzeLogicalPlanningOpportunities(graph, optimized_input);

    REQUIRE(containsPair(optimized.parallel_candidates, "alpha", "beta"));
    REQUIRE(containsPair(optimized.fusion_candidates, "alpha", "beta"));
    REQUIRE(optimized.alias_candidates.size() == aliasCandidates().size());
    REQUIRE(std::find(optimized.node_order.begin(),
                      optimized.node_order.end(), "producer") <
            std::find(optimized.node_order.begin(),
                      optimized.node_order.end(), "consumer"));

    auto constrained_input = optimized_input;
    constrained_input.node_constraints.push_back(
        PlanningNodeConstraint{.node = "alpha", .serial = true});
    constrained_input.resource_constraints.push_back(
        PlanningResourceConstraint{.resource = "alias_a",
                                   .no_alias = true});
    const auto constrained =
        analyzeLogicalPlanningOpportunities(graph, constrained_input);
    REQUIRE_FALSE(
        containsPair(constrained.parallel_candidates, "alpha", "beta"));
    REQUIRE(std::none_of(
        constrained.alias_candidates.begin(),
        constrained.alias_candidates.end(), [](const auto &pair) {
            return pair.first == "alias_a" ||
                   pair.second == "alias_a";
        }));

    auto conservative_input = optimized_input;
    conservative_input.profile.kind =
        PlanningProfileKind::conservative_debug;
    const auto conservative =
        analyzeLogicalPlanningOpportunities(graph, conservative_input);
    REQUIRE(conservative.parallel_candidates.empty());
    REQUIRE(conservative.fusion_candidates.empty());
    REQUIRE(conservative.alias_candidates.empty());
}

TEST_CASE("hazard stress is seed-reproducible and varies only legal decisions",
          "[target-planning][hazard-stress]") {
    const auto graph = planningGraph();
    const auto legal_aliases = aliasCandidates();
    LogicalPlanningOpportunityInput input;
    input.profile = {PlanningProfileKind::hazard_stress, 17};
    input.legal_alias_candidates = legal_aliases;

    const auto first =
        analyzeLogicalPlanningOpportunities(graph, input);
    const auto second =
        analyzeLogicalPlanningOpportunities(graph, input);
    REQUIRE(logicalPlanningOpportunityReportToJson(first).dump() ==
            logicalPlanningOpportunityReportToJson(second).dump());

    std::set<std::string> schedules;
    std::set<std::string> aliases;
    for (std::uint64_t seed = 1; seed <= 24; ++seed) {
        input.profile.seed = seed;
        const auto report =
            analyzeLogicalPlanningOpportunities(graph, input);
        schedules.insert(
            nlohmann::ordered_json(report.node_order).dump());
        aliases.insert(
            logicalPlanningOpportunityReportToJson(report)
                .at("alias_candidates")
                .dump());
        REQUIRE(std::find(report.node_order.begin(),
                          report.node_order.end(), "producer") <
                std::find(report.node_order.begin(),
                          report.node_order.end(), "consumer"));
        for (const auto &pair : report.alias_candidates) {
            REQUIRE(containsPair(legal_aliases, pair.first,
                                 pair.second));
        }
    }
    REQUIRE(schedules.size() > 1);
    REQUIRE(aliases.size() > 1);

    requireThrowsContaining(
        [&] {
            LogicalPlanningOpportunityInput invalid;
            invalid.profile = {PlanningProfileKind::optimized, 1};
            (void)analyzeLogicalPlanningOpportunities(graph, invalid);
        },
        "only valid for hazard_stress");
}

TEST_CASE("screen input declaration must match reflected set shape",
          "[target-planning][shader-interface]") {
    const std::vector<MaterialScreenInputReflectionBinding> valid{
        {0, 0, MaterialScreenInputReflectionKind::unsupported},
        {1, 1,
         MaterialScreenInputReflectionKind::combined_image_sampler},
        {1, 0,
         MaterialScreenInputReflectionKind::combined_image_sampler},
    };
    REQUIRE_NOTHROW(
        validateMaterialScreenInputInterfaceReflection(2, valid, 1));

    auto missing = valid;
    missing.erase(missing.begin() + 1);
    requireThrowsContaining(
        [&] {
            validateMaterialScreenInputInterfaceReflection(
                2, missing, 1);
        },
        "binding count");

    auto wrong_type = valid;
    wrong_type.back().kind =
        MaterialScreenInputReflectionKind::unsupported;
    requireThrowsContaining(
        [&] {
            validateMaterialScreenInputInterfaceReflection(
                2, wrong_type, 1);
        },
        "consecutive combined image samplers");

    auto gap = valid;
    gap[1].binding = 2;
    requireThrowsContaining(
        [&] {
            validateMaterialScreenInputInterfaceReflection(2, gap, 1);
        },
        "consecutive combined image samplers");
}

} // namespace Pelican
