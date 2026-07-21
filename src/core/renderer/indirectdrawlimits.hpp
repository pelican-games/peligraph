#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Pelican::renderer_detail {

struct IndirectDrawSegment {
    std::size_t first_command = 0;
    std::uint32_t draw_count = 0;
};

std::vector<IndirectDrawSegment>
splitIndirectDrawRange(std::size_t first_command, std::size_t end_command,
                       std::uint32_t max_draw_count);

} // namespace Pelican::renderer_detail
