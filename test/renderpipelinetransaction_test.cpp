#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/renderingpass/renderingpasscontainer.hpp"
#include "../src/project/graphvariantpolicy.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace Pelican;

MaterialRouteClass routeFor(int revision) {
    return revision % 2 == 0
               ? MaterialRouteClass::forward_opaque
               : MaterialRouteClass::deferred_geometry;
}

MaterialPassContract contractFor(int revision) {
    return revision % 2 == 0
               ? MaterialPassContract::forward_opaque_v1
               : MaterialPassContract::deferred_geometry_v1;
}

RenderPipelineProgramPreparation programFor(
    int revision, std::string program_name = "main") {
    PassDefinition pass;
    pass.name = "draw";
    pass.materialInfo().contract = contractFor(revision);

    CompiledRenderingPass rendering_pass;
    rendering_pass.name = std::move(program_name);
    rendering_pass.passes.push_back(
        CompiledPass{std::move(pass), PassId{revision}});

    FramePlan frame_plan;
    frame_plan.name = rendering_pass.name;
    frame_plan.nodes.push_back(FramePlanNode{
        .name = "draw",
        .kind = FramePlanNodeKind::render,
        .declaration_index =
            static_cast<std::size_t>(revision),
    });

    auto pipeline = std::make_shared<CompiledRenderPipeline>();
    pipeline->feature_names = {
        "revision_" + std::to_string(revision)};
    pipeline->material_routing = CompiledMaterialRouting{};
    pipeline->material_routing->routes.push_back(
        CompiledMaterialRoute{
            routeFor(revision), "draw",
            contractFor(revision)});
    pipeline->draw_sorting.opaque.provider =
        "provider_" + std::to_string(revision);
    pipeline->graph_variant_policy =
        compileGraphVariantPolicy(
            GraphVariantPolicyRequest{
                RenderPipelineGraphVariant::flat});

    auto target_plan = std::make_shared<VulkanTargetPlan>();
    target_plan->graph = rendering_pass.name;
    target_plan->sample_count_plan =
        ResolvedSampleCountPlan{};
    target_plan->sample_count_plan->request.samples =
        static_cast<std::uint32_t>(revision);

    return RenderPipelineProgramPreparation{
        std::move(rendering_pass),
        std::move(frame_plan),
        std::move(pipeline),
        std::move(target_plan),
    };
}

int revisionOf(const RenderPipelineRuntimeGeneration &generation,
               RenderingPassId id) {
    const auto *program = generation.find(id);
    if (program == nullptr) return -1;
    const auto pass_revision =
        program->rendering_pass.passes.front().pass_id.value;
    const auto plan_revision = static_cast<int>(
        program->frame_graph.plan.nodes.front()
            .declaration_index);
    const auto sample_revision = static_cast<int>(
        program->frame_graph.sample_count_plan->request.samples);
    const auto expected_provider =
        "provider_" + std::to_string(pass_revision);
    if (plan_revision != pass_revision ||
        sample_revision != pass_revision ||
        program->frame_graph.render_pipeline
                ->draw_sorting.opaque.provider !=
            expected_provider ||
        program->frame_graph.material_routes.front().route !=
            routeFor(pass_revision)) {
        return -1;
    }
    return pass_revision;
}

} // namespace

TEST_CASE(
    "WP193 prepared render generation is invisible until atomic publication",
    "[wp193][render-pipeline][transaction]") {
    FrameGraphRuntimeContainer runtime;
    RenderingPassContainer passes;
    passes.bindRuntimePublication(runtime.publicationState());

    auto prepared = runtime.prepareGeneration(
        {programFor(1)},
        std::vector<std::string>{"flat_feature"});
    REQUIRE(prepared.valid());
    REQUIRE(prepared.baseGeneration() == 0);
    REQUIRE(prepared.generation() == 1);
    REQUIRE(prepared.renderingPassIds() ==
            std::vector<RenderingPassId>{RenderingPassId{0}});
    REQUIRE(runtime.activeGeneration() == 0);
    REQUIRE(runtime.snapshot() == nullptr);
    REQUIRE_FALSE(isValidRenderingPassId(
        passes.getRenderingPassIdByName("main")));

    runtime.publishPreparedGeneration(std::move(prepared));

    REQUIRE_FALSE(prepared.valid());
    REQUIRE(runtime.activeGeneration() == 1);
    REQUIRE(passes.activeGeneration() == 1);
    const auto main = passes.getRenderingPassIdByName("main");
    REQUIRE(main == RenderingPassId{0});
    REQUIRE(passes.getEnabledFeatures() ==
            std::vector<std::string>{"flat_feature"});
    REQUIRE(passes.getCompiledRenderingPass(main)
                .passes.front()
                .pass_id == PassId{1});
    REQUIRE(revisionOf(*runtime.snapshot(), main) == 1);
}

