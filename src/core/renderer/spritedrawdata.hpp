#pragma once

#include "spritegpuabi.hpp"
#include "../userpublic/sprite/spriteworld.hpp"

#include <cstdint>
#include <vector>

namespace Pelican::renderer_detail {

struct SpriteDrawRun {
    sprite::SpriteBatchKey key;
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    std::int32_t vertex_offset = 0;
};

struct SpriteDrawData {
    std::vector<sprite::GpuVertex> vertices;
    std::vector<std::uint16_t> indices;
    std::vector<SpriteDrawRun> runs;
};

SpriteDrawData buildSpriteDrawData(const sprite::SpriteFrame &frame);

} // namespace Pelican::renderer_detail
