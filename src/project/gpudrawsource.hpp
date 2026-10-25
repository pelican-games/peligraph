#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace Pelican {

enum class GpuDrawFallback : std::uint8_t {
    cpu_draw_queue,
};

enum class GpuDrawExecutionMode : std::uint8_t {
    automatic,
    cpu,
};

enum class GpuDrawSourceLayout : std::uint8_t {
    fixed_state_v1,
    draw_queue_segments_v1,
};

// RC-3a2: a material pass's `gpu_draw_source` as the author declares it: the
// buffers by name, the byte offsets of the command and count words in them,
// and how the draws are laid out and run. Core's GpuDrawSourceDefinition adds
// the buffers' handles in a GPU generation.
//
// A material pass may replace CPU-compiled fixed-state draw ranges with
// GPU-written indexed indirect commands. Segmented layout retains CPU-owned
// pipeline/material binding and gives each DrawQueue state range disjoint
// command/count output slots.
struct GpuDrawSourceDeclaration {
    std::string commands;
    std::string count;
    std::string segments;
    std::uint32_t max_draw_count = 0;
    std::uint64_t command_offset = 0;
    std::uint64_t count_offset = 0;
    GpuDrawSourceLayout layout =
        GpuDrawSourceLayout::fixed_state_v1;
    GpuDrawFallback fallback =
        GpuDrawFallback::cpu_draw_queue;
    GpuDrawExecutionMode execution =
        GpuDrawExecutionMode::automatic;

    bool operator==(
        const GpuDrawSourceDeclaration &) const = default;
};

struct RenderGraphIR;
struct FrameGraphBufferDefinition;

void validateGpuDrawSourceBufferContracts(
    const RenderGraphIR &render_graph,
    std::span<const FrameGraphBufferDefinition> buffer_definitions);

} // namespace Pelican
