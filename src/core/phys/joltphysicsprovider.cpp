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
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
JPH_SUPPRESS_WARNING_POP

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

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

} // namespace

const Physics::ProviderV1 &joltProviderV1() noexcept {
    static const Physics::ProviderV1 provider = [] {
        auto value = Physics::descriptor<Physics::ProviderV1>();
        value.capability_bits = Physics::builtinQueryCapabilitiesV1;
        value.name_utf8 = "pelican.jolt";
        value.name_size = 12;
        value.raycast_all = raycastAll;
        value.overlap_all = overlapAll;
        return value;
    }();
    return provider;
}

} // namespace Pelican::physics_internal
