#pragma once

#include "types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Pelican::ui {

enum class Sampler { Nearest, Linear };

struct DrawKey {
    std::string pipeline;
    Sampler sampler = Sampler::Nearest;
    std::string texture;
    RectI scissor_px{};
    auto operator<=>(const DrawKey &) const = default;
};

struct QuadCommand {
    DrawKey key;
    RectI rect_px{};
    std::int32_t layer = 0;
    std::int32_t decl_seq = 0;
};

struct DrawRun {
    DrawKey key;
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
};

struct DrawBatch {
    std::vector<QuadCommand> quads;
    std::vector<DrawRun> runs;
    std::uint32_t total_index_count = 0;
};

DrawBatch buildDrawBatch(std::vector<QuadCommand> commands);

} // namespace Pelican::ui
