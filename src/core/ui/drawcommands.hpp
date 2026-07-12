#pragma once

#include "types.hpp"

#include <cstdint>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace Pelican::ui {

enum class Sampler { Nearest, Linear };

inline constexpr std::size_t maxQuads = 16384;
inline constexpr std::size_t maxUniqueClips = 256;

struct QuadVertex {
    std::array<float, 2> position{};
    std::array<float, 2> uv{};
    std::array<std::uint8_t, 4> color{255, 255, 255, 255};
};

static_assert(sizeof(QuadVertex) == 20);
static_assert(alignof(QuadVertex) == 4);
static_assert(offsetof(QuadVertex, position) == 0);
static_assert(offsetof(QuadVertex, uv) == 8);
static_assert(offsetof(QuadVertex, color) == 16);
static_assert(maxQuads * 4 == 65536);

struct DrawKey {
    std::string pipeline;
    Sampler sampler = Sampler::Nearest;
    std::string texture;
    RectI scissor_px{};
    std::uint16_t texture_page = 0;
    std::uint16_t clip_id = 0;
    bool operator==(const DrawKey &other) const noexcept {
        return pipeline == other.pipeline && texture_page == other.texture_page &&
               sampler == other.sampler && clip_id == other.clip_id;
    }
};

struct QuadCommand {
    DrawKey key;
    RectI rect_px{};
    std::int32_t layer = 0;
    std::int32_t decl_seq = 0;
    std::array<float, 4> uv_rect{0.0f, 0.0f, 1.0f, 1.0f};
    std::array<std::uint8_t, 4> color{255, 255, 255, 255};
    std::string widget_id;
};

struct DrawRun {
    DrawKey key;
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
};

struct DrawBatch {
    std::vector<QuadCommand> quads;
    std::vector<QuadVertex> vertices;
    std::vector<std::uint16_t> indices;
    std::vector<DrawRun> runs;
    std::uint32_t total_index_count = 0;
};

DrawBatch buildDrawBatch(std::vector<QuadCommand> commands, std::string_view document_key = {});

} // namespace Pelican::ui
