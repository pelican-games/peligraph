#include "indirectdrawlimits.hpp"

#include <algorithm>
#include <stdexcept>

namespace Pelican::renderer_detail {

std::vector<IndirectDrawSegment>
splitIndirectDrawRange(std::size_t first_command, std::size_t end_command,
                       std::uint32_t max_draw_count) {
    if (end_command < first_command) {
        throw std::invalid_argument(
            "Indirect draw range ends before its first command");
    }
    if (max_draw_count == 0) {
        throw std::invalid_argument(
            "Vulkan maxDrawIndirectCount must be greater than zero");
    }

    std::vector<IndirectDrawSegment> segments;
    const auto command_count = end_command - first_command;
    segments.reserve(command_count / max_draw_count +
                     (command_count % max_draw_count != 0));

    while (first_command < end_command) {
        const auto draw_count = std::min<std::size_t>(
            end_command - first_command, max_draw_count);
        segments.push_back(IndirectDrawSegment{
            .first_command = first_command,
            .draw_count = static_cast<std::uint32_t>(draw_count),
        });
        first_command += draw_count;
    }
    return segments;
}

} // namespace Pelican::renderer_detail
