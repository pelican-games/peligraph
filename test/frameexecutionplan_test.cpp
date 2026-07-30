#include "../src/core/renderingpass/frameexecutionadapter.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <string_view>

namespace Pelican {
namespace {

FrameGraphDefinition mixedGraph() {
    FrameGraphDefinition graph;
    graph.name = "mixed";
    graph.declared_resources = {
        "gbuffer", "lighting", "history"};
    graph.nodes = {
        FrameGraphNodeDefinition{
            .name = "geometry",
            .kind = FramePlanNodeKind::render,
            .declaration_index = 0,
            .writes = {"gbuffer"},
            .attachments =
                {FrameGraphAttachmentDefinition{
                    .resource = "gbuffer",
                }},
        },
        FrameGraphNodeDefinition{
            .name = "lighting",
            .kind = FramePlanNodeKind::compute,
            .declaration_index = 1,
            .reads = {"gbuffer"},
            .read_footprints =
                {FrameGraphReadFootprintDefinition{
                    .resource = "gbuffer",
                    .footprint =
                        {LogicalReadFootprintKind::
                             same_pixel,
                         std::nullopt},
                }},
            .resource_accesses =
                {
                    FrameGraphResourceAccessDefinition{
                        .resource = "gbuffer",
                        .intent =
                            LogicalAccessIntent::sampled,
                    },
                    FrameGraphResourceAccessDefinition{
                        .resource = "lighting",
                        .intent =
                            LogicalAccessIntent::storage,
                    },
                },
            .writes = {"lighting"},
            .after = {"geometry"},
        },
        FrameGraphNodeDefinition{
            .name = "snapshot",
            .kind =
                FramePlanNodeKind::snapshot_copy,
            .declaration_index = 2,
            .reads = {"lighting"},
            .writes = {"history"},
            .snapshot_after = "lighting",
        },
    };
    return graph;
}

ExecutionEndpoint testEndpoint(
    std::vector<std::string> capabilities = {}) {
    return ExecutionEndpoint{
        .id = "device:test",
        .endpoint_class =
            ExecutionEndpointClass::device,
        .backend = "vulkan",
        .capabilities =
            std::move(capabilities),
    };
}

const ExecutionResourceUse &requireUse(
    const FrameExecutionNode &node,
    std::string_view resource,
    ExecutionResourceEpoch epoch) {
    const auto found = std::find_if(
        node.resource_uses.begin(),
        node.resource_uses.end(),
        [&](const auto &use) {
            return use.resource == resource &&
                   use.epoch == epoch;
        });
    REQUIRE(found != node.resource_uses.end());
    return *found;
}

} // namespace

TEST_CASE(
    "frame execution adapter maps render compute and copy "
    "onto one open endpoint vocabulary",
    "[wp238][execution-plan]") {
    const auto graph = mixedGraph();
    const auto frame = planFrameGraph(graph);
    const auto execution =
        compileFrameExecutionPlan(
            graph, frame,
            testEndpoint({
                "pelican.vulkan.storage_buffer@1",
                "pelican.vulkan.graphics@1",
            }));

    REQUIRE_NOTHROW(
        validateFrameExecutionPlan(execution));
    REQUIRE(execution.nodes.size() == 3);
    CHECK(execution.nodes[0].semantic_dialect ==
          "pelican.logical.render@1");
    CHECK(execution.nodes[1].semantic_dialect ==
          "pelican.logical.compute@1");
    CHECK(execution.nodes[2].semantic_dialect ==
          "pelican.logical.transfer@1");
    CHECK(execution.nodes[0].selected_endpoint ==
          "device:test");
    CHECK(execution.nodes[1].required_capabilities ==
          std::vector<std::string>{
              "pelican.execution.compute@1"});
    CHECK(execution.nodes[2].required_capabilities ==
          std::vector<std::string>{
              "pelican.execution.transfer@1"});

    const auto &gbuffer =
        requireUse(
            execution.nodes[1], "gbuffer",
            ExecutionResourceEpoch::current);
    CHECK(gbuffer.access ==
          LogicalAccessMode::read);
    CHECK(gbuffer.intent ==
          LogicalAccessIntent::sampled);
    CHECK(gbuffer.footprint.kind ==
          LogicalReadFootprintKind::same_pixel);
    const auto &snapshot_source =
        requireUse(
            execution.nodes[2], "lighting",
            ExecutionResourceEpoch::current);
    CHECK(snapshot_source.intent ==
          LogicalAccessIntent::transfer);

    const auto encoded =
        frameExecutionPlanToJson(execution);
    CHECK(encoded.at("schema") ==
          "pelican.frame_execution_plan");
    CHECK(encoded.at("nodes").at(1)
              .at("selected_implementation") ==
          "pelican.execution.frame_compute@1");
    CHECK(encoded.at("fingerprint")
              .get<std::string>()
              .starts_with("fnv1a64:"));
    CHECK(std::count_if(
              execution.dependencies.begin(),
              execution.dependencies.end(),
              [](const auto &dependency) {
                  return dependency.from ==
                             "lighting" &&
                         dependency.to ==
                             "snapshot";
              }) == 2);
}

TEST_CASE(
    "frame execution plans canonicalize deterministically "
    "without requiring one connected component",
    "[wp238][execution-plan]") {
    const auto graph = mixedGraph();
    const auto frame = planFrameGraph(graph);
    const auto lhs =
        compileFrameExecutionPlan(
            graph, frame,
            testEndpoint({
                "pelican.vulkan.storage_buffer@1",
                "pelican.vulkan.graphics@1",
                "pelican.vulkan.graphics@1",
            }));
    const auto rhs =
        compileFrameExecutionPlan(
            graph, frame,
            testEndpoint({
                "pelican.vulkan.graphics@1",
                "pelican.vulkan.storage_buffer@1",
            }));
    CHECK(lhs == rhs);
    CHECK(lhs.fingerprint ==
          frameExecutionPlanFingerprint(lhs));

    FrameGraphDefinition disconnected;
    disconnected.name = "disconnected";
    disconnected.nodes = {
        FrameGraphNodeDefinition{
            .name = "left",
            .kind = FramePlanNodeKind::anchor,
            .declaration_index = 0,
        },
        FrameGraphNodeDefinition{
            .name = "right",
            .kind = FramePlanNodeKind::anchor,
            .declaration_index = 1,
        },
    };
    const auto disconnected_frame =
        planFrameGraph(disconnected);
    const auto disconnected_execution =
        compileFrameExecutionPlan(
            disconnected, disconnected_frame,
            testEndpoint());
    CHECK(disconnected_execution.nodes.size() ==
          2);
    CHECK(
        disconnected_execution.dependencies.empty());
    REQUIRE_NOTHROW(
        validateFrameExecutionPlan(
            disconnected_execution));
}

TEST_CASE(
    "generic raster dialect remains visible in the common execution plan",
    "[wp238][execution-plan][raster]") {
    FrameGraphDefinition graph;
    graph.name = "generic_raster";
    graph.declared_resources = {"color"};
    graph.nodes = {
        FrameGraphNodeDefinition{
            .name = "custom_draw",
            .kind = FramePlanNodeKind::render,
            .declaration_index = 0,
            .writes = {"color"},
            .attachments =
                {FrameGraphAttachmentDefinition{
                    .resource = "color",
                }},
            .raster_geometry = true,
            .semantic_dialect =
                "pelican.logical.raster@1",
            .execution_implementation =
                "pelican.execution.generic_raster_direct@1",
        },
    };
    const auto frame = planFrameGraph(graph);
    const auto execution =
        compileFrameExecutionPlan(
            graph, frame,
            testEndpoint());

    REQUIRE(execution.nodes.size() == 1);
    CHECK(
        execution.nodes.front().semantic_dialect ==
        "pelican.logical.raster@1");
    CHECK(
        execution.nodes.front()
            .selected_implementation ==
        "pelican.execution.generic_raster_direct@1");
    CHECK(
        execution.nodes.front()
            .required_capabilities ==
        std::vector<std::string>{
            "pelican.execution.graphics@1"});
    REQUIRE_NOTHROW(
        validateFrameExecutionPlan(execution));
}

TEST_CASE(
    "frame execution validation rejects unclosed endpoint "
    "and dependency state",
    "[wp238][execution-plan]") {
    const auto graph = mixedGraph();
    const auto frame = planFrameGraph(graph);
    const auto valid =
        compileFrameExecutionPlan(
            graph, frame,
            testEndpoint());

    SECTION("missing capability") {
        auto invalid = valid;
        invalid.nodes.front()
            .required_capabilities.push_back(
                "test.execution.missing@1");
        CHECK_THROWS_WITH(
            canonicalizeFrameExecutionPlan(
                std::move(invalid)),
            Catch::Matchers::ContainsSubstring(
                "does not provide capability"));
    }

    SECTION("backward dependency") {
        auto invalid = valid;
        invalid.dependencies.push_back(
            ExecutionDependency{
                .from = "snapshot",
                .to = "geometry",
                .reason =
                    "test.dependency.backward@1",
            });
        CHECK_THROWS_WITH(
            canonicalizeFrameExecutionPlan(
                std::move(invalid)),
            Catch::Matchers::ContainsSubstring(
                "source does not precede target"));
    }

    SECTION("stale fingerprint") {
        auto invalid = valid;
        ++invalid.fingerprint;
        CHECK_THROWS_WITH(
            validateFrameExecutionPlan(invalid),
            Catch::Matchers::ContainsSubstring(
                "fingerprint mismatch"));
    }
}

} // namespace Pelican
