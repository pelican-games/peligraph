#pragma once

#include <cstdint>

namespace Pelican {

struct ShaderBindingTableGroupCounts {
    std::uint32_t raygen = 1;
    std::uint32_t miss = 0;
    std::uint32_t hit = 0;

    bool operator==(
        const ShaderBindingTableGroupCounts &) const = default;
};

struct ShaderBindingTableRegionLayout {
    std::uint64_t offset = 0;
    std::uint64_t stride = 0;
    std::uint64_t size = 0;
    std::uint32_t group_count = 0;

    bool operator==(
        const ShaderBindingTableRegionLayout &) const = default;
};

struct ShaderBindingTableLayout {
    ShaderBindingTableRegionLayout raygen;
    ShaderBindingTableRegionLayout miss;
    ShaderBindingTableRegionLayout hit;
    std::uint64_t total_size = 0;

    bool operator==(
        const ShaderBindingTableLayout &) const = default;
};

// Pure SBT packing policy. Offsets are relative to a base address aligned to
// shader_group_base_alignment; runtime allocation is responsible for choosing
// that aligned base address inside its buffer.
ShaderBindingTableLayout calculateShaderBindingTableLayout(
    std::uint32_t shader_group_handle_size,
    std::uint32_t shader_group_handle_alignment,
    std::uint32_t shader_group_base_alignment,
    ShaderBindingTableGroupCounts group_counts);

} // namespace Pelican
