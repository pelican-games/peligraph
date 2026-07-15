#include "builtinphysicsprovider.hpp"

#include "physquery.hpp"

#include <cmath>
#include <new>
#include <stdexcept>
#include <variant>

namespace Pelican::physics_internal {
namespace {

static_assert(phys::shapeCastContactEpsilon ==
              Physics::shapeCastContactEpsilonV2);
static_assert(phys::shapeCastTieEpsilon == Physics::shapeCastTieEpsilonV2);

phys::Shape fromAbi(const Physics::ShapeV1 &shape) {
    const vec3 center{shape.center.x, shape.center.y, shape.center.z};
    switch (shape.kind) {
    case Physics::ShapeKindV1::sphere:
        return phys::Sphere{center, shape.radius};
    case Physics::ShapeKindV1::box:
        return phys::Box{
            center,
            quat{shape.rotation.x, shape.rotation.y, shape.rotation.z, shape.rotation.w},
            vec3{shape.half_extents.x, shape.half_extents.y, shape.half_extents.z},
        };
    case Physics::ShapeKindV1::capsule:
        return phys::Capsule{
            center,
            quat{shape.rotation.x, shape.rotation.y, shape.rotation.z, shape.rotation.w},
            shape.half_height,
            shape.radius,
        };
    case Physics::ShapeKindV1::provider:
        break;
    }
    throw std::invalid_argument("builtin physics provider does not support custom shapes");
}

Physics::Vec3V1 toAbi(vec3 value) {
    return {value.x, value.y, value.z};
}

bool finite(Physics::Vec3V1 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

Physics::Status raycastAll(void *, const Physics::ProviderRaycastQueryV1 *query,
                           Physics::ProviderRaycastHitV1 *hits,
                           std::uint32_t hit_capacity,
                           std::uint32_t *out_hit_count) noexcept {
    if (query == nullptr || out_hit_count == nullptr || query->reserved != 0 ||
        (query->collider_count != 0 && query->colliders == nullptr) ||
        (hit_capacity != 0 && hits == nullptr)) {
        return Physics::Status::invalid_argument;
    }

    try {
        const phys::Ray ray{
            vec3{query->ray.origin.x, query->ray.origin.y, query->ray.origin.z},
            vec3{query->ray.direction.x, query->ray.direction.y, query->ray.direction.z},
            query->ray.max_distance,
        };
        std::uint32_t required = 0;
        for (std::uint32_t index = 0; index < query->collider_count; ++index) {
            const auto hit = phys::raycast(ray, fromAbi(query->colliders[index]));
            if (!hit) {
                continue;
            }
            if (required < hit_capacity) {
                hits[required] = Physics::ProviderRaycastHitV1{
                    .collider_index = index,
                    .distance = hit->distance,
                    .normal = toAbi(hit->normal),
                };
            }
            ++required;
        }
        *out_hit_count = required;
        return required > hit_capacity ? Physics::Status::buffer_too_small
                                       : Physics::Status::ok;
    } catch (const std::bad_alloc &) {
        return Physics::Status::out_of_memory;
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
        const phys::Shape query_shape = fromAbi(query->shape);
        std::uint32_t required = 0;
        for (std::uint32_t index = 0; index < query->collider_count; ++index) {
            if (!phys::overlaps(query_shape, fromAbi(query->colliders[index]))) {
                continue;
            }
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
        const phys::Shape moving_shape = fromAbi(query->shape);
        const vec3 delta{query->delta.x, query->delta.y, query->delta.z};
        std::uint32_t required = 0;
        for (std::uint32_t index = 0; index < query->collider_count; ++index) {
            const auto hit = phys::shapeCast(
                moving_shape, delta, fromAbi(query->colliders[index]));
            if (!hit) continue;
            if (required < hit_capacity) {
                hits[required] = Physics::ProviderShapeCastHitV2{
                    .collider_index = index,
                    .flags = hit->initial_overlap
                                 ? Physics::shape_cast_initial_overlap
                                 : 0U,
                    .time_of_impact = hit->time_of_impact,
                    .penetration_depth = hit->penetration_depth,
                    .position = toAbi(hit->position),
                    .normal = toAbi(hit->normal),
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

const Physics::ProviderV2 &builtinProviderV2() noexcept {
    static const Physics::ProviderV2 provider = [] {
        auto value = Physics::descriptor<Physics::ProviderV2>();
        value.capability_bits = Physics::builtinQueryCapabilitiesV2;
        value.name_utf8 = "pelican.builtin";
        value.name_size = 15;
        value.raycast_all = raycastAll;
        value.overlap_all = overlapAll;
        value.shape_cast_all = shapeCastAll;
        return value;
    }();
    return provider;
}

} // namespace Pelican::physics_internal
