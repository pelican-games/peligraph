#include "../src/core/renderingpass/frameexecutionadapter.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/renderingpass/vulkannativescopeexecutor.hpp"
#include "../src/core/renderingpass/vulkanrendercompilerpackage.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <memory>
#include <string>

namespace Pelican {
namespace {

VulkanTargetPlan markerTargetPlan() {
    VulkanTargetPlan plan;
    plan.graph = "native_graph";
    plan.logical_graph_fingerprint = 0x1001;
    plan.automatic_plan_fingerprint = 0x2002;
    plan.backend_selection.selected_candidate =
        "pelican.vulkan.materialized_plan@1";
    plan.scopes.push_back(
        VulkanPhysicalScopePlan{
            .id = "scope.marker",
            .kind =
                VulkanPhysicalScopeKind::marker,
            .nodes = {"anchor"},
            .view_execution =
                VulkanScopeViewExecution::single_view,
            .view_count = 1,
            .execution_count = 1,
        });
    plan.view_execution_plan.view_count = 1;
    return plan;
}

FramePlan markerFramePlan() {
    FramePlan plan;
    plan.name = "native_graph";
    plan.nodes.push_back(
        FramePlanNode{
            .name = "anchor",
            .kind = FramePlanNodeKind::anchor,
        });
    return plan;
}

VerifiedVulkanCompletePhysicalPlanPackage
markerCompletePackage(std::string implementation) {
    auto package =
        ejectVulkanCompletePhysicalPlanPackage(
            markerTargetPlan());
    package.native_scopes.push_back(
        VulkanNativeScopeDeclaration{
            .scope = "scope.marker",
            .implementation =
                std::move(implementation),
        });
    return {
        .package = package,
        .package_fingerprint =
            vulkanCompletePhysicalPlanPackageFingerprint(
                package),
    };
}

std::shared_ptr<const PreparedVulkanNativeScopeGraph>
prepareMarker(
    const VulkanNativeScopeExecutorRegistrySnapshot
        &snapshot,
    const VerifiedVulkanCompletePhysicalPlanPackage
        &verified) {
    const auto target = markerTargetPlan();
    const auto frame = markerFramePlan();
    return prepareVulkanNativeScopeExecutors(
        snapshot, verified, target, frame,
        {}, {}, {});
}

class CountingExecutor final
    : public VulkanNativeScopeExecutor {
    std::shared_ptr<int> calls_;

  public:
    explicit CountingExecutor(
        std::shared_ptr<int> calls)
        : calls_{std::move(calls)} {}

    void record(
        const VulkanNativeScopeRecordContext &context) const override {
        REQUIRE(context.scope != nullptr);
        REQUIRE(context.declaration != nullptr);
        REQUIRE(context.resources.empty());
        ++*calls_;
    }
};

RenderPipelineProgramPreparation markerProgram(
    std::shared_ptr<const PreparedVulkanNativeScopeGraph>
        native_scopes = {}) {
    CompiledRenderingPass rendering_pass;
    rendering_pass.name = "native_graph";
    auto frame_plan = markerFramePlan();
    auto pipeline =
        std::make_shared<CompiledRenderPipeline>();
    pipeline->graph_variant_policy.variant =
        RenderPipelineGraphVariant::flat;
    auto target =
        std::make_shared<VulkanTargetPlan>(
            markerTargetPlan());
    return {
        .rendering_pass =
            std::move(rendering_pass),
        .frame_plan = frame_plan,
        .execution_plan =
            makeCompatibilityFrameExecutionPlan(
                frame_plan),
        .render_pipeline = std::move(pipeline),
        .target_plan = std::move(target),
        .native_scopes =
            std::move(native_scopes),
    };
}

} // namespace

TEST_CASE(
    "WP238d builtin NativeScope marker prepares an executable runtime "
    "artifact",
    "[wp238d][native-scope][runtime]") {
    VulkanNativeScopeExecutorRegistry registry;
    const auto verified =
        markerCompletePackage(
            std::string{
                builtinVulkanNoopMarkerNativeScope});
    const auto prepared =
        prepareMarker(
            registry.snapshot(), verified);

    REQUIRE(prepared != nullptr);
    REQUIRE(
        std::string{prepared->graph()} ==
        "native_graph");
    REQUIRE(
        prepared->packageFingerprint() ==
        verified.package_fingerprint);
    REQUIRE(prepared->scopes().size() == 1);
    const auto *scope =
        prepared->find("scope.marker");
    REQUIRE(scope != nullptr);
    CHECK(
        scope->selection().provider ==
        "builtin.vulkan");
    CHECK(
        scope->selection().implementation ==
        std::string{
            builtinVulkanNoopMarkerNativeScope});
    scope->record(
        VulkanNativeScopeRecordContext{
            .view =
                RenderPassViewInvocation{1, 0},
        });
}

TEST_CASE(
    "WP238d provider snapshot and runtime generation retain source code "
    "leases through retirement",
    "[wp238d][native-scope][lease][transaction]") {
    VulkanNativeScopeExecutorRegistry registry;
    constexpr internal::RegistrationOwner owner =
        42;
    auto lease = std::make_shared<int>(7);
    std::weak_ptr<const void> retired_lease =
        lease;
    auto calls = std::make_shared<int>(0);
    registry.registerProvider(
        VulkanNativeScopeExecutorProvider{
            .provider = "test.source",
            .implementation =
                "test.native.marker@1",
            .capabilities =
                vulkanNativeScopeExecutorCapability(
                    VulkanNativeScopeExecutorCapability::
                        automatic_synchronization),
            .generation_lease = lease,
            .prepare =
                [calls](
                    const VulkanNativeScopePrepareContext
                        &context)
                -> std::shared_ptr<
                    const VulkanNativeScopeExecutor> {
                    REQUIRE(
                        context.scope.kind ==
                        VulkanPhysicalScopeKind::marker);
                    REQUIRE(context.resources.empty());
                    return std::make_shared<
                        CountingExecutor>(calls);
                },
        },
        owner);
    registry.activateOwner(owner);
    auto snapshot = registry.snapshot();
    registry.releaseOwner(owner);
    lease.reset();

    const auto verified =
        markerCompletePackage(
            "test.native.marker@1");
    auto prepared =
        prepareMarker(snapshot, verified);
    snapshot = {};
    REQUIRE_FALSE(retired_lease.expired());

    const auto *scope =
        prepared->find("scope.marker");
    REQUIRE(scope != nullptr);
    CHECK(scope->selection().owner == owner);
    scope->record(
        VulkanNativeScopeRecordContext{
            .view =
                RenderPassViewInvocation{1, 0},
        });
    CHECK(*calls == 1);

    FrameGraphRuntimeContainer runtime;
    auto candidate =
        runtime.prepareGeneration(
            {markerProgram(prepared)});
    runtime.publishPreparedGeneration(
        std::move(candidate));
    auto in_flight_frame = runtime.snapshot();
    prepared.reset();
    REQUIRE_FALSE(retired_lease.expired());

    auto replacement =
        runtime.prepareGeneration(
            {markerProgram()});
    runtime.publishPreparedGeneration(
        std::move(replacement));
    REQUIRE_FALSE(retired_lease.expired());
    in_flight_frame.reset();
    REQUIRE(retired_lease.expired());
}

TEST_CASE(
    "WP238d NativeScope preparation rejects missing providers and "
    "capability drift before publication",
    "[wp238d][native-scope][validation]") {
    VulkanNativeScopeExecutorRegistry registry;

    SECTION("missing implementation") {
        const auto verified =
            markerCompletePackage(
                "missing.native@1");
        CHECK_THROWS_WITH(
            prepareMarker(
                registry.snapshot(),
                verified),
            Catch::Matchers::ContainsSubstring(
                "provider was not found"));
    }

    SECTION("declared hot reload support") {
        auto verified =
            markerCompletePackage(
                std::string{
                    builtinVulkanNoopMarkerNativeScope});
        verified.package.native_scopes.front()
            .hot_reloadable = true;
        verified.package_fingerprint =
            vulkanCompletePhysicalPlanPackageFingerprint(
                verified.package);
        CHECK_THROWS_WITH(
            prepareMarker(
                registry.snapshot(),
                verified),
            Catch::Matchers::ContainsSubstring(
                "hot-reload capability"));
    }
}

TEST_CASE(
    "WP238d compiler package rejects complete-plan and runtime-plan "
    "drift before GPU registration",
    "[wp238d][native-scope][compiler]") {
    const auto verified =
        std::make_shared<
            const VerifiedVulkanCompletePhysicalPlanPackage>(
            markerCompletePackage(
                std::string{
                    builtinVulkanNoopMarkerNativeScope}));
    auto target =
        std::make_shared<VulkanTargetPlan>(
            markerTargetPlan());

    VulkanRenderCompilerPhysicalPackage physical;
    physical.target_plan_compilation.plans = {
        target};
    physical.target_plans.emplace(
        target->graph, target);
    physical.verified_complete_physical_plans
        .emplace(target->graph, verified);
    const std::vector<std::string> graph_names{
        "native_graph"};
    REQUIRE_NOTHROW(
        physical.validate(graph_names));

    auto drifted =
        std::make_shared<VulkanTargetPlan>(
            *target);
    drifted->scopes.front().id =
        "scope.drifted";
    physical.target_plan_compilation.plans = {
        drifted};
    physical.target_plans.at(
        "native_graph") = drifted;
    CHECK_THROWS_WITH(
        physical.validate(graph_names),
        Catch::Matchers::ContainsSubstring(
            "does not match its verified complete physical package"));
}

} // namespace Pelican
