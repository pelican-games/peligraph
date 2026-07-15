#include "joltphysicsprovider.hpp"

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNING_PUSH
JPH_SUPPRESS_WARNINGS
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollisionDispatch.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
JPH_SUPPRESS_WARNING_POP

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <vector>

namespace Pelican::physics_internal {
namespace {

constexpr float kEpsilon = 1.0e-5F;

void ensureJoltInitialized() {
    static const bool initialized = [] {
        JPH::RegisterDefaultAllocator();
        if (JPH::Factory::sInstance == nullptr) {
            // Process-lifetime ownership is deliberate: provider callbacks can
            // race shutdown, while Jolt's global dispatch tables must outlive
            // every shape and callback.
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
        }
        return true;
    }();
    (void)initialized;
}

bool finite(Physics::Vec3V1 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite(Physics::QuatV1 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}

JPH::Vec3 toJolt(Physics::Vec3V1 value) {
    return {value.x, value.y, value.z};
}

JPH::Quat toJoltNormalized(Physics::QuatV1 value) {
    const double length = std::hypot(
        std::hypot(static_cast<double>(value.x), static_cast<double>(value.y)),
        std::hypot(static_cast<double>(value.z), static_cast<double>(value.w)));
    if (!std::isfinite(length) || length <= kEpsilon) return JPH::Quat::sIdentity();
    return {
        static_cast<float>(static_cast<double>(value.x) / length),
        static_cast<float>(static_cast<double>(value.y) / length),
        static_cast<float>(static_cast<double>(value.z) / length),
        static_cast<float>(static_cast<double>(value.w) / length),
    };
}

struct JoltShape {
    JPH::RefConst<JPH::Shape> shape;
    JPH::Vec3 center = JPH::Vec3::sZero();
    JPH::Quat rotation = JPH::Quat::sIdentity();

    [[nodiscard]] JPH::Mat44 transform() const {
        return JPH::Mat44::sRotationTranslation(rotation, center);
    }

    [[nodiscard]] JPH::TransformedShape transformed() const {
        return JPH::TransformedShape{
            JPH::RVec3{center}, rotation, shape.GetPtr(), JPH::BodyID{}};
    }
};

JoltShape makeShape(const Physics::ShapeV1 &input) {
    if (input.reserved != 0 || !finite(input.center) || !finite(input.rotation)) {
        throw std::invalid_argument("invalid Jolt shape descriptor");
    }

    JoltShape result;
    result.center = toJolt(input.center);
    result.rotation = toJoltNormalized(input.rotation);
    switch (input.kind) {
    case Physics::ShapeKindV1::sphere:
        if (!std::isfinite(input.radius) || input.radius < 0.0F)
            throw std::invalid_argument("invalid Jolt sphere");
        if (input.radius == 0.0F) {
            result.shape = new JPH::BoxShape{JPH::Vec3::sZero(), 0.0F};
        } else {
            result.shape = new JPH::SphereShape{input.radius};
        }
        result.rotation = JPH::Quat::sIdentity();
        break;
    case Physics::ShapeKindV1::box:
        if (!finite(input.half_extents) || input.half_extents.x < 0.0F ||
            input.half_extents.y < 0.0F || input.half_extents.z < 0.0F) {
            throw std::invalid_argument("invalid Jolt box");
        }
        result.shape = new JPH::BoxShape{toJolt(input.half_extents), 0.0F};
        break;
    case Physics::ShapeKindV1::capsule:
        if (!std::isfinite(input.half_height) || !std::isfinite(input.radius) ||
            input.half_height < 0.0F || input.radius < 0.0F) {
            throw std::invalid_argument("invalid Jolt capsule");
        }
        if (input.radius == 0.0F) {
            result.shape = new JPH::BoxShape{
                JPH::Vec3{0.0F, input.half_height, 0.0F}, 0.0F};
        } else if (input.half_height == 0.0F) {
            result.shape = new JPH::SphereShape{input.radius};
        } else {
            result.shape = new JPH::CapsuleShape{input.half_height, input.radius};
        }
        break;
    case Physics::ShapeKindV1::provider:
        throw std::invalid_argument("Jolt provider does not accept opaque V1 shapes");
    default:
        throw std::invalid_argument("unknown Jolt shape kind");
    }
    return result;
}

Physics::Vec3V1 normalizeOr(Physics::Vec3V1 value, Physics::Vec3V1 fallback) {
    const float length_squared = value.x * value.x + value.y * value.y + value.z * value.z;
    if (!std::isfinite(length_squared) || length_squared <= kEpsilon * kEpsilon)
        return fallback;
    const float inverse_length = 1.0F / std::sqrt(length_squared);
    return {value.x * inverse_length, value.y * inverse_length, value.z * inverse_length};
}

Physics::Vec3V1 preferredMtdNormal(Physics::Vec3V1 delta) {
    return normalizeOr({-delta.x, -delta.y, -delta.z}, {1.0F, 0.0F, 0.0F});
}

Physics::Vec3V1 toAbi(JPH::Vec3Arg value);

float dot(Physics::Vec3V1 lhs, Physics::Vec3V1 rhs) {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

Physics::Vec3V1 neg(Physics::Vec3V1 value) {
    return {-value.x, -value.y, -value.z};
}

Physics::Vec3V1 rotate(Physics::QuatV1 rotation, Physics::Vec3V1 value) {
    const double rotation_length = std::hypot(
        std::hypot(static_cast<double>(rotation.x), static_cast<double>(rotation.y)),
        std::hypot(static_cast<double>(rotation.z), static_cast<double>(rotation.w)));
    if (!std::isfinite(rotation_length) || rotation_length <= kEpsilon) return value;
    const Physics::Vec3V1 q{
        static_cast<float>(rotation.x / rotation_length),
        static_cast<float>(rotation.y / rotation_length),
        static_cast<float>(rotation.z / rotation_length),
    };
    const float qw = static_cast<float>(rotation.w / rotation_length);
    const Physics::Vec3V1 twice_cross{
        2.0F * (q.y * value.z - q.z * value.y),
        2.0F * (q.z * value.x - q.x * value.z),
        2.0F * (q.x * value.y - q.y * value.x),
    };
    return {
        value.x + qw * twice_cross.x +
            (q.y * twice_cross.z - q.z * twice_cross.y),
        value.y + qw * twice_cross.y +
            (q.z * twice_cross.x - q.x * twice_cross.z),
        value.z + qw * twice_cross.z +
            (q.x * twice_cross.y - q.y * twice_cross.x),
    };
}

float maximumProjection(const Physics::ShapeV1 &shape, Physics::Vec3V1 normal) {
    const float center_projection = dot(shape.center, normal);
    switch (shape.kind) {
    case Physics::ShapeKindV1::sphere:
        return center_projection + shape.radius;
    case Physics::ShapeKindV1::box: {
        const std::array axes{
            rotate(shape.rotation, {1.0F, 0.0F, 0.0F}),
            rotate(shape.rotation, {0.0F, 1.0F, 0.0F}),
            rotate(shape.rotation, {0.0F, 0.0F, 1.0F}),
        };
        return center_projection +
               std::abs(dot(axes[0], normal)) * shape.half_extents.x +
               std::abs(dot(axes[1], normal)) * shape.half_extents.y +
               std::abs(dot(axes[2], normal)) * shape.half_extents.z;
    }
    case Physics::ShapeKindV1::capsule: {
        const auto axis = rotate(shape.rotation, {0.0F, 1.0F, 0.0F});
        return center_projection + std::abs(dot(axis, normal)) * shape.half_height +
               shape.radius;
    }
    case Physics::ShapeKindV1::provider:
        break;
    }
    throw std::invalid_argument("Jolt provider cannot project custom shapes");
}

float penetrationDepthAlong(Physics::Vec3V1 normal,
                            const Physics::ShapeV1 &moving,
                            const Physics::ShapeV1 &collider) {
    return maximumProjection(collider, normal) +
           maximumProjection(moving, neg(normal));
}

bool canonicalNormalGreater(Physics::Vec3V1 lhs, Physics::Vec3V1 rhs,
                            Physics::Vec3V1 preferred) {
    const std::array lhs_key{dot(lhs, preferred), lhs.x, lhs.y, lhs.z};
    const std::array rhs_key{dot(rhs, preferred), rhs.x, rhs.y, rhs.z};
    return lhs_key > rhs_key;
}

struct MtdCandidate {
    float depth = 0.0F;
    Physics::Vec3V1 position{};
    Physics::Vec3V1 normal{1.0F, 0.0F, 0.0F};
};

std::optional<JPH::ShapeCastResult> castAgainst(
    const JPH::ShapeCast &cast, const JPH::ShapeCastSettings &settings,
    const JoltShape &collider) {
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    JPH::CollisionDispatch::sCastShapeVsShapeWorldSpace(
        cast, settings, collider.shape.GetPtr(), JPH::Vec3::sOne(),
        JPH::ShapeFilter{}, collider.transform(), JPH::SubShapeIDCreator{},
        JPH::SubShapeIDCreator{}, collector);
    if (!collector.HadHit()) return std::nullopt;
    return collector.mHit;
}

std::optional<MtdCandidate> mtdCandidate(
    const JPH::ShapeCastResult &result, const Physics::ShapeV1 &moving,
    const Physics::ShapeV1 &collider, Physics::Vec3V1 fallback) {
    if (result.mFraction > kEpsilon || result.mPenetrationDepth <= kEpsilon) {
        return std::nullopt;
    }
    const auto normal = normalizeOr(toAbi(-result.mPenetrationAxis), fallback);
    const float depth = penetrationDepthAlong(normal, moving, collider);
    if (!std::isfinite(depth) || depth <= Physics::shapeCastContactEpsilonV2) {
        return std::nullopt;
    }
    return MtdCandidate{depth, toAbi(result.mContactPointOn2), normal};
}

MtdCandidate canonicalMtd(
    const JPH::ShapeCast &cast, const JPH::ShapeCastSettings &settings,
    const JoltShape &collider_shape, const Physics::ShapeV1 &moving,
    const Physics::ShapeV1 &collider, Physics::Vec3V1 delta,
    const JPH::ShapeCastResult &initial_result) {
    const auto preferred = preferredMtdNormal(delta);
    const Physics::Vec3V1 fallback{
        moving.center.x - collider.center.x,
        moving.center.y - collider.center.y,
        moving.center.z - collider.center.z,
    };
    std::vector<MtdCandidate> candidates;
    if (const auto original = mtdCandidate(
            initial_result, moving, collider, normalizeOr(fallback, preferred))) {
        candidates.push_back(*original);
    }

    constexpr float bias_magnitude = 1.0e-3F;
    const std::array bias_directions{
        preferred,
        Physics::Vec3V1{1.0F, 0.0F, 0.0F},
        Physics::Vec3V1{0.0F, 1.0F, 0.0F},
        Physics::Vec3V1{0.0F, 0.0F, 1.0F},
    };
    for (const auto direction : bias_directions) {
        const auto biased_cast = cast.PostTranslated(
            JPH::Vec3{direction.x * bias_magnitude,
                      direction.y * bias_magnitude,
                      direction.z * bias_magnitude});
        const auto biased_result = castAgainst(biased_cast, settings, collider_shape);
        if (!biased_result) continue;
        if (const auto candidate = mtdCandidate(
                *biased_result, moving, collider, normalizeOr(fallback, preferred))) {
            candidates.push_back(*candidate);
        }
    }

    if (candidates.empty()) {
        return MtdCandidate{
            initial_result.mPenetrationDepth,
            toAbi(initial_result.mContactPointOn2),
            normalizeOr(toAbi(-initial_result.mPenetrationAxis), preferred),
        };
    }

    const float minimum_depth = std::min_element(
        candidates.begin(), candidates.end(),
        [](const MtdCandidate &lhs, const MtdCandidate &rhs) {
            return lhs.depth < rhs.depth;
        })->depth;
    auto selected = candidates.begin();
    bool selected_in_tie = false;
    for (auto candidate = candidates.begin(); candidate != candidates.end(); ++candidate) {
        if (candidate->depth > minimum_depth + Physics::shapeCastTieEpsilonV2) continue;
        if (!selected_in_tie ||
            canonicalNormalGreater(candidate->normal, selected->normal, preferred)) {
            selected = candidate;
            selected_in_tie = true;
        }
    }
    return *selected;
}

bool handleInitialSphereContact(
    const Physics::ShapeV1 &moving, Physics::Vec3V1 delta,
    const Physics::ShapeV1 &collider, std::uint32_t collider_index,
    Physics::ProviderShapeCastHitV2 &out_hit, bool &out_has_hit) {
    if (moving.kind != Physics::ShapeKindV1::sphere ||
        collider.kind != Physics::ShapeKindV1::sphere) {
        return false;
    }

    const Physics::Vec3V1 center_delta{
        moving.center.x - collider.center.x,
        moving.center.y - collider.center.y,
        moving.center.z - collider.center.z,
    };
    const float center_distance = std::sqrt(
        center_delta.x * center_delta.x + center_delta.y * center_delta.y +
        center_delta.z * center_delta.z);
    const float signed_depth = moving.radius + collider.radius - center_distance;
    if (signed_depth < -Physics::shapeCastContactEpsilonV2) return false;

    const auto normal = normalizeOr(center_delta, preferredMtdNormal(delta));
    const float approach = delta.x * normal.x + delta.y * normal.y +
                           delta.z * normal.z;
    const bool initial_overlap = signed_depth > Physics::shapeCastContactEpsilonV2;
    out_has_hit = initial_overlap || approach < -kEpsilon;
    if (!out_has_hit) return true;

    out_hit = Physics::ProviderShapeCastHitV2{
        .collider_index = collider_index,
        .flags = initial_overlap ? Physics::shape_cast_initial_overlap : 0U,
        .time_of_impact = 0.0F,
        .penetration_depth = initial_overlap ? signed_depth : 0.0F,
        .position = {
            collider.center.x + normal.x * collider.radius,
            collider.center.y + normal.y * collider.radius,
            collider.center.z + normal.z * collider.radius,
        },
        .normal = normal,
    };
    return true;
}

Physics::Vec3V1 toAbi(JPH::Vec3Arg value) {
    return {value.GetX(), value.GetY(), value.GetZ()};
}

Physics::Status raycastAll(void *, const Physics::ProviderRaycastQueryV1 *query,
                           Physics::ProviderRaycastHitV1 *hits,
                           std::uint32_t hit_capacity,
                           std::uint32_t *out_hit_count) noexcept {
    if (query == nullptr || out_hit_count == nullptr || query->reserved != 0 ||
        query->ray.reserved != 0 || !finite(query->ray.origin) ||
        !finite(query->ray.direction) || !std::isfinite(query->ray.max_distance) ||
        query->ray.max_distance < 0.0F ||
        (query->collider_count != 0 && query->colliders == nullptr) ||
        (hit_capacity != 0 && hits == nullptr)) {
        return Physics::Status::invalid_argument;
    }

    try {
        ensureJoltInitialized();
        const auto direction = normalizeOr(query->ray.direction, {});
        const float direction_length_squared = direction.x * direction.x +
                                               direction.y * direction.y +
                                               direction.z * direction.z;
        std::uint32_t required = 0;
        if (direction_length_squared > kEpsilon * kEpsilon) {
            const JPH::RRayCast ray{
                JPH::RVec3{toJolt(query->ray.origin)},
                toJolt(direction),
            };
            const float maximum_fraction =
                query->ray.max_distance == std::numeric_limits<float>::max()
                    ? query->ray.max_distance
                    : std::nextafter(query->ray.max_distance,
                                     std::numeric_limits<float>::infinity());
            JPH::RayCastSettings settings;
            settings.SetBackFaceMode(JPH::EBackFaceMode::CollideWithBackFaces);
            settings.mTreatConvexAsSolid = false;
            for (std::uint32_t index = 0; index < query->collider_count; ++index) {
                const auto shape = makeShape(query->colliders[index]);
                const auto transformed = shape.transformed();
                // A unit direction makes Jolt's ray fraction a world-space
                // distance. Extending the collector bound preserves Pelican's
                // max-distance semantics without multiplying a ray by FLT_MAX.
                JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
                collector.ResetEarlyOutFraction(maximum_fraction);
                transformed.CastRay(ray, settings, collector);
                if (!collector.HadHit() || collector.mHit.mFraction < 0.0F ||
                    collector.mHit.mFraction > query->ray.max_distance + kEpsilon) {
                    continue;
                }
                const float distance = collector.mHit.mFraction;

                if (required < hit_capacity) {
                    const auto position = Physics::Vec3V1{
                        query->ray.origin.x + direction.x * distance,
                        query->ray.origin.y + direction.y * distance,
                        query->ray.origin.z + direction.z * distance,
                    };
                    auto normal = toAbi(transformed.GetWorldSpaceSurfaceNormal(
                        collector.mHit.mSubShapeID2,
                        JPH::RVec3{toJolt(position)}));
                    normal = normalizeOr(normal, {-direction.x, -direction.y, -direction.z});
                    hits[required] = Physics::ProviderRaycastHitV1{
                        .collider_index = index,
                        .distance = distance,
                        .normal = normal,
                    };
                }
                ++required;
            }
        }
        *out_hit_count = required;
        return required > hit_capacity ? Physics::Status::buffer_too_small
                                       : Physics::Status::ok;
    } catch (const std::bad_alloc &) {
        return Physics::Status::out_of_memory;
    } catch (const std::invalid_argument &) {
        return Physics::Status::invalid_argument;
    } catch (...) {
        return Physics::Status::provider_error;
    }
}

Physics::Status overlapAll(void *, const Physics::ProviderOverlapQueryV1 *query,
                           Physics::ProviderOverlapHitV1 *hits,
                           std::uint32_t hit_capacity,
                           std::uint32_t *out_hit_count) noexcept {
    if (query == nullptr || out_hit_count == nullptr || query->reserved != 0 ||
        (query->collider_count != 0 && query->colliders == nullptr) ||
        (hit_capacity != 0 && hits == nullptr)) {
        return Physics::Status::invalid_argument;
    }

    try {
        ensureJoltInitialized();
        const auto query_shape = makeShape(query->shape);
        const JPH::CollideShapeSettings settings;
        std::uint32_t required = 0;
        for (std::uint32_t index = 0; index < query->collider_count; ++index) {
            const auto collider_shape = makeShape(query->colliders[index]);
            JPH::AnyHitCollisionCollector<JPH::CollideShapeCollector> collector;
            JPH::CollisionDispatch::sCollideShapeVsShape(
                query_shape.shape.GetPtr(), collider_shape.shape.GetPtr(),
                JPH::Vec3::sOne(), JPH::Vec3::sOne(),
                query_shape.transform(), collider_shape.transform(),
                JPH::SubShapeIDCreator{}, JPH::SubShapeIDCreator{}, settings, collector);
            if (!collector.HadHit()) continue;
            if (required < hit_capacity) {
                hits[required] = Physics::ProviderOverlapHitV1{.collider_index = index};
            }
            ++required;
        }
        *out_hit_count = required;
        return required > hit_capacity ? Physics::Status::buffer_too_small
                                       : Physics::Status::ok;
    } catch (const std::bad_alloc &) {
        return Physics::Status::out_of_memory;
    } catch (const std::invalid_argument &) {
        return Physics::Status::invalid_argument;
    } catch (...) {
        return Physics::Status::provider_error;
    }
}

Physics::Status shapeCastAll(void *, const Physics::ProviderShapeCastQueryV2 *query,
                             Physics::ProviderShapeCastHitV2 *hits,
                             std::uint32_t hit_capacity,
                             std::uint32_t *out_hit_count) noexcept {
    if (query == nullptr || out_hit_count == nullptr || query->reserved0 != 0 ||
        query->reserved1 != 0 || !finite(query->delta) ||
        (query->collider_count != 0 && query->colliders == nullptr) ||
        (hit_capacity != 0 && hits == nullptr)) {
        return Physics::Status::invalid_argument;
    }

    try {
        ensureJoltInitialized();
        const auto moving_shape = makeShape(query->shape);
        const auto delta = toJolt(query->delta);
        const auto cast = JPH::ShapeCast::sFromWorldTransform(
            moving_shape.shape.GetPtr(), JPH::Vec3::sOne(),
            moving_shape.transform(), delta);
        JPH::ShapeCastSettings settings;
        settings.SetBackFaceMode(JPH::EBackFaceMode::CollideWithBackFaces);
        settings.mReturnDeepestPoint = true;

        std::uint32_t required = 0;
        for (std::uint32_t index = 0; index < query->collider_count; ++index) {
            const auto collider_shape = makeShape(query->colliders[index]);
            Physics::ProviderShapeCastHitV2 analytic_hit{};
            bool analytic_has_hit = false;
            if (handleInitialSphereContact(query->shape, query->delta,
                                           query->colliders[index], index,
                                           analytic_hit, analytic_has_hit)) {
                if (analytic_has_hit) {
                    if (required < hit_capacity) hits[required] = analytic_hit;
                    ++required;
                }
                continue;
            }
            const auto cast_result = castAgainst(cast, settings, collider_shape);
            if (!cast_result) continue;

            const auto &result = *cast_result;
            auto normal = normalizeOr(toAbi(-result.mPenetrationAxis),
                                      normalizeOr(
                                          {query->shape.center.x -
                                               query->colliders[index].center.x,
                                           query->shape.center.y -
                                               query->colliders[index].center.y,
                                           query->shape.center.z -
                                               query->colliders[index].center.z},
                                          {-query->delta.x, -query->delta.y,
                                           -query->delta.z}));
            const bool initial_overlap =
                result.mFraction <= kEpsilon &&
                result.mPenetrationDepth > kEpsilon;
            std::optional<MtdCandidate> canonical_mtd;
            if (initial_overlap) {
                canonical_mtd = canonicalMtd(
                    cast, settings, collider_shape, query->shape,
                    query->colliders[index], query->delta, result);
                normal = canonical_mtd->normal;
            }
            if (!initial_overlap && result.mFraction <= kEpsilon) {
                const float approach = query->delta.x * normal.x +
                                       query->delta.y * normal.y +
                                       query->delta.z * normal.z;
                if (approach >= -kEpsilon) continue;
            }
            if (result.mFraction < -kEpsilon ||
                result.mFraction > 1.0F + kEpsilon) {
                continue;
            }

            if (required < hit_capacity) {
                hits[required] = Physics::ProviderShapeCastHitV2{
                    .collider_index = index,
                    .flags = initial_overlap
                                 ? Physics::shape_cast_initial_overlap
                                 : 0U,
                    .time_of_impact =
                        std::clamp(result.mFraction, 0.0F, 1.0F),
                    .penetration_depth =
                        initial_overlap ? canonical_mtd->depth : 0.0F,
                    .position = initial_overlap
                                    ? canonical_mtd->position
                                    : toAbi(result.mContactPointOn2),
                    .normal = normal,
                };
            }
            ++required;
        }
        *out_hit_count = required;
        return required > hit_capacity ? Physics::Status::buffer_too_small
                                       : Physics::Status::ok;
    } catch (const std::bad_alloc &) {
        return Physics::Status::out_of_memory;
    } catch (const std::invalid_argument &) {
        return Physics::Status::invalid_argument;
    } catch (...) {
        return Physics::Status::provider_error;
    }
}

} // namespace

const Physics::ProviderV2 &joltProviderV2() noexcept {
    static const Physics::ProviderV2 provider = [] {
        auto value = Physics::descriptor<Physics::ProviderV2>();
        value.capability_bits = Physics::builtinQueryCapabilitiesV2;
        value.name_utf8 = "pelican.jolt";
        value.name_size = 12;
        value.raycast_all = raycastAll;
        value.overlap_all = overlapAll;
        value.shape_cast_all = shapeCastAll;
        return value;
    }();
    return provider;
}

} // namespace Pelican::physics_internal
