#pragma once

// Compile-only companion for the frozen v1 public headers. physquery.hpp was
// outside src/core/userpublic at the snapshot boundary, but gamecontext.hpp
// included it as part of the public surface. Keep this copy detached from the
// live userpublic tree so the stale fixture cannot silently consume v2 types.

#include <details/ecs/entity.hpp>
#include <geom/quat.hpp>
#include <geom/vec.hpp>

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Pelican::phys {

struct Ray {
    vec3 origin{0.0f, 0.0f, 0.0f};
    vec3 direction{0.0f, 0.0f, 1.0f};
    float max_distance = 3.4028234663852886e38f;
};

struct Sphere {
    vec3 center{0.0f, 0.0f, 0.0f};
    float radius = 0.5f;
};

struct Box {
    vec3 center{0.0f, 0.0f, 0.0f};
    quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    vec3 half_extents{0.5f, 0.5f, 0.5f};
};

struct Capsule {
    vec3 center{0.0f, 0.0f, 0.0f};
    quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    float half_height = 0.5f;
    float radius = 0.5f;
};

using Shape = std::variant<Sphere, Box, Capsule>;

struct ColliderId {
    std::uint64_t value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    auto operator<=>(const ColliderId &) const = default;
};

inline constexpr ColliderId invalid_collider_id{};
using CollisionLayerMask = std::uint32_t;
inline constexpr CollisionLayerMask default_collision_layer = 1U;
inline constexpr CollisionLayerMask all_collision_layers = ~CollisionLayerMask{0};

struct ColliderIdentity {
    ColliderId collider_id = invalid_collider_id;
    std::string name;
    std::optional<GameObjectId> entity;
    std::uint32_t shape_ordinal = 0;
    bool operator==(const ColliderIdentity &) const = default;
};

struct ColliderQueryMetadata {
    CollisionLayerMask layer = default_collision_layer;
    CollisionLayerMask mask = all_collision_layers;
    bool trigger = false;
    bool one_way = false;
    bool operator==(const ColliderQueryMetadata &) const = default;
};

struct QueryFilter {
    CollisionLayerMask layer = default_collision_layer;
    CollisionLayerMask mask = all_collision_layers;
    bool include_triggers = true;
    bool include_one_way = true;
    std::optional<ColliderId> self;
    std::span<const ColliderId> ignored{};
    std::optional<GameObjectId> self_entity;
    std::span<const GameObjectId> ignored_entities{};
};

struct ObjectRaycastHit {
    std::string id;
    float distance = 0.0f;
    vec3 position{};
    vec3 normal{};
};

struct RaycastQueryHit {
    std::string id;
    float distance = 0.0f;
    vec3 position{};
    vec3 normal{};
    ColliderIdentity identity{};
    ColliderQueryMetadata metadata{};
};

struct OverlapHit {
    std::string id;
    ColliderIdentity identity{};
    ColliderQueryMetadata metadata{};
};

struct ShapeCastQueryHit {
    std::string id;
    float time_of_impact = 0.0f;
    float penetration_depth = 0.0f;
    vec3 position{};
    vec3 normal{};
    bool initial_overlap = false;
    ColliderIdentity identity{};
    ColliderQueryMetadata metadata{};
};

} // namespace Pelican::phys
