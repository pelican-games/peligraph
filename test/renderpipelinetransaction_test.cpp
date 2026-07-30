#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/renderingpass/frameexecutionadapter.hpp"
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
    auto execution_plan =
        makeCompatibilityFrameExecutionPlan(
            frame_plan);

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
        .rendering_pass = std::move(rendering_pass),
        .frame_plan = std::move(frame_plan),
        .execution_plan =
            std::move(execution_plan),
        .render_pipeline = std::move(pipeline),
        .target_plan = std::move(target_plan),
    };
}

int revisionOf(const RendererRuntimeGeneration &generation,
               RenderingPassId id) {
    const auto *program = generation.find(id);
    if (program == nullptr) return -1;
    const auto pass_revision =
        program->rendering_pass.passes.front().pass_id.value;
    const auto plan_revision = static_cast<int>(
        program->frame_graph.plan.nodes.front()
            .declaration_index);
    const auto execution_revision =
        static_cast<int>(
            program->frame_graph.execution_plan
                .nodes.front()
                .declaration_index);
    const auto sample_revision = static_cast<int>(
        program->frame_graph.sample_count_plan->request.samples);
    const auto expected_provider =
        "provider_" + std::to_string(pass_revision);
    if (plan_revision != pass_revision ||
        execution_revision != pass_revision ||
        program->frame_graph.execution_plan
                .fingerprint !=
            frameExecutionPlanFingerprint(
                program->frame_graph
                    .execution_plan) ||
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

OutputCompileFacts windowFacts(
    std::uint32_t width = 1280) {
    return OutputCompileFacts{
        .target_kind = OutputTargetKind::window,
        .extent = vk::Extent2D{width, 720},
        .color_format = vk::Format::eB8G8R8A8Srgb,
        .color_space =
            vk::ColorSpaceKHR::eSrgbNonlinear,
        .encoding_path =
            OutputEncodingPath::srgb_hardware,
        .selected_usage =
            vk::ImageUsageFlagBits::eColorAttachment,
        .surface_transform =
            vk::SurfaceTransformFlagBitsKHR::eIdentity,
    };
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
    "WP215 window output facts and programs share one renderer publication root",
    "[wp215][render-pipeline][output][transaction]") {
    FrameGraphRuntimeContainer runtime;
    const auto initial_facts = windowFacts();
    auto initial = runtime.prepareGeneration(
        {programFor(1)}, std::nullopt, std::nullopt,
        initial_facts);

    REQUIRE(runtime.snapshot() == nullptr);
    REQUIRE(initial.candidate().window_output !=
            nullptr);
    REQUIRE(
        initial.candidate()
            .window_output->compile_fingerprint ==
        outputCompileFactsFingerprint(initial_facts));
    REQUIRE(revisionOf(initial.candidate(),
                       RenderingPassId{0}) == 1);

    runtime.publishPreparedGeneration(
        std::move(initial));
    auto old_frame_root = runtime.snapshot();
    std::weak_ptr<const RendererRuntimeGeneration>
        retired = old_frame_root;

    const auto resized_facts = windowFacts(1920);
    auto resized = runtime.prepareGeneration(
        {programFor(2)}, std::nullopt, std::nullopt,
        resized_facts);
    REQUIRE(runtime.snapshot() == old_frame_root);
    REQUIRE(
        resized.candidate()
            .window_output->compile_fingerprint ==
        outputCompileFactsFingerprint(resized_facts));
    REQUIRE(revisionOf(resized.candidate(),
                       RenderingPassId{0}) == 2);

    runtime.publishPreparedGeneration(
        std::move(resized));
    REQUIRE(runtime.snapshot() != old_frame_root);
    REQUIRE(
        runtime.snapshot()
            ->window_output->compile_facts.extent ==
        resized_facts.extent);
    REQUIRE(revisionOf(*runtime.snapshot(),
                       RenderingPassId{0}) == 2);
    REQUIRE_FALSE(retired.expired());
    old_frame_root.reset();
    REQUIRE(retired.expired());

    const auto published = runtime.snapshot();
    auto invalid_facts = resized_facts;
    invalid_facts.target_kind =
        OutputTargetKind::offscreen;
    REQUIRE_THROWS_WITH(
        runtime.prepareGeneration(
            {programFor(3)}, std::nullopt,
            std::nullopt, invalid_facts),
        "renderer window output child requires window compile facts");
    REQUIRE(runtime.snapshot() == published);
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
        "Frame execution node does not match FramePlan node at order 0: missing_runtime_pass");
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
    bool stale_commit_called = false;
    REQUIRE_THROWS_WITH(
        runtime.publishPreparedGeneration(
            std::move(stale),
            [&](const RendererRuntimeGeneration &) {
                stale_commit_called = true;
            }),
        "Renderer runtime candidate is stale");
    REQUIRE_FALSE(stale_commit_called);
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
    std::weak_ptr<const RendererRuntimeGeneration>
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

TEST_CASE(
    "WP194 GPU arena candidate and leases stay unpublished until root publication",
    "[wp194][render-pipeline][gpu-arena][transaction]") {
    FrameGraphRuntimeContainer runtime;
    auto lease = std::make_shared<int>(194);
    std::weak_ptr<const void> lifetime = lease;
    RenderPipelineGpuScopePreparation gpu_scope;
    gpu_scope.owner_scope = "render_pipeline/flat";
    gpu_scope.resources.push_back(
        RenderPipelineGpuResourceRegistration{
            RenderPipelineGpuResourceKind::pipeline,
            7,
            "pipeline/7",
            0,
        });
    gpu_scope.resource_leases.push_back(lease);
    lease.reset();

    auto prepared = runtime.prepareGeneration(
        {programFor(1)}, std::nullopt,
        std::move(gpu_scope));
    REQUIRE(runtime.snapshot() == nullptr);
    REQUIRE_FALSE(lifetime.expired());

    runtime.publishPreparedGeneration(std::move(prepared));
    const auto published = runtime.snapshot();
    REQUIRE(published != nullptr);
    REQUIRE(published->gpu_arena != nullptr);
    REQUIRE(published->gpu_arena->runtime_generation == 1);
    REQUIRE(published->gpu_arena->resourceCount() == 1);
    REQUIRE(
        published->gpu_arena
            ->findScope("render_pipeline/flat")
            ->resources.front()
            .name == "pipeline/7");
    REQUIRE_FALSE(lifetime.expired());
}

TEST_CASE(
    "WP194 rollback releases unpublished GPU arena leases",
    "[wp194][render-pipeline][gpu-arena][rollback]") {
    FrameGraphRuntimeContainer runtime;
    auto initial = runtime.prepareGeneration(
        {programFor(1)}, std::nullopt,
        RenderPipelineGpuScopePreparation{
            .owner_scope = "render_pipeline/flat",
            .resources = {
                RenderPipelineGpuResourceRegistration{
                    RenderPipelineGpuResourceKind::render_target,
                    0,
                    "display",
                    0,
                }},
        });
    runtime.publishPreparedGeneration(std::move(initial));
    const auto before = runtime.snapshot();

    auto lease = std::make_shared<int>(195);
    std::weak_ptr<const void> lifetime = lease;
    RenderPipelineGpuScopePreparation next_scope{
        .owner_scope = "render_pipeline/xr",
        .resources = {
            RenderPipelineGpuResourceRegistration{
                RenderPipelineGpuResourceKind::pipeline,
                8,
                "pipeline/8",
                0,
            }},
        .resource_leases = {lease},
    };
    lease.reset();
    auto rolled_back = runtime.prepareGeneration(
        {programFor(2, "main#xr")}, std::nullopt,
        std::move(next_scope));
    runtime.rollbackPreparedGeneration(rolled_back);
    REQUIRE(lifetime.expired());
    REQUIRE(runtime.snapshot() == before);
}

TEST_CASE(
    "WP195 owner replacement keeps old resources for old frames and removes omitted programs",
    "[wp195][render-pipeline][gpu-arena][replacement]") {
    FrameGraphRuntimeContainer runtime;
    auto old_lease = std::make_shared<int>(195);
    std::weak_ptr<const void> old_lifetime = old_lease;
    auto initial = runtime.prepareGeneration(
        {programFor(1, "main"),
         programFor(1, "removed")},
        std::nullopt,
        RenderPipelineGpuScopePreparation{
            .owner_scope = "render_pipeline/flat",
            .resources = {
                RenderPipelineGpuResourceRegistration{
                    RenderPipelineGpuResourceKind::render_target,
                    1,
                    "display",
                    0,
                }},
            .resource_leases = {old_lease},
        });
    old_lease.reset();
    runtime.publishPreparedGeneration(std::move(initial));

    auto old_frame = runtime.snapshot();
    const auto main_id =
        old_frame->name_to_id.at("main");
    const auto removed_id =
        old_frame->name_to_id.at("removed");

    auto new_lease = std::make_shared<int>(196);
    std::weak_ptr<const void> new_lifetime = new_lease;
    auto replacement = runtime.prepareGeneration(
        {programFor(2, "main")}, std::nullopt,
        RenderPipelineGpuScopePreparation{
            .owner_scope = "render_pipeline/flat",
            .resources = {
                RenderPipelineGpuResourceRegistration{
                    RenderPipelineGpuResourceKind::render_target,
                    2,
                    "display",
                    0,
                }},
            .resource_leases = {new_lease},
        });
    new_lease.reset();

    REQUIRE(runtime.snapshot() == old_frame);
    REQUIRE_FALSE(old_lifetime.expired());
    runtime.publishPreparedGeneration(
        std::move(replacement));

    auto active = runtime.snapshot();
    REQUIRE(active->name_to_id.at("main") == main_id);
    REQUIRE_FALSE(active->name_to_id.contains("removed"));
    REQUIRE(active->find(removed_id) == nullptr);
    REQUIRE(revisionOf(*active, main_id) == 2);
    REQUIRE(
        active->gpu_arena
            ->findScope("render_pipeline/flat")
            ->resources.front()
            .handle == 2);
    REQUIRE(revisionOf(*old_frame, main_id) == 1);
    REQUIRE(old_frame->find(removed_id) != nullptr);
    REQUIRE_FALSE(old_lifetime.expired());
    REQUIRE_FALSE(new_lifetime.expired());

    active.reset();
    old_frame.reset();
    REQUIRE(old_lifetime.expired());
    REQUIRE_FALSE(new_lifetime.expired());
}

TEST_CASE(
    "WP195 unchanged foreign programs retain replaced scope dependencies",
    "[wp195][render-pipeline][gpu-arena][dependency]") {
    FrameGraphRuntimeContainer runtime;
    auto flat_lease = std::make_shared<int>(1);
    std::weak_ptr<const void> flat_lifetime = flat_lease;
    auto flat = runtime.prepareGeneration(
        {programFor(1, "main")}, std::nullopt,
        RenderPipelineGpuScopePreparation{
            .owner_scope = "render_pipeline/flat",
            .resource_leases = {flat_lease},
        });
    flat_lease.reset();
    runtime.publishPreparedGeneration(std::move(flat));

    auto xr = runtime.prepareGeneration(
        {programFor(1, "main#xr")}, std::nullopt,
        RenderPipelineGpuScopePreparation{
            .owner_scope = "render_pipeline/xr",
        });
    runtime.publishPreparedGeneration(std::move(xr));

    auto next_flat = runtime.prepareGeneration(
        {programFor(2, "main")}, std::nullopt,
        RenderPipelineGpuScopePreparation{
            .owner_scope = "render_pipeline/flat",
        });
    runtime.publishPreparedGeneration(
        std::move(next_flat));
    REQUIRE_FALSE(flat_lifetime.expired());

    auto next_xr = runtime.prepareGeneration(
        {programFor(2, "main#xr")}, std::nullopt,
        RenderPipelineGpuScopePreparation{
            .owner_scope = "render_pipeline/xr",
        });
    runtime.publishPreparedGeneration(std::move(next_xr));
    REQUIRE(flat_lifetime.expired());
}
