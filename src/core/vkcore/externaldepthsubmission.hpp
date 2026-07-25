#pragma once

#include "../renderingpass/framegraphruntime.hpp"
#include "../renderingpass/rendertargetmetadata.hpp"
#include "rendertarget.hpp"

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <optional>

namespace Pelican {

class RenderTargetContainer;
class RenderTargetLayoutTracker;
class VulkanUtils;

struct RuntimeExternalDepthExport {
    GlobalRenderTargetId source_id;
    RenderTargetMetadata source;
    const VulkanExternalDepthExportPlan *plan = nullptr;
};

std::optional<RuntimeExternalDepthExport>
resolveRuntimeExternalDepthExport(
    const CompiledFrameGraphExecution &frame_graph,
    const RenderTargetContainer &render_targets);

void recordExternalDepthExport(
    const FrameRenderContext &render_ctx,
    const RuntimeExternalDepthExport &export_depth,
    RenderTargetContainer &render_targets,
    VulkanUtils &vulkan,
    RenderTargetLayoutTracker &layout_tracker,
    std::uint32_t view_index,
    std::uint32_t logical_view_count,
    bool view_family,
    nlohmann::json *node_trace);

} // namespace Pelican
