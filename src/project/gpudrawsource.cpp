#include "gpudrawsource.hpp"
#include "rendergraphir.hpp"
#include "graphcomposition.hpp"

#include <algorithm>

namespace Pelican {
namespace {

[[noreturn]] void invalidGpuDraw(const GraphIR &graph, const NodeIR &node,
    std::string_view field, std::string_view reason, nlohmann::json more) {
    more["graph"] = graph.name;
    more["node"] = node.name;
    more["field"] = "gpu_draw_source." + std::string{field};
    more["reason"] = reason;
    throw GraphRegistryError("render_graph.gpu_draw_source_invalid@1", std::move(more));
}

const FrameGraphBufferDefinition &requireGpuDrawBuffer(
    std::span<const FrameGraphBufferDefinition> definitions, const std::string &name,
    std::string_view role, const GraphIR &graph, const NodeIR &node) {
    const auto found = std::find_if(definitions.begin(), definitions.end(),
        [&](const auto &candidate) { return candidate.name == name; });
    if (found == definitions.end()) {
        invalidGpuDraw(graph, node, role, "unknown_buffer", {{"buffer", name}});
    }
    return *found;
}

void validateGpuDrawSourceBufferContract(const GpuDrawSourceDeclaration &source,
    std::span<const FrameGraphBufferDefinition> definitions, const GraphIR &graph, const NodeIR &node) {
    const auto &commands = requireGpuDrawBuffer(definitions, source.commands, "commands", graph, node);
    const auto &count = requireGpuDrawBuffer(definitions, source.count, "count", graph, node);
    if (commands.command_layout != FrameGraphBufferCommandLayout::indexed_draw) {
        invalidGpuDraw(graph, node, "commands", "command_layout_mismatch",
            {{"buffer", source.commands}, {"expected", "indexed_draw"}, {"detail", "requires command_layout 'indexed_draw'"}});
    }
    if (count.command_layout != FrameGraphBufferCommandLayout::draw_count) {
        invalidGpuDraw(graph, node, "count", "command_layout_mismatch",
            {{"buffer", source.count}, {"expected", "draw_count"}, {"detail", "requires command_layout 'draw_count'"}});
    }
    // Programs may return their own IR, bypassing the JSON alignment check.
    if (source.command_offset % frameGraphIndirectCommandAlignment != 0) {
        invalidGpuDraw(graph, node, "command_offset", "not_4_byte_aligned", {{"offset", source.command_offset}});
    }
    if (source.count_offset % frameGraphIndirectCommandAlignment != 0) {
        invalidGpuDraw(graph, node, "count_offset", "not_4_byte_aligned", {{"offset", source.count_offset}});
    }
    const auto command_bytes = static_cast<std::uint64_t>(source.max_draw_count) * frameGraphIndexedDrawCommandBytes;
    if (source.command_offset > commands.size || commands.size - source.command_offset < command_bytes) {
        invalidGpuDraw(graph, node, "command_offset", "command_range_exceeds_buffer",
            {{"buffer", source.commands}, {"offset", source.command_offset}, {"bytes", command_bytes}, {"size", commands.size}});
    }
    if (source.count_offset > count.size || count.size - source.count_offset < frameGraphDrawCountBytes) {
        invalidGpuDraw(graph, node, "count_offset", "count_exceeds_buffer",
            {{"buffer", source.count}, {"offset", source.count_offset}, {"size", count.size}});
    }
    if (source.layout == GpuDrawSourceLayout::draw_queue_segments_v1) {
        const auto &segments = requireGpuDrawBuffer(definitions, source.segments, "segments", graph, node);
        if (segments.host_source != FrameGraphHostBufferSource::scene_draw_segments_v1) {
            invalidGpuDraw(graph, node, "segments", "host_source_mismatch",
                {{"buffer", source.segments}, {"expected", "scene_draw_segments_v1"}});
        }
    }
}
} // namespace

void validateGpuDrawSourceBufferContracts(const RenderGraphIR &render_graph,
    std::span<const FrameGraphBufferDefinition> buffer_definitions) {
    for (const auto &graph : render_graph.graphs) {
        for (const auto &node : graph.nodes) {
            const auto *material = std::get_if<MaterialNodeInfo>(&node.info);
            if (material != nullptr && material->gpu_draw_source) {
                validateGpuDrawSourceBufferContract(*material->gpu_draw_source, buffer_definitions, graph, node);
            }
        }
    }
}
} // namespace Pelican
