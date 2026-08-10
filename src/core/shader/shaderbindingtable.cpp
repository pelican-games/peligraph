#include "shaderbindingtable.hpp"

#include <limits>
#include <stdexcept>

namespace Pelican {
namespace {

std::uint64_t checkedAdd(
    std::uint64_t lhs, std::uint64_t rhs) {
    if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs) {
        throw std::overflow_error(
            "pelican.sbt.layout_overflow@1: SBT byte offset overflow");
    }
    return lhs + rhs;
}

std::uint64_t checkedMultiply(
    std::uint64_t lhs, std::uint64_t rhs) {
    if (rhs != 0 &&
        lhs > std::numeric_limits<std::uint64_t>::max() / rhs) {
        throw std::overflow_error(
            "pelican.sbt.layout_overflow@1: SBT byte size overflow");
    }
    return lhs * rhs;
}

std::uint64_t alignUp(
    std::uint64_t value, std::uint64_t alignment) {
    const auto remainder = value % alignment;
    return remainder == 0
               ? value
               : checkedAdd(value, alignment - remainder);
}

ShaderBindingTableRegionLayout appendRegion(
    std::uint64_t &cursor, std::uint64_t record_stride,
    std::uint64_t base_alignment, std::uint32_t group_count) {
    if (group_count == 0) return {};
    const auto offset = alignUp(cursor, base_alignment);
    const auto size = checkedMultiply(
        record_stride, group_count);
    cursor = checkedAdd(offset, size);
    return {
        .offset = offset,
        .stride = record_stride,
        .size = size,
        .group_count = group_count,
    };
}

} // namespace

ShaderBindingTableLayout calculateShaderBindingTableLayout(
    std::uint32_t shader_group_handle_size,
    std::uint32_t shader_group_handle_alignment,
    std::uint32_t shader_group_base_alignment,
    ShaderBindingTableGroupCounts group_counts) {
    if (shader_group_handle_size == 0 ||
        shader_group_handle_alignment == 0 ||
        shader_group_base_alignment == 0) {
        throw std::invalid_argument(
            "pelican.sbt.invalid_alignment@1: SBT handle size and "
            "alignments must be non-zero");
    }
    if (group_counts.raygen != 1) {
        throw std::invalid_argument(
            "pelican.sbt.invalid_raygen_group_count@1: a trace-rays SBT "
            "requires exactly one raygen group");
    }

    const auto stride = alignUp(
        shader_group_handle_size,
        shader_group_handle_alignment);
    std::uint64_t cursor = 0;
    ShaderBindingTableLayout result;
    result.raygen = appendRegion(
        cursor, stride, shader_group_base_alignment,
        group_counts.raygen);
    result.miss = appendRegion(
        cursor, stride, shader_group_base_alignment,
        group_counts.miss);
    result.hit = appendRegion(
        cursor, stride, shader_group_base_alignment,
        group_counts.hit);
    result.total_size = cursor;
    return result;
}

} // namespace Pelican
