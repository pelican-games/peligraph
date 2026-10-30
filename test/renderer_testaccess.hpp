#pragma once

// The reads and pokes only a test performs on Renderer. They used to be
// eleven public `*ForTesting` members on the renderer itself, which made a
// test-only surface indistinguishable from the engine API. They live here
// now, behind the one friend the renderer declares, so the renderer's public
// header states only what production calls.
//
// `execution_tracing_for_testing` and `last_execution_trace` stay members of
// Renderer -- the frame recorder writes them while a logical frame runs --
// and this struct is now the only way in or out of them. What the trace
// contains is unchanged.

#include "../src/core/vkcore/accelerationstructure.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include <catch2/interfaces/catch_interfaces_capture.hpp>
#include "retain_observed_test_support.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>

#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican::test_support {

struct RendererTestAccess {
    using Target = Renderer;

    static void selectProgram(Target &renderer, RenderingPassId program) {
        renderer.current_rendering_pass_id = program;
    }

    static std::set<std::string> declaredReadbacks(const VulkanTargetPlan &plan) {
        std::set<std::string> declared;
        for (const auto &entry : plan.policy)
            if (entry.key == "retain" && entry.status == PlanningPolicyStatus::adopted && std::get<bool>(entry.value))
                declared.insert(entry.target);
        return declared;
    }

    static TestSupport::RetainObservationAudit::Key observationKey(const Target &renderer,
        const VulkanTargetPlan &plan, const std::set<std::string> &declared) {
        const auto *paths = FastModuleContainer::tryGet<PathResolver>();
        const auto root = paths && paths->isSetup() ? paths->projectRoot().generic_string() : "<unconfigured>";
        // Temporary project directory identifies the fixture; the graph and
        // adopted policy distinguish its independently compiled revisions.
        return {root + ":" + nlohmann::json(declared).dump(),
            plan.graph, static_cast<int>(renderer.active_graph_variant)};
    }

    static void declareActiveObservation() {
        const auto *renderer = FastModuleContainer::tryGet<Target>();
        const auto *runtime = FastModuleContainer::tryGet<FrameGraphRuntimeContainer>();
        if (!renderer || !runtime) return;
        const auto generation = runtime->snapshot();
        const auto *program = generation ? generation->find(renderer->current_rendering_pass_id) : nullptr;
        if (!program || !program->frame_graph.target_plan) return;
        const auto &plan = *program->frame_graph.target_plan;
        // Pixel assertions can number in the millions. Inspect a program's
        // declarations once per audit epoch, without extending GPU lifetime.
        static std::weak_ptr<const RendererRuntimeGeneration> previous;
        static const VulkanTargetPlan *previous_plan = nullptr;
        static int previous_variant = -1;
        static std::uint64_t previous_epoch = 0;
        const auto old = previous.lock();
        const auto variant = static_cast<int>(renderer->active_graph_variant);
        auto &audit = TestSupport::retainObservationAudit;
        if (old == generation && previous_plan == &plan && previous_variant == variant && previous_epoch == audit.epoch) return;
        previous = generation;
        previous_plan = &plan;
        previous_variant = variant;
        previous_epoch = audit.epoch;
        const auto declared = declaredReadbacks(plan);
        audit.declare(observationKey(*renderer, plan, declared), declared);
    }

    // Always audit the successful read against this snapshot's declarations.
    // The optional log is evidence only; assertions never depend on an env var.
    static void recordObservation(Target &renderer, std::string_view name, const ImageSubresourceRange &range) {
        const auto generation = GET_MODULE(FrameGraphRuntimeContainer).snapshot();
        const auto *program = generation ? generation->find(renderer.current_rendering_pass_id) : nullptr;
        if (!program || !program->frame_graph.target_plan) throw std::logic_error("readback observation has no program plan");
        const auto &plan = *program->frame_graph.target_plan;
        const auto retained = declaredReadbacks(plan);
        const auto key = observationKey(renderer, plan, retained);
        TestSupport::retainObservationAudit.record(key, retained, name);
        const auto *path = std::getenv("RC7_OBSERVATION_LOG");
        if (!path || !*path) return;
        std::ofstream out{path, std::ios::binary | std::ios::app};
        out << nlohmann::json{{"test", Catch::getResultCapture().getCurrentTestName()},
            {"graph", plan.graph}, {"fixture", key.fixture}, {"program", plan.logical_graph_fingerprint},
            {"variant", static_cast<int>(renderer.active_graph_variant)}, {"resource", name},
            {"mip", range.base_mip_level}, {"layer", range.base_array_layer}, {"layers", range.layer_count},
            {"retained", retained}}.dump() << '\n';
        if (!out) throw std::runtime_error("cannot write readback observation evidence");
    }

    // Render target readback.

    static RenderTargetReadback
    readRenderTarget(Target &renderer, std::string_view name,
                     const ImageSubresourceRange &range) {
        auto result = renderer.readRenderTargetForTesting(name, range);
        recordObservation(renderer, name, range);
        return result;
    }

    static R8RenderTargetReadback readR8RenderTarget(Target &renderer,
                                                     std::string_view name) {
        auto result = renderer.readR8RenderTargetForTesting(name);
        recordObservation(renderer, name, {});
        return result;
    }

    static void recreateRenderTargetsAndRebind(Target &renderer,
                                               vk::Extent2D extent) {
        renderer.recreateRenderTargetsAndRebindForTesting(extent);
    }

    // Frame plan and diagnostics.

    static std::vector<std::string>
    currentFramePlanOrder(const Target &renderer) {
        return renderer.currentFramePlanOrderForTesting();
    }

    static RayQueryAccelerationStructureDiagnostics
    rayQueryAccelerationStructureDiagnostics(const Target &renderer) {
        return renderer.rayQueryAccelerationStructureDiagnostics();
    }

    static std::size_t
    imageMemoryDependencyCount(const Target &renderer) noexcept {
        return renderer.render_target_layout_tracker
            .memoryDependencyCountForTesting();
    }

    static std::size_t
    imageAliasDependencyCount(const Target &renderer) noexcept {
        return renderer.render_target_layout_tracker
            .aliasDependencyCountForTesting();
    }

    static const std::vector<RenderFrameSnapshot> &
    lastViewSnapshots(const Target &renderer) {
        return renderer.last_view_snapshots;
    }

    static const nlohmann::json &
    graphVariantTransitionTrace(const Target &renderer) noexcept {
        return renderer.graph_variant_transition_trace;
    }

    // FP-1: how many times get_frame_plan's generation part has been built,
    // and forgetting it so the next call builds it the way a first call
    // does.
    static std::uint64_t
    framePlanGenerationPartBuilds(const Target &renderer) noexcept {
        return renderer.frame_plan_generation_part.builds;
    }

    static void dropFramePlanGenerationPart(const Target &renderer) noexcept {
        renderer.frame_plan_generation_part.plan = nullptr;
    }

    // Execution trace. Recording is off unless a test turns it on.

    static void setExecutionTracing(Target &renderer, bool enabled) {
        renderer.execution_tracing_for_testing = enabled;
    }

    static const nlohmann::json &lastExecutionTrace(const Target &renderer) {
        return renderer.last_execution_trace;
    }
};

inline const bool retainObservationHookInstalled = [] {
    TestSupport::declareActiveRetainFixture = &RendererTestAccess::declareActiveObservation;
    return true;
}();

} // namespace Pelican::test_support

// Short name for the call sites. This header is only ever included by tests,
// so the global-scope alias reaches no production translation unit.
using RendererTestAccess = Pelican::test_support::RendererTestAccess;