TEST_CASE(
    "WP193 rollback and failed preparation preserve the published generation",
    "[wp193][render-pipeline][transaction][rollback]") {
    FrameGraphRuntimeContainer runtime;
    auto initial = runtime.prepareGeneration({programFor(1)});
    runtime.publishPreparedGeneration(std::move(initial));
    const auto before = runtime.snapshot();

    auto rolled_back =
        runtime.prepareGeneration({programFor(2)});
    REQUIRE(rolled_back.valid());
    runtime.rollbackPreparedGeneration(rolled_back);
    REQUIRE_FALSE(rolled_back.valid());
    REQUIRE(runtime.snapshot() == before);

    auto invalid = programFor(3);
    invalid.frame_plan.nodes.front().name =
        "missing_runtime_pass";
    REQUIRE_THROWS_WITH(
        runtime.prepareGeneration(
            {std::move(invalid)}),
        "Frame plan render node is not compiled: missing_runtime_pass");
    REQUIRE(runtime.snapshot() == before);
    REQUIRE(revisionOf(*runtime.snapshot(),
                       RenderingPassId{0}) == 1);
}

TEST_CASE(
    "WP193 stale candidates cannot overwrite a newer publication",
    "[wp193][render-pipeline][transaction][stale]") {
    FrameGraphRuntimeContainer runtime;
    auto left = runtime.prepareGeneration({programFor(1)});
    auto stale = runtime.prepareGeneration({programFor(2)});

    runtime.publishPreparedGeneration(std::move(left));
    REQUIRE_THROWS_WITH(
        runtime.publishPreparedGeneration(
            std::move(stale)),
        "Render pipeline runtime candidate is stale");
    REQUIRE(stale.valid());
    REQUIRE(runtime.activeGeneration() == 1);
    REQUIRE(revisionOf(*runtime.snapshot(),
                       RenderingPassId{0}) == 1);
}

TEST_CASE(
    "WP193 one snapshot keeps route sample provider and pass revision coherent",
    "[wp193][render-pipeline][transaction][atomic]") {
    FrameGraphRuntimeContainer runtime;
    auto initial = runtime.prepareGeneration({programFor(1)});
    runtime.publishPreparedGeneration(std::move(initial));

    std::atomic_bool running{true};
    std::atomic_bool mixed_generation{false};
    std::thread observer{[&] {
        while (running.load(std::memory_order_relaxed)) {
            const auto snapshot = runtime.snapshot();
            if (snapshot == nullptr ||
                revisionOf(*snapshot,
                           RenderingPassId{0}) < 1) {
                mixed_generation.store(
                    true, std::memory_order_relaxed);
                break;
            }
        }
    }};

    for (int revision = 2; revision <= 500; ++revision) {
        auto prepared =
            runtime.prepareGeneration(
                {programFor(revision)});
        runtime.publishPreparedGeneration(
            std::move(prepared));
    }
    running.store(false, std::memory_order_relaxed);
    observer.join();

    REQUIRE_FALSE(
        mixed_generation.load(std::memory_order_relaxed));
    REQUIRE(runtime.activeGeneration() == 500);
    REQUIRE(revisionOf(*runtime.snapshot(),
                       RenderingPassId{0}) == 500);
}

TEST_CASE(
    "WP193 frame lease retires an old generation only after its last reader",
    "[wp193][render-pipeline][transaction][retire]") {
    FrameGraphRuntimeContainer runtime;
    RenderingPassContainer passes;
    passes.bindRuntimePublication(runtime.publicationState());
    auto initial = runtime.prepareGeneration({programFor(1)});
    runtime.publishPreparedGeneration(std::move(initial));

    auto frame_lease = runtime.snapshot();
    std::weak_ptr<const RenderPipelineRuntimeGeneration>
        retired = frame_lease;

    auto replacement =
        runtime.prepareGeneration({programFor(2)});
    runtime.publishPreparedGeneration(
        std::move(replacement));

    REQUIRE(revisionOf(*frame_lease,
                       RenderingPassId{0}) == 1);
    REQUIRE(revisionOf(*runtime.snapshot(),
                       RenderingPassId{0}) == 2);
    REQUIRE_FALSE(retired.expired());
    frame_lease.reset();
    REQUIRE(retired.expired());
}

TEST_CASE(
    "WP193 variant registration appends programs without replacing flat feature publication",
    "[wp193][render-pipeline][transaction][variant]") {
    FrameGraphRuntimeContainer runtime;
    RenderingPassContainer passes;
    passes.bindRuntimePublication(runtime.publicationState());

    auto flat = runtime.prepareGeneration(
        {programFor(1, "main")},
        std::vector<std::string>{"taa", "ui"});
    runtime.publishPreparedGeneration(std::move(flat));

    auto xr = runtime.prepareGeneration(
        {programFor(2, "main#xr")});
    REQUIRE(xr.renderingPassIds() ==
            std::vector<RenderingPassId>{RenderingPassId{1}});
    runtime.publishPreparedGeneration(std::move(xr));

    REQUIRE(runtime.activeGeneration() == 2);
    REQUIRE(passes.activeGeneration() == 2);
    REQUIRE(passes.getEnabledFeatures() ==
            std::vector<std::string>{"taa", "ui"});
    REQUIRE(passes.getRegisteredPassIds() ==
            std::vector<RenderingPassId>{
                RenderingPassId{0}, RenderingPassId{1}});
    REQUIRE(passes.getRenderingPassIdByName("main") ==
            RenderingPassId{0});
    REQUIRE(passes.getRenderingPassIdByName("main#xr") ==
            RenderingPassId{1});
}
