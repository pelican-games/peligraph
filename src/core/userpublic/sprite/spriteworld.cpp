#include "spriteworld.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Pelican::sprite {
namespace {

bool finite(Bounds2 value) {
    return std::isfinite(value.min_x) && std::isfinite(value.min_y) &&
           std::isfinite(value.max_x) && std::isfinite(value.max_y);
}

std::uint64_t policyKey(const SpriteCommand &command, SortPolicy policy) {
    switch (policy) {
    case SortPolicy::z: return canonicalFloatKey(command.view_depth); // view-space far -> near
    case SortPolicy::y_down: return ~std::uint64_t{canonicalFloatKey(command.pivot_world_y)};
    case SortPolicy::declaration: return command.declaration_seq;
    }
    return 0;
}

bool lessCommand(const SpriteCommand &a, const SpriteCommand &b, SortPolicy policy) {
    if (a.layer != b.layer) return a.layer < b.layer;
    const auto ak = policyKey(a, policy);
    const auto bk = policyKey(b, policy);
    if (ak != bk) return ak < bk;
    if (a.source_entity.index != b.source_entity.index)
        return a.source_entity.index < b.source_entity.index;
    if (a.source_entity.generation != b.source_entity.generation)
        return a.source_entity.generation < b.source_entity.generation;
    return a.source_ordinal < b.source_ordinal;
}

void hashByte(std::uint64_t &hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= UINT64_C(1099511628211);
}

template <class T> void hashValue(std::uint64_t &hash, T value) noexcept {
    const auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
    for (const auto byte : bytes) hashByte(hash, std::to_integer<std::uint8_t>(byte));
}

} // namespace

std::uint64_t StableOrdinalAllocator::issue(std::string_view key) {
    if (key.empty()) throw std::invalid_argument("sprite source stable primitive key must not be empty");
    if (const auto found = values.find(key); found != values.end()) return found->second;
    if (next == std::numeric_limits<std::uint64_t>::max())
        throw std::length_error("sprite source ordinal exhausted");
    const auto value = next++;
    values.emplace(key, value);
    return value;
}

void StableOrdinalAllocator::remove(std::string_view key) noexcept {
    // Deliberately retain the tombstone/value for deterministic re-add.
    (void)key;
}

void StableOrdinalAllocator::resetForReplay() noexcept {
    values.clear();
    next = 0;
}

std::uint64_t DeclarationSequenceAllocator::issue() {
    if (next == std::numeric_limits<std::uint64_t>::max())
        throw std::length_error("sprite declaration sequence exhausted");
    return next++;
}

std::uint32_t canonicalFloatKey(float value) {
    if (!std::isfinite(value)) throw std::invalid_argument("sprite sort value must be finite");
    if (value == 0.0f) value = 0.0f; // canonicalize -0 to +0
    const auto bits = std::bit_cast<std::uint32_t>(value);
    return (bits & UINT32_C(0x80000000)) != 0 ? ~bits : bits ^ UINT32_C(0x80000000);
}

void validateCommand(const SpriteCommand &command) {
    for (const auto value : command.world_transform)
        if (!std::isfinite(value)) throw std::invalid_argument("sprite world transform must be finite");
    for (const auto value : command.pivot)
        if (!std::isfinite(value) || value < 0.0f || value > 1.0f)
            throw std::invalid_argument("sprite pivot must be finite and within 0..1");
    for (const auto value : command.uv_rect)
        if (!std::isfinite(value)) throw std::invalid_argument("sprite UV must be finite");
    for (const auto value : command.color)
        if (!std::isfinite(value)) throw std::invalid_argument("sprite color must be finite");
    if (command.source_texel_extent[0] == 0 || command.source_texel_extent[1] == 0)
        throw std::invalid_argument("sprite source texel extent must be non-zero");
    (void)canonicalFloatKey(command.view_depth);
    (void)canonicalFloatKey(command.pivot_world_y);
    if (!finite(command.canvas_bounds) || command.canvas_bounds.min_x > command.canvas_bounds.max_x ||
        command.canvas_bounds.min_y > command.canvas_bounds.max_y)
        throw std::invalid_argument("sprite canvas bounds must be finite and ordered");
}

bool visible(Bounds2 bounds, Bounds2 view) noexcept {
    return bounds.max_x >= view.min_x && bounds.min_x <= view.max_x &&
           bounds.max_y >= view.min_y && bounds.min_y <= view.max_y;
}

