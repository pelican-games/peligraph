#include "gpudrawsource.hpp"
#include "rendergraphir.hpp"

#include <algorithm>
#include <stdexcept>

namespace Pelican {
namespace {

const FrameGraphBufferDefinition &
requireGpuDrawBuffer(
    std::span<const FrameGraphBufferDefinition>
        definitions,
    std::string_view name,
    std::string_view role,
    std::string_view pass_name) {
    const auto found = std::find_if(
        definitions.begin(), definitions.end(),
        [&](const auto &candidate) {
            return candidate.name == name;
        });
    if (found == definitions.end()) {
        throw std::runtime_error(
            "Material pass '" +
            std::string{pass_name} +
            "' gpu_draw_source " +
            std::string{role} +
            " references unknown buffer '" +
            std::string{name} + "'");
    }
    return *found;
}

void validateGpuDrawSourceBufferContract(
    const GpuDrawSourceDeclaration &source,
    std::span<const FrameGraphBufferDefinition>
        definitions,
    std::string_view pass_name) {
    const auto &commands = requireGpuDrawBuffer(
        definitions, source.commands, "commands",
        pass_name);
    const auto &count = requireGpuDrawBuffer(
        definitions, source.count, "count",
        pass_name);
    if (commands.command_layout !=
        FrameGraphBufferCommandLayout::
            indexed_draw) {
        throw std::runtime_error(
            "Material pass '" +
            std::string{pass_name} +
            "' gpu_draw_source commands buffer '" +
            source.commands +
            "' requires command_layout 'indexed_draw'");
    }
    if (count.command_layout !=
        FrameGraphBufferCommandLayout::
            draw_count) {
        throw std::runtime_error(
            "Material pass '" +
            std::string{pass_name} +
            "' gpu_draw_source count buffer '" +
            source.count +
            "' requires command_layout 'draw_count'");
    }
    const auto command_bytes =
        static_cast<std::uint64_t>(
            source.max_draw_count) *
        frameGraphIndexedDrawCommandBytes;
    if (source.command_offset >
            commands.size ||
        commands.size - source.command_offset <
            command_bytes) {
        throw std::runtime_error(
            "Material pass '" +
            std::string{pass_name} +
            "' gpu_draw_source command range exceeds buffer '" +
            source.commands + "'");
    }
    if (source.count_offset > count.size ||
        count.size - source.count_offset <
            frameGraphDrawCountBytes) {
        throw std::runtime_error(
            "Material pass '" +
            std::string{pass_name} +
            "' gpu_draw_source count value exceeds buffer '" +
            source.count + "'");
    }
    if (source.layout ==
        GpuDrawSourceLayout::
            draw_queue_segments_v1) {
        const auto &segments =
            requireGpuDrawBuffer(
                definitions, source.segments,
                "segments", pass_name);
        if (segments.host_source !=
            FrameGraphHostBufferSource::
                scene_draw_segments_v1) {
            throw std::runtime_error(
                "Material pass '" +
                std::string{pass_name} +
                "' gpu_draw_source segments buffer '" +
                source.segments +
                "' requires host_source "
                "'scene_draw_segments_v1'");
        }
    }
}


} // namespace

void validateGpuDrawSourceBufferContracts(
    const RenderGraphIR &render_graph,
    std::span<const FrameGraphBufferDefinition>
        buffer_definitions) {
    for (const auto &graph : render_graph.graphs) {
        for (const auto &node : graph.nodes) {
            const auto *material =
                std::get_if<MaterialNodeInfo>(&node.info);
            if (material == nullptr ||
                !material->gpu_draw_source) {
                continue;
            }
            validateGpuDrawSourceBufferContract(
                *material->gpu_draw_source,
                buffer_definitions, node.name);
        }
    }
}

} // namespace Pelican
