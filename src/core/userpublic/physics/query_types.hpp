#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Pelican::Physics {

inline constexpr std::uint32_t descriptorVersionV1 = 1;
inline constexpr std::size_t descriptorHeaderSize = 16;
inline constexpr std::uint32_t maximumProviderNameBytesV1 = 127;

enum class Status : std::uint32_t {
    ok = 0,
    buffer_too_small = 1,
    invalid_argument = 2,
    unsupported_version = 3,
    reserved_not_zero = 4,
    unavailable = 5,
    duplicate_provider = 6,
    stale_provider = 7,
    wrong_owner = 8,
    provider_error = 9,
    out_of_memory = 10,
};

struct DescriptorHeaderV1 {
    std::uint32_t struct_size = sizeof(DescriptorHeaderV1);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
};

template <class T> constexpr T descriptor() noexcept {
    T value{};
    value.struct_size = sizeof(T);
    value.version = descriptorVersionV1;
    return value;
}

struct Vec3V1 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

struct QuatV1 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 1.0F;
};

struct EntityIdV1 {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
};

struct ColliderIdentityV1 {
    std::uint64_t collider_id = 0;
    EntityIdV1 entity{};
    std::uint32_t has_entity = 0;
    std::uint32_t shape_ordinal = 0;
};

struct ColliderMetadataV1 {
    std::uint32_t layer = 1U;
    std::uint32_t mask = ~std::uint32_t{0};
    std::uint32_t flags = 0;
    std::uint32_t reserved = 0;
};

enum ColliderMetadataFlagBitsV1 : std::uint32_t {
    collider_trigger = 1U << 0U,
    collider_one_way = 1U << 1U,
};

enum QueryFilterFlagBitsV1 : std::uint32_t {
    query_include_triggers = 1U << 0U,
    query_include_one_way = 1U << 1U,
};

struct QueryFilterV1 {
    std::uint32_t layer = 1U;
    std::uint32_t mask = ~std::uint32_t{0};
    std::uint32_t flags = query_include_triggers | query_include_one_way;
    std::uint32_t reserved0 = 0;
    std::uint64_t self_collider_id = 0;
    EntityIdV1 self_entity{};
    std::uint32_t has_self_entity = 0;
    std::uint32_t ignored_collider_count = 0;
    const std::uint64_t *ignored_collider_ids = nullptr;
    std::uint32_t ignored_entity_count = 0;
    std::uint32_t reserved1 = 0;
    const EntityIdV1 *ignored_entities = nullptr;
};

enum class ShapeKindV1 : std::uint32_t {
    sphere = 1,
    box = 2,
    capsule = 3,
    provider = 0x80000000U,
};

// Standard shapes use the geometric fields. A provider-owned custom shape uses
// provider_id/provider_shape and is accepted only when the active provider
// advertises that capability in a future additive service revision.
struct ShapeV1 {
    ShapeKindV1 kind = ShapeKindV1::sphere;
    std::uint32_t reserved = 0;
    Vec3V1 center{};
    float radius = 0.5F;
    QuatV1 rotation{};
    Vec3V1 half_extents{0.5F, 0.5F, 0.5F};
    float half_height = 0.5F;
    std::uint64_t provider_id = 0;
    std::uint64_t provider_shape = 0;
};

struct RayV1 {
    Vec3V1 origin{};
    Vec3V1 direction{0.0F, 0.0F, 1.0F};
    float max_distance = 3.4028234663852886e38F;
    std::uint32_t reserved = 0;
};

struct RaycastQueryV1 {
    std::uint32_t struct_size = sizeof(RaycastQueryV1);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    RayV1 ray{};
    QueryFilterV1 filter{};
};

struct OverlapQueryV1 {
    std::uint32_t struct_size = sizeof(OverlapQueryV1);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    ShapeV1 shape{};
    QueryFilterV1 filter{};
};

struct RaycastHitV1 {
    ColliderIdentityV1 identity{};
    ColliderMetadataV1 metadata{};
    float distance = 0.0F;
    std::uint32_t reserved = 0;
    Vec3V1 position{};
    Vec3V1 normal{0.0F, 1.0F, 0.0F};
};

struct OverlapHitV1 {
    ColliderIdentityV1 identity{};
    ColliderMetadataV1 metadata{};
};

struct ProviderRaycastQueryV1 {
    // Direction magnitude is ignored. A zero direction produces no hits;
    // max_distance is an inclusive world-space bound. For a ray that begins
    // inside a standard closed shape, report its first forward exit surface.
    RayV1 ray{};
    const ShapeV1 *colliders = nullptr;
    std::uint32_t collider_count = 0;
    std::uint32_t reserved = 0;
};

struct ProviderOverlapQueryV1 {
    // Touching standard shapes count as overlapping.
    ShapeV1 shape{};
    const ShapeV1 *colliders = nullptr;
    std::uint32_t collider_count = 0;
    std::uint32_t reserved = 0;
};

struct ProviderRaycastHitV1 {
    // A V1 provider returns at most one hit per input collider. The engine
    // validates the index/distance/normal, reconstructs the position from the
    // canonical ray, restores identity/metadata, and applies canonical order.
    std::uint32_t collider_index = 0;
    std::uint32_t reserved = 0;
    float distance = 0.0F;
    Vec3V1 normal{0.0F, 1.0F, 0.0F};
};

struct ProviderOverlapHitV1 {
    // A V1 provider returns at most one hit per input collider.
    std::uint32_t collider_index = 0;
    std::uint32_t reserved = 0;
};

enum PhysicsQueryCapabilityBitsV1 : std::uint64_t {
    query_raycast_all = 1ULL << 0U,
    query_overlap_all = 1ULL << 1U,
};

inline constexpr std::uint64_t builtinQueryCapabilitiesV1 =
    query_raycast_all | query_overlap_all;

using ProviderRaycastAllV1Fn = Status (*)(
    void *, const ProviderRaycastQueryV1 *, ProviderRaycastHitV1 *,
    std::uint32_t, std::uint32_t *) noexcept;
using ProviderOverlapAllV1Fn = Status (*)(
    void *, const ProviderOverlapQueryV1 *, ProviderOverlapHitV1 *,
    std::uint32_t, std::uint32_t *) noexcept;

static_assert(sizeof(DescriptorHeaderV1) == descriptorHeaderSize);
static_assert(std::is_trivially_copyable_v<ShapeV1>);
static_assert(std::is_standard_layout_v<ProviderRaycastHitV1>);
static_assert(std::is_trivially_copyable_v<ProviderRaycastHitV1>);
static_assert(sizeof(ProviderRaycastHitV1) == 24);

} // namespace Pelican::Physics
