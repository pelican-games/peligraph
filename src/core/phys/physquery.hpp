#pragma once

#include "../userpublic/geom/quat.hpp"
#include "../userpublic/geom/vec.hpp"
#include "../userpublic/details/ecs/entity.hpp"

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

struct ColliderIdentity {
    ColliderId collider_id = invalid_collider_id;
    std::string name;
    std::optional<GameObjectId> entity;
    std::uint32_t shape_ordinal = 0;

    bool operator==(const ColliderIdentity &) const = default;
};

using CollisionLayerMask = std::uint32_t;
inline constexpr CollisionLayerMask default_collision_layer = 1U;
inline constexpr CollisionLayerMask all_collision_layers = ~CollisionLayerMask{0};
inline constexpr float shapeCastContactEpsilon = 1.0e-5F;
inline constexpr float shapeCastTieEpsilon = 1.0e-5F;

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

struct Collider {
    // Compatibility label retained for existing callers. New query results also
    // expose the complete identity below.
    std::string id;
    Shape shape;
    ColliderIdentity identity{};
    ColliderQueryMetadata metadata{};
};

struct RaycastHit {
    float distance = 0.0f;
    vec3 position{0.0f, 0.0f, 0.0f};
    vec3 normal{0.0f, 1.0f, 0.0f};
};

struct ObjectRaycastHit {
    std::string id;
    float distance = 0.0f;
    vec3 position{0.0f, 0.0f, 0.0f};
    vec3 normal{0.0f, 1.0f, 0.0f};
};

struct RaycastQueryHit {
    std::string id;
    float distance = 0.0f;
    vec3 position{0.0f, 0.0f, 0.0f};
    vec3 normal{0.0f, 1.0f, 0.0f};
    ColliderIdentity identity{};
    ColliderQueryMetadata metadata{};
};

struct OverlapHit {
    std::string id;
    ColliderIdentity identity{};
    ColliderQueryMetadata metadata{};
};

// A translational cast keeps the shape orientation fixed while moving its
// center by delta. time_of_impact is in [0, 1], position is on the stationary
// collider, and normal points in the direction that moves the cast shape out.
// For an initial penetration deeper than shapeCastContactEpsilon, TOI is zero,
// initial_overlap is true, and normal * penetration_depth is its MTD. Touching
// within that epsilon is a TOI-zero hit only while approaching; stationary or
// separating contact is omitted. A zero delta is therefore a depenetration
// query. Non-finite or negative shape input produces no pure-query result.
//
// MTD candidates whose depths differ by at most shapeCastTieEpsilon use the
// lexicographically greatest key (dot(normal, preferred), normal.x, normal.y,
// normal.z), where preferred is normalized(-delta), or +X for zero delta.
// shapeCastAll clusters TOIs from the lowest raw value with the same epsilon,
// then orders that cluster by complete collider identity and shape ordinal.
struct ShapeCastHit {
    float time_of_impact = 0.0f;
    float penetration_depth = 0.0f;
    vec3 position{0.0f, 0.0f, 0.0f};
    vec3 normal{0.0f, 1.0f, 0.0f};
    bool initial_overlap = false;
};

struct ShapeCastQueryHit {
    std::string id;
    float time_of_impact = 0.0f;
    float penetration_depth = 0.0f;
    vec3 position{0.0f, 0.0f, 0.0f};
    vec3 normal{0.0f, 1.0f, 0.0f};
    bool initial_overlap = false;
    ColliderIdentity identity{};
    ColliderQueryMetadata metadata{};
};

std::optional<RaycastHit> raycast(const Ray &ray, const Sphere &sphere);
std::optional<RaycastHit> raycast(const Ray &ray, const Box &box);
std::optional<RaycastHit> raycast(const Ray &ray, const Capsule &capsule);
std::optional<RaycastHit> raycast(const Ray &ray, const Shape &shape);

bool overlaps(const Sphere &lhs, const Sphere &rhs);
bool overlaps(const Sphere &lhs, const Box &rhs);
bool overlaps(const Box &lhs, const Sphere &rhs);
bool overlaps(const Sphere &lhs, const Capsule &rhs);
bool overlaps(const Capsule &lhs, const Sphere &rhs);
bool overlaps(const Box &lhs, const Box &rhs);
bool overlaps(const Capsule &lhs, const Capsule &rhs);
bool overlaps(const Box &lhs, const Capsule &rhs);
bool overlaps(const Capsule &lhs, const Box &rhs);
bool overlaps(const Shape &lhs, const Shape &rhs);

std::optional<ShapeCastHit> shapeCast(const Shape &moving_shape, vec3 delta,
                                      const Shape &collider_shape);

// Legacy name-only colliders receive a deterministic compatibility identity.
// PhysWorld supplies persistent ids and a full entity generation instead.
[[nodiscard]] ColliderIdentity effectiveColliderIdentity(const Collider &collider);
[[nodiscard]] bool colliderIdentityLess(const ColliderIdentity &lhs, const ColliderIdentity &rhs);

std::vector<RaycastQueryHit> raycastAll(const Ray &ray, std::span<const Collider> colliders,
                                        const QueryFilter &filter = {});

// Original ABI-compatible closest-hit entry point.
std::optional<ObjectRaycastHit> raycastClosest(const Ray &ray,
                                               std::span<const Collider> colliders);
std::optional<RaycastQueryHit> raycastClosest(const Ray &ray,
                                              std::span<const Collider> colliders,
                                              const QueryFilter &filter);
std::vector<OverlapHit> overlapAllHits(const Shape &shape, std::span<const Collider> colliders,
                                       const QueryFilter &filter = {});
std::vector<ShapeCastQueryHit> shapeCastAll(
    const Shape &moving_shape, vec3 delta, std::span<const Collider> colliders,
    const QueryFilter &filter = {});
std::optional<ShapeCastQueryHit> shapeCastClosest(
    const Shape &moving_shape, vec3 delta, std::span<const Collider> colliders,
    const QueryFilter &filter = {});

// Compatibility adapter for the original name-only overlap API.
std::vector<std::string> overlapAll(const Shape &shape,
                                    std::span<const Collider> colliders);
std::vector<std::string> overlapAll(const Shape &shape,
                                    std::span<const Collider> colliders,
                                    const QueryFilter &filter);

} // namespace Pelican::phys
