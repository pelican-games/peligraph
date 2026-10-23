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

#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican::test_support {

struct RendererTestAccess {
    using Target = Renderer;

    // Render target readback.

    static RenderTargetReadback
    readRenderTarget(Target &renderer, std::string_view name,
                     const ImageSubresourceRange &range) {
        return renderer.readRenderTargetForTesting(name, range);
    }

    static R8RenderTargetReadback readR8RenderTarget(Target &renderer,
                                                     std::string_view name) {
        return renderer.readR8RenderTargetForTesting(name);
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

} // namespace Pelican::test_support

// Short name for the call sites. This header is only ever included by tests,
// so the global-scope alias reaches no production translation unit.
using RendererTestAccess = Pelican::test_support::RendererTestAccess;
