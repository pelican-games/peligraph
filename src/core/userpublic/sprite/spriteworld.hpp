#pragma once

#include "../details/ecs/entity.hpp"
#include "../components/spriteview.hpp"
#include "sampler.hpp"
#include "pixelpolicy.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican::sprite {

inline constexpr std::size_t maxQuadsPerChunk = 16384;

enum class SortPolicy : std::uint8_t { z, y_down, declaration };

struct Bounds2 {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    auto operator<=>(const Bounds2 &) const = default;
};

struct SpriteBatchKey {
    std::uint32_t atlas_asset = 0;
    std::uint16_t texture_page = 0;
    SamplerKey sampler = SamplerKey::linear;
    auto operator<=>(const SpriteBatchKey &) const = default;
};

// World command ABI, deliberately unrelated to ui::QuadCommand. The matrix is
// column-major and transforms the fixed XY/+Z unit quad. GPU expansion belongs
// to S2D-0b; this WP only defines and processes the CPU contract.
struct SpriteCommand {
    std::array<float, 16> world_transform{};
    std::array<float, 2> pivot{0.5f, 0.5f};
    std::array<float, 4> uv_rect{};
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
    SpriteBatchKey batch{};
    std::int16_t layer = 0;
    float view_depth = 0.0f;
    float pivot_world_y = 0.0f;
    std::uint64_t declaration_seq = 0;
    EntityId source_entity{};
    std::uint64_t source_ordinal = 0;
    SpriteBillboard billboard = SpriteBillboard::none;
    std::array<std::uint32_t, 2> source_texel_extent{1, 1};
    PixelSnapReason pixel_snap = PixelSnapReason::not_requested;
    Bounds2 canvas_bounds{};
    bool operator==(const SpriteCommand &) const = default;
};

struct SpriteChunk {
    std::vector<SpriteCommand> commands;
    std::vector<std::uint16_t> indices;
};

struct SpriteFrame {
    std::size_t logical_count = 0;
    std::size_t visible_count = 0;
    std::vector<SpriteChunk> chunks;
    bool cache_hit = false;
};

struct SpriteRevisions {
    std::uint64_t source = 0;
    std::uint64_t transform = 0;
    std::uint64_t atlas = 0;
    std::uint64_t sort = 0;
    auto operator<=>(const SpriteRevisions &) const = default;
};

// Per render-source allocator. Ordinals are issued at logical primitive
// declaration, stored in that primitive, and never derived from container
// iteration. Tombstones are retained, so remove/re-add with the same stable key
// keeps its ordinal. Cache reuse copies it unchanged. Replay resets the allocator
// and must replay declarations in the recorded declaration order, reproducing
// the same values. Recreating the source entity starts a new local namespace;
// full EntityId and declaration_seq still keep the global order total.
class StableOrdinalAllocator {
    std::map<std::string, std::uint64_t, std::less<>> values;
    std::uint64_t next = 0;

  public:
    std::uint64_t issue(std::string_view stable_primitive_key);
    void remove(std::string_view stable_primitive_key) noexcept;
    void resetForReplay() noexcept;
};

// Issued whenever a sprite registration is created. Removal never rewinds the
// sequence and recreating a registration receives a fresh value. Replay resets
// once, then reissues in recorded scene/registration order.
class DeclarationSequenceAllocator {
    std::uint64_t next = 0;

  public:
    std::uint64_t issue();
    void resetForReplay() noexcept { next = 0; }
};

class StaticChunkCache {
    bool populated = false;
    SpriteRevisions revisions{};
    Bounds2 view{};
    SortPolicy policy = SortPolicy::z;
    SpriteFrame frame;
    friend SpriteFrame buildFrame(std::vector<SpriteCommand>, Bounds2, SortPolicy,
                                  StaticChunkCache *, SpriteRevisions);
};

std::uint32_t canonicalFloatKey(float value);
void validateCommand(const SpriteCommand &command);
bool visible(Bounds2 bounds, Bounds2 view) noexcept;
std::vector<std::uint16_t> buildQuadIndices(std::size_t quad_count);
SpriteFrame buildFrame(std::vector<SpriteCommand> commands, Bounds2 view,
                       SortPolicy policy, StaticChunkCache *cache = nullptr,
                       SpriteRevisions revisions = {});
std::uint64_t commandOrderHash(const SpriteFrame &frame);

} // namespace Pelican::sprite
