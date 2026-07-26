#include "physicsruntime.hpp"

#include "physworld.hpp"
#include "../container.hpp"
#include "../userpublic/physics/abi_v2.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace Pelican::Physics {
namespace {

int service_context_token = 0;

template <class T>
Status validateDescriptor(const T &descriptor_value) {
    if (descriptor_value.struct_size < sizeof(T)) return Status::invalid_argument;
    if (descriptor_value.version != descriptorVersionV1) return Status::unsupported_version;
    if (descriptor_value.reserved0 != 0 || descriptor_value.reserved1 != 0)
        return Status::reserved_not_zero;
    return Status::ok;
}

bool finite(Vec3V1 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite(QuatV1 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}

vec3 fromAbi(Vec3V1 value) {
    return {value.x, value.y, value.z};
}

phys::Shape fromAbi(const ShapeV1 &shape) {
    if (shape.reserved != 0 || !finite(shape.center)) {
        throw std::invalid_argument("invalid physics shape descriptor");
    }
    if (shape.kind != ShapeKindV1::provider &&
        (shape.provider_id != 0 || shape.provider_shape != 0)) {
        throw std::invalid_argument("standard physics shape has provider-owned fields");
    }
    const vec3 center = fromAbi(shape.center);
    switch (shape.kind) {
    case ShapeKindV1::sphere:
        if (!std::isfinite(shape.radius) || shape.radius < 0.0F)
            throw std::invalid_argument("invalid sphere radius");
        return phys::Sphere{center, shape.radius};
    case ShapeKindV1::box:
        if (!finite(shape.rotation) || !finite(shape.half_extents) ||
            shape.half_extents.x < 0.0F || shape.half_extents.y < 0.0F ||
            shape.half_extents.z < 0.0F)
            throw std::invalid_argument("invalid box shape");
        return phys::Box{
            center,
            quat{shape.rotation.x, shape.rotation.y, shape.rotation.z, shape.rotation.w},
            fromAbi(shape.half_extents),
        };
    case ShapeKindV1::capsule:
        if (!finite(shape.rotation) || !std::isfinite(shape.half_height) ||
            !std::isfinite(shape.radius) || shape.half_height < 0.0F || shape.radius < 0.0F)
            throw std::invalid_argument("invalid capsule shape");
        return phys::Capsule{
            center,
            quat{shape.rotation.x, shape.rotation.y, shape.rotation.z, shape.rotation.w},
            shape.half_height,
            shape.radius,
        };
    case ShapeKindV1::provider:
        throw std::invalid_argument("custom provider shapes are not available in PhysicsServiceV2");
    }
    throw std::invalid_argument("unknown physics shape kind");
}

struct ConvertedFilter {
    std::vector<phys::ColliderId> ignored_colliders;
    std::vector<GameObjectId> ignored_entities;
    phys::QueryFilter value{};
};

ConvertedFilter fromAbi(const QueryFilterV1 &filter) {
    constexpr std::uint32_t known_flags = query_include_triggers | query_include_one_way;
    if (filter.reserved0 != 0 || filter.reserved1 != 0 ||
        (filter.flags & ~known_flags) != 0 || filter.has_self_entity > 1 ||
        (filter.ignored_collider_count != 0 && filter.ignored_collider_ids == nullptr) ||
        (filter.ignored_entity_count != 0 && filter.ignored_entities == nullptr)) {
        throw std::invalid_argument("invalid physics query filter");
    }

    ConvertedFilter converted;
    converted.ignored_colliders.reserve(filter.ignored_collider_count);
    for (std::uint32_t index = 0; index < filter.ignored_collider_count; ++index) {
        const auto id = filter.ignored_collider_ids[index];
        if (id == 0) throw std::invalid_argument("ignored collider id must be non-zero");
        converted.ignored_colliders.push_back(phys::ColliderId{id});
    }
    converted.ignored_entities.reserve(filter.ignored_entity_count);
    for (std::uint32_t index = 0; index < filter.ignored_entity_count; ++index) {
        converted.ignored_entities.push_back(GameObjectId{
            filter.ignored_entities[index].index,
            filter.ignored_entities[index].generation,
        });
    }

    converted.value.layer = filter.layer;
    converted.value.mask = filter.mask;
    converted.value.include_triggers = (filter.flags & query_include_triggers) != 0;
    converted.value.include_one_way = (filter.flags & query_include_one_way) != 0;
    if (filter.self_collider_id != 0)
        converted.value.self = phys::ColliderId{filter.self_collider_id};
    if (filter.has_self_entity != 0)
        converted.value.self_entity = GameObjectId{filter.self_entity.index,
                                                   filter.self_entity.generation};
    converted.value.ignored = converted.ignored_colliders;
    converted.value.ignored_entities = converted.ignored_entities;
    return converted;
}

Vec3V1 toAbi(vec3 value) {
    return {value.x, value.y, value.z};
}

ColliderIdentityV1 toAbi(const phys::ColliderIdentity &identity) {
    ColliderIdentityV1 result{};
    result.collider_id = identity.collider_id.value;
    result.shape_ordinal = identity.shape_ordinal;
    if (identity.entity) {
        result.entity = EntityIdV1{identity.entity->index, identity.entity->generation};
        result.has_entity = 1;
    }
    return result;
}

ColliderMetadataV1 toAbi(const phys::ColliderQueryMetadata &metadata) {
    return ColliderMetadataV1{
        .layer = metadata.layer,
        .mask = metadata.mask,
        .flags = (metadata.trigger ? collider_trigger : 0U) |
                 (metadata.one_way ? collider_one_way : 0U),
    };
}

Status copyRaycastHits(const std::vector<phys::RaycastQueryHit> &source,
                       RaycastHitV1 *hits, std::uint32_t capacity,
                       std::uint32_t *out_count) {
    if (out_count == nullptr || (capacity != 0 && hits == nullptr))
        return Status::invalid_argument;
    if (source.size() > std::numeric_limits<std::uint32_t>::max())
        return Status::out_of_memory;
    *out_count = static_cast<std::uint32_t>(source.size());
    if (capacity < source.size()) return Status::buffer_too_small;
    for (std::size_t index = 0; index < source.size(); ++index) {
        hits[index] = RaycastHitV1{
            .identity = toAbi(source[index].identity),
            .metadata = toAbi(source[index].metadata),
            .distance = source[index].distance,
            .position = toAbi(source[index].position),
            .normal = toAbi(source[index].normal),
        };
    }
    return Status::ok;
}

Status copyOverlapHits(const std::vector<phys::OverlapHit> &source,
                       OverlapHitV1 *hits, std::uint32_t capacity,
                       std::uint32_t *out_count) {
    if (out_count == nullptr || (capacity != 0 && hits == nullptr))
        return Status::invalid_argument;
    if (source.size() > std::numeric_limits<std::uint32_t>::max())
        return Status::out_of_memory;
    *out_count = static_cast<std::uint32_t>(source.size());
    if (capacity < source.size()) return Status::buffer_too_small;
    for (std::size_t index = 0; index < source.size(); ++index) {
        hits[index] = OverlapHitV1{
            .identity = toAbi(source[index].identity),
            .metadata = toAbi(source[index].metadata),
        };
    }
    return Status::ok;
}

Status copyShapeCastHits(const std::vector<phys::ShapeCastQueryHit> &source,
                         ShapeCastHitV2 *hits, std::uint32_t capacity,
                         std::uint32_t *out_count) {
    if (out_count == nullptr || (capacity != 0 && hits == nullptr))
        return Status::invalid_argument;
    if (source.size() > std::numeric_limits<std::uint32_t>::max())
        return Status::out_of_memory;
    *out_count = static_cast<std::uint32_t>(source.size());
    if (capacity < source.size()) return Status::buffer_too_small;
    for (std::size_t index = 0; index < source.size(); ++index) {
        hits[index] = ShapeCastHitV2{
            .identity = toAbi(source[index].identity),
            .metadata = toAbi(source[index].metadata),
            .time_of_impact = source[index].time_of_impact,
            .penetration_depth = source[index].penetration_depth,
            .position = toAbi(source[index].position),
            .normal = toAbi(source[index].normal),
            .flags = source[index].initial_overlap
                         ? shape_cast_initial_overlap
                         : 0U,
        };
    }
    return Status::ok;
}

Status serviceRaycastAll(void *context, const RaycastQueryV1 *query,
                         RaycastHitV1 *hits, std::uint32_t capacity,
                         std::uint32_t *out_count) noexcept {
    if (context != &service_context_token || query == nullptr || out_count == nullptr ||
        (capacity != 0 && hits == nullptr)) {
        return Status::invalid_argument;
    }
    if (const auto status = validateDescriptor(*query); status != Status::ok) return status;
    if (query->ray.reserved != 0 || !finite(query->ray.origin) ||
        !finite(query->ray.direction) || !std::isfinite(query->ray.max_distance) ||
        query->ray.max_distance < 0.0F) {
        return Status::invalid_argument;
    }
    try {
        const auto converted_filter = fromAbi(query->filter);
        const auto *world = FastModuleContainer::tryGet<PhysWorld>();
        if (world == nullptr) return Status::unavailable;
        const auto colliders = world->collectColliders();
        std::vector<phys::RaycastQueryHit> result;
        const auto status = physics_internal::raycastAll(
            phys::Ray{fromAbi(query->ray.origin), fromAbi(query->ray.direction),
                      query->ray.max_distance},
            colliders, &converted_filter.value, result);
        if (status != Status::ok) return status;
        return copyRaycastHits(result, hits, capacity, out_count);
    } catch (const std::bad_alloc &) {
        return Status::out_of_memory;
    } catch (const std::invalid_argument &) {
        return Status::invalid_argument;
    } catch (...) {
        return Status::provider_error;
    }
}

Status serviceOverlapAll(void *context, const OverlapQueryV1 *query,
                         OverlapHitV1 *hits, std::uint32_t capacity,
                         std::uint32_t *out_count) noexcept {
    if (context != &service_context_token || query == nullptr || out_count == nullptr ||
        (capacity != 0 && hits == nullptr)) {
        return Status::invalid_argument;
    }
    if (const auto status = validateDescriptor(*query); status != Status::ok) return status;
    try {
        const auto converted_filter = fromAbi(query->filter);
        const auto *world = FastModuleContainer::tryGet<PhysWorld>();
        if (world == nullptr) return Status::unavailable;
        const auto colliders = world->collectColliders();
        std::vector<phys::OverlapHit> result;
        const auto status = physics_internal::overlapAll(
            fromAbi(query->shape), colliders, &converted_filter.value, result);
        if (status != Status::ok) return status;
        return copyOverlapHits(result, hits, capacity, out_count);
    } catch (const std::bad_alloc &) {
        return Status::out_of_memory;
    } catch (const std::invalid_argument &) {
        return Status::invalid_argument;
    } catch (...) {
        return Status::provider_error;
    }
}

Status serviceShapeCastAll(void *context, const ShapeCastQueryV2 *query,
                           ShapeCastHitV2 *hits, std::uint32_t capacity,
                           std::uint32_t *out_count) noexcept {
    if (context != &service_context_token || query == nullptr || out_count == nullptr ||
        (capacity != 0 && hits == nullptr)) {
        return Status::invalid_argument;
    }
    if (const auto status = validateDescriptor(*query); status != Status::ok) return status;
    if (query->reserved2 != 0 || !finite(query->delta)) {
        return Status::invalid_argument;
    }
    try {
        const auto converted_filter = fromAbi(query->filter);
        const auto *world = FastModuleContainer::tryGet<PhysWorld>();
        if (world == nullptr) return Status::unavailable;
        const auto colliders = world->collectColliders();
        std::vector<phys::ShapeCastQueryHit> result;
        const auto status = physics_internal::shapeCastAll(
            fromAbi(query->shape), fromAbi(query->delta), colliders,
            &converted_filter.value, result);
        if (status != Status::ok) return status;
        return copyShapeCastHits(result, hits, capacity, out_count);
    } catch (const std::bad_alloc &) {
        return Status::out_of_memory;
    } catch (const std::invalid_argument &) {
        return Status::invalid_argument;
    } catch (...) {
        return Status::provider_error;
    }
}

Status getServiceV2(void *context, std::uint32_t client_version,
                    ServiceV2 *out) noexcept {
    if (context != &service_context_token || out == nullptr ||
        out->struct_size < descriptorHeaderSize) {
        return Status::invalid_argument;
    }
    if (out->version != descriptorVersionV1) return Status::unsupported_version;
    if (out->reserved0 != 0 || out->reserved1 != 0) return Status::reserved_not_zero;
    if (client_version != serviceVersionV2) return Status::unsupported_version;

    const auto caller_size = out->struct_size;
    auto produced = descriptor<ServiceV2>();
    produced.service_version = serviceVersionV2;
    produced.minimum_client_service_version = serviceVersionV2;
    produced.capability_bits = builtinQueryCapabilitiesV2;
    produced.context = &service_context_token;
    produced.raycast_all = serviceRaycastAll;
    produced.overlap_all = serviceOverlapAll;
    produced.shape_cast_all = serviceShapeCastAll;
    std::memcpy(out, &produced, std::min<std::size_t>(caller_size, sizeof(produced)));
    return Status::ok;
}

Status registerProviderV2(void *context, const ProviderV2 *provider,
                          ProviderHandleV2 *out_handle) noexcept {
    if (context != &service_context_token || provider == nullptr || out_handle == nullptr)
        return Status::invalid_argument;
    const auto owner = internal::currentRegistrationOwner();
    if (owner == internal::engineRegistrationOwner) return Status::wrong_owner;
    return physics_internal::registerProvider(*provider, owner, *out_handle);
}

Status unregisterProvider(void *context, ProviderHandleV2 handle) noexcept {
    if (context != &service_context_token) return Status::invalid_argument;
    const auto owner = internal::currentRegistrationOwner();
    if (owner == internal::engineRegistrationOwner) return Status::wrong_owner;
    return physics_internal::unregisterProvider(handle, owner);
}

} // namespace

Status getApiV2(std::uint32_t client_abi_version, ApiV2 *out_api) noexcept {
    if (out_api == nullptr || out_api->struct_size < descriptorHeaderSize)
        return Status::invalid_argument;
    if (out_api->version != descriptorVersionV1) return Status::unsupported_version;
    if (out_api->reserved0 != 0 || out_api->reserved1 != 0)
        return Status::reserved_not_zero;
    if (client_abi_version != abiVersionV2) return Status::unsupported_version;

    const auto caller_size = out_api->struct_size;
    auto produced = descriptor<ApiV2>();
    produced.engine_abi_version = abiVersionV2;
    produced.minimum_client_abi_version = abiVersionV2;
    produced.capability_bits = api_query_service | api_provider_registration;
    produced.context = &service_context_token;
    produced.get_service = getServiceV2;
    produced.register_provider = registerProviderV2;
    produced.unregister_provider = unregisterProvider;
    std::memcpy(out_api, &produced, std::min<std::size_t>(caller_size, sizeof(produced)));
    return Status::ok;
}

} // namespace Pelican::Physics