std::vector<std::uint16_t> buildQuadIndices(std::size_t count) {
    if (count > maxQuadsPerChunk)
        throw std::length_error("sprite chunk exceeds 16384 quads");
    std::vector<std::uint16_t> result;
    result.reserve(count * 6);
    for (std::size_t i = 0; i < count; ++i) {
        const auto base = static_cast<std::uint16_t>(i * 4);
        result.insert(result.end(), {base, static_cast<std::uint16_t>(base + 1),
                                     static_cast<std::uint16_t>(base + 2), base,
                                     static_cast<std::uint16_t>(base + 2),
                                     static_cast<std::uint16_t>(base + 3)});
    }
    return result;
}

SpriteFrame buildFrame(std::vector<SpriteCommand> commands, Bounds2 view, SortPolicy policy,
                       StaticChunkCache *cache, SpriteRevisions revisions) {
    if (!finite(view) || view.min_x > view.max_x || view.min_y > view.max_y)
        throw std::invalid_argument("sprite view bounds must be finite and ordered");
    if (cache != nullptr && cache->populated && cache->revisions == revisions &&
        cache->view == view && cache->policy == policy) {
        auto result = cache->frame;
        result.cache_hit = true;
        return result;
    }

    SpriteFrame result;
    result.logical_count = commands.size();
    for (const auto &command : commands) validateCommand(command);
    std::erase_if(commands, [&](const auto &command) { return !visible(command.canvas_bounds, view); });
    result.visible_count = commands.size();
    std::sort(commands.begin(), commands.end(),
              [&](const auto &a, const auto &b) { return lessCommand(a, b, policy); });
    for (std::size_t begin = 0; begin < commands.size(); begin += maxQuadsPerChunk) {
        const auto end = std::min(begin + maxQuadsPerChunk, commands.size());
        SpriteChunk chunk;
        chunk.commands.insert(chunk.commands.end(),
                              std::make_move_iterator(commands.begin() + static_cast<std::ptrdiff_t>(begin)),
                              std::make_move_iterator(commands.begin() + static_cast<std::ptrdiff_t>(end)));
        chunk.indices = buildQuadIndices(chunk.commands.size());
        result.chunks.push_back(std::move(chunk));
    }
    if (cache != nullptr) {
        cache->populated = true;
        cache->revisions = revisions;
        cache->view = view;
        cache->policy = policy;
        cache->frame = result;
    }
    return result;
}

std::uint64_t commandOrderHash(const SpriteFrame &frame) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (const auto &chunk : frame.chunks) {
        for (const auto &command : chunk.commands) {
            for (const auto value : command.world_transform)
                hashValue(hash, canonicalFloatKey(value));
            for (const auto value : command.pivot) hashValue(hash, canonicalFloatKey(value));
            for (const auto value : command.uv_rect) hashValue(hash, canonicalFloatKey(value));
            for (const auto value : command.color) hashValue(hash, canonicalFloatKey(value));
            hashValue(hash, command.layer);
            hashValue(hash, command.source_entity.index);
            hashValue(hash, command.source_entity.generation);
            hashValue(hash, command.source_ordinal);
            hashValue(hash, command.declaration_seq);
            hashValue(hash, canonicalFloatKey(command.view_depth));
            hashValue(hash, canonicalFloatKey(command.pivot_world_y));
            hashValue(hash, command.batch.atlas_asset);
            hashValue(hash, command.batch.texture_page);
            hashValue(hash, static_cast<std::uint8_t>(command.batch.sampler));
            hashValue(hash, static_cast<std::uint8_t>(command.billboard));
            hashValue(hash, command.source_texel_extent[0]);
            hashValue(hash, command.source_texel_extent[1]);
            hashValue(hash, static_cast<std::uint8_t>(command.pixel_snap));
            hashValue(hash, canonicalFloatKey(command.canvas_bounds.min_x));
            hashValue(hash, canonicalFloatKey(command.canvas_bounds.min_y));
            hashValue(hash, canonicalFloatKey(command.canvas_bounds.max_x));
            hashValue(hash, canonicalFloatKey(command.canvas_bounds.max_y));
        }
    }
    return hash;
}

} // namespace Pelican::sprite
