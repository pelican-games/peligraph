#include "physicsruntime.hpp"

#include "physqueryinternal.hpp"
#if PELICAN_WITH_BUILTIN_PHYSICS || PELICAN_WITH_JOLT_PHYSICS
#include "meshphysicsprovider.hpp"
#endif
#if PELICAN_WITH_BUILTIN_PHYSICS
#include "builtinphysicsprovider.hpp"
#endif
#if PELICAN_WITH_JOLT_PHYSICS
#include "joltphysicsprovider.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace Pelican::physics_internal {
namespace {

struct ProviderDispatch {
    std::uint64_t capability_bits = 0;
    void *context = nullptr;
    Physics::ProviderRaycastAllV1Fn raycast_all = nullptr;
    Physics::ProviderOverlapAllV1Fn overlap_all = nullptr;
    Physics::ProviderShapeCastAllV2Fn shape_cast_all = nullptr;
    std::uint64_t mesh_capability_bits = 0;
    Physics::ProviderMeshRaycastAllV3Fn mesh_raycast_all = nullptr;
    Physics::ProviderMeshSphereOverlapAllV3Fn mesh_sphere_overlap_all = nullptr;
    Physics::ProviderMeshBoxOverlapAllV3Fn mesh_box_overlap_all = nullptr;
    Physics::ProviderMeshCapsuleOverlapAllV3Fn mesh_capsule_overlap_all = nullptr;
    Physics::ProviderMeshSphereShapeCastAllV3Fn mesh_sphere_shape_cast_all = nullptr;
    Physics::ProviderMeshBoxShapeCastAllV3Fn mesh_box_shape_cast_all = nullptr;
    Physics::ProviderMeshCapsuleShapeCastAllV3Fn mesh_capsule_shape_cast_all = nullptr;
};

struct RegisteredProvider {
    Physics::ProviderHandleV2 handle{};
    internal::RegistrationOwner owner = internal::engineRegistrationOwner;
    ProviderDispatch dispatch{};
    std::string name;
};

struct Registry {
    mutable std::shared_mutex mutex;
    std::vector<RegisteredProvider> providers;
    std::uint64_t active_game_provider = 0;
    std::uint64_t next_identity = 1;
};

Registry &registry() {
    static auto *value = new Registry();
    return *value;
}

bool validHandle(Physics::ProviderHandleV2 handle) {
    return handle.identity != 0 && handle.generation != 0 && handle.reserved == 0;
}

const RegisteredProvider *findByIdentity(const Registry &value, std::uint64_t identity) {
    const auto found = std::find_if(value.providers.begin(), value.providers.end(),
                                    [identity](const RegisteredProvider &provider) {
                                        return provider.handle.identity == identity;
                                    });
    return found == value.providers.end() ? nullptr : &*found;
}

const RegisteredProvider *findByOwner(const Registry &value,
                                      internal::RegistrationOwner owner) {
    const auto found = std::find_if(value.providers.begin(), value.providers.end(),
                                    [owner](const RegisteredProvider &provider) {
                                        return provider.owner == owner;
                                    });
    return found == value.providers.end() ? nullptr : &*found;
}

const RegisteredProvider *activeGameProviderLocked(const Registry &value) {
    if (value.active_game_provider != 0) {
        if (const auto *provider = findByIdentity(value, value.active_game_provider)) {
            return provider;
        }
    }
    return nullptr;
}

ProviderDispatch dispatchFor(const Physics::ProviderV2 &provider) {
    return ProviderDispatch{
        provider.capability_bits,
        provider.context,
        provider.raycast_all,
        provider.overlap_all,
        provider.shape_cast_all,
    };
}

const ProviderDispatch *configuredProviderLocked(const Registry &value,
                                                  std::string_view *name = nullptr) {
    if (const auto *provider = findByOwner(value, internal::engineRegistrationOwner);
        provider != nullptr && provider->dispatch.capability_bits != 0) {
        if (name != nullptr) *name = provider->name;
        return &provider->dispatch;
    }
#if PELICAN_WITH_JOLT_PHYSICS
    static const auto dispatch = dispatchFor(joltProviderV2());
    if (name != nullptr) *name = "pelican.jolt";
    return &dispatch;
#elif PELICAN_WITH_BUILTIN_PHYSICS
    static const auto dispatch = dispatchFor(builtinProviderV2());
    if (name != nullptr) *name = "pelican.builtin";
    return &dispatch;
#else
    if (name != nullptr) *name = {};
    return nullptr;
#endif
}

const ProviderDispatch *providerForCapabilityLocked(const Registry &value,
                                                     std::uint64_t capability) {
    if (const auto *game_provider = activeGameProviderLocked(value);
        game_provider != nullptr &&
        (game_provider->dispatch.capability_bits & capability) != 0) {
        return &game_provider->dispatch;
    }
    const auto *configured = configuredProviderLocked(value);
    if (configured == nullptr || (configured->capability_bits & capability) == 0) {
        return nullptr;
    }
    return configured;
}

bool finite(Physics::Vec3V1 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite(Physics::QuatV1 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}

bool normalized(Physics::Vec3V1 value, Physics::Vec3V1 &out) {
    const double length = std::hypot(static_cast<double>(value.x),
                                     static_cast<double>(value.y),
                                     static_cast<double>(value.z));
    if (!std::isfinite(length) || length <= 1.0e-5) return false;
    out = Physics::Vec3V1{
        static_cast<float>(static_cast<double>(value.x) / length),
        static_cast<float>(static_cast<double>(value.y) / length),
        static_cast<float>(static_cast<double>(value.z) / length),
    };
    return true;
}

bool knownStatus(Physics::Status status) {
    return status >= Physics::Status::ok && status <= Physics::Status::out_of_memory;
}

Physics::Vec3V1 toAbi(vec3 value) {
    return {value.x, value.y, value.z};
}

vec3 fromAbi(Physics::Vec3V1 value) {
    return {value.x, value.y, value.z};
}

Physics::QuatV1 canonicalRotation(quat value) {
    const Physics::QuatV1 input{value.x, value.y, value.z, value.w};
    if (!finite(input)) throw std::invalid_argument("non-finite physics rotation");
    const double length = std::hypot(
        std::hypot(static_cast<double>(input.x), static_cast<double>(input.y)),
        std::hypot(static_cast<double>(input.z), static_cast<double>(input.w)));
    if (!std::isfinite(length)) throw std::invalid_argument("invalid physics rotation");
    if (length <= 1.0e-5) return {};
    return {
        static_cast<float>(static_cast<double>(input.x) / length),
        static_cast<float>(static_cast<double>(input.y) / length),
        static_cast<float>(static_cast<double>(input.z) / length),
        static_cast<float>(static_cast<double>(input.w) / length),
    };
}

Physics::ShapeV1 toAbi(const phys::Shape &shape) {
    return std::visit([](const auto &typed_shape) {
        using ShapeType = std::remove_cvref_t<decltype(typed_shape)>;
        Physics::ShapeV1 result{};
        result.center = toAbi(typed_shape.center);
        if (!finite(result.center)) throw std::invalid_argument("non-finite physics shape center");
        if constexpr (std::is_same_v<ShapeType, phys::Sphere>) {
            if (!std::isfinite(typed_shape.radius) || typed_shape.radius < 0.0F)
                throw std::invalid_argument("invalid physics sphere");
            result.kind = Physics::ShapeKindV1::sphere;
            result.radius = typed_shape.radius;
        } else if constexpr (std::is_same_v<ShapeType, phys::Box>) {
            result.half_extents = toAbi(typed_shape.half_extents);
            if (!finite(result.half_extents) || result.half_extents.x < 0.0F ||
                result.half_extents.y < 0.0F || result.half_extents.z < 0.0F) {
                throw std::invalid_argument("invalid physics box");
            }
            result.kind = Physics::ShapeKindV1::box;
            result.rotation = canonicalRotation(typed_shape.rotation);
        } else {
            if (!std::isfinite(typed_shape.half_height) ||
                !std::isfinite(typed_shape.radius) || typed_shape.half_height < 0.0F ||
                typed_shape.radius < 0.0F) {
                throw std::invalid_argument("invalid physics capsule");
            }
            result.kind = Physics::ShapeKindV1::capsule;
            result.rotation = canonicalRotation(typed_shape.rotation);
            result.half_height = typed_shape.half_height;
            result.radius = typed_shape.radius;
        }
        return result;
    }, shape);
}

struct SelectedCollider {
    const phys::Collider *collider = nullptr;
    phys::ColliderIdentity identity{};
};

std::vector<SelectedCollider> selectColliders(std::span<const phys::Collider> colliders,
                                              const phys::QueryFilter *filter,
                                              std::vector<Physics::ShapeV1> &out_shapes) {
    std::vector<SelectedCollider> selected;
    selected.reserve(colliders.size());
    out_shapes.reserve(colliders.size());
    for (const auto &collider : colliders) {
        auto identity = phys::effectiveColliderIdentity(collider);
        if (filter != nullptr &&
            !phys::internal::passesQueryFilter(collider, identity, *filter)) {
            continue;
        }
        selected.push_back(SelectedCollider{&collider, std::move(identity)});
        out_shapes.push_back(toAbi(collider.shape));
    }
    return selected;
}

template <class RawHit, class Query, class Callback>
Physics::Status invokeProvider(const ProviderDispatch &provider,
                               std::uint64_t required_capability,
                               Callback callback,
                               const Query &query,
                               std::size_t maximum_hits,
                               std::vector<RawHit> &out_hits) {
    if ((provider.capability_bits & required_capability) == 0 || callback == nullptr) {
        return Physics::Status::unavailable;
    }
    if (maximum_hits > std::numeric_limits<std::uint32_t>::max()) {
        return Physics::Status::out_of_memory;
    }

    out_hits.resize(maximum_hits);
    std::uint32_t hit_count = 0;
    auto status = callback(provider.context, &query,
                           out_hits.empty() ? nullptr : out_hits.data(),
                           static_cast<std::uint32_t>(out_hits.size()), &hit_count);
    if (!knownStatus(status)) {
        out_hits.clear();
        return Physics::Status::provider_error;
    }
    if (status == Physics::Status::buffer_too_small) {
        // V1 has one top-level result per collider. Requiring more indicates a
        // provider contract violation rather than an allocation request.
        if (hit_count > maximum_hits) {
            out_hits.clear();
            return Physics::Status::provider_error;
        }
        status = Physics::Status::provider_error;
    }
    if (status != Physics::Status::ok || hit_count > out_hits.size()) {
        out_hits.clear();
        return status == Physics::Status::ok ? Physics::Status::provider_error : status;
    }
    out_hits.resize(hit_count);
    return Physics::Status::ok;
}

struct SelectedMeshes {
    std::vector<Physics::TriangleMeshV3> meshes;
    std::vector<const phys::TriangleMesh *> acceleration;
    std::vector<std::size_t> source_indices;
    std::vector<phys::ColliderIdentity> identities;
};

Physics::Status selectMeshes(
    std::span<const Physics::TriangleMeshV3> meshes,
    std::span<const phys::ColliderIdentity> identities,
    std::span<const phys::ColliderQueryMetadata> metadata,
    const phys::QueryFilter *filter,
    std::span<const phys::TriangleMesh *const> acceleration, SelectedMeshes &result) {
    using Physics::Status;
    if ((!acceleration.empty() && acceleration.size() != meshes.size()) ||
        meshes.size() != identities.size() || meshes.size() != metadata.size())
        return Status::invalid_argument;
    if (meshes.size() > std::numeric_limits<std::uint32_t>::max())
        return Status::out_of_memory;
    for (std::size_t index = 0; index < meshes.size(); ++index) {
        phys::Collider identity_proxy;
        identity_proxy.id = identities[index].name;
        identity_proxy.identity = identities[index];
        identity_proxy.metadata = metadata[index];
        auto identity = phys::effectiveColliderIdentity(identity_proxy);
        if (filter != nullptr &&
            !phys::internal::passesQueryFilter(identity_proxy, identity, *filter)) continue;
        const auto &mesh = meshes[index];
        if (mesh.reserved0 != 0 || mesh.reserved1 != 0 || mesh.index_count % 3 != 0 ||
            (mesh.vertex_count != 0 && mesh.vertices == nullptr) ||
            (mesh.index_count != 0 && mesh.indices == nullptr)) return Status::invalid_argument;
        const auto *cached = acceleration.empty() ? nullptr : acceleration[index];
        // Prepared engine geometry was validated before publication.
        if (cached == nullptr) {
            for (std::uint32_t vertex = 0; vertex < mesh.vertex_count; ++vertex)
                if (!finite(mesh.vertices[vertex])) return Status::invalid_argument;
            for (std::uint32_t corner = 0; corner < mesh.index_count; ++corner)
                if (mesh.indices[corner] >= mesh.vertex_count) return Status::invalid_argument;
        }
        result.acceleration.push_back(cached);
        for (const auto value : mesh.local_to_world)
            if (!std::isfinite(value)) return Status::invalid_argument;
        const auto *m = mesh.local_to_world;
        if (m[3] != 0.0F || m[7] != 0.0F || m[11] != 0.0F || m[15] != 1.0F)
            return Status::invalid_argument;
        const double determinant =
            static_cast<double>(m[0]) * (static_cast<double>(m[5]) * m[10] - static_cast<double>(m[9]) * m[6]) -
            static_cast<double>(m[4]) * (static_cast<double>(m[1]) * m[10] - static_cast<double>(m[9]) * m[2]) +
            static_cast<double>(m[8]) * (static_cast<double>(m[1]) * m[6] - static_cast<double>(m[5]) * m[2]);
        if (determinant == 0.0 || !std::isfinite(determinant)) return Status::invalid_argument;
        result.meshes.push_back(mesh);
        result.source_indices.push_back(index);
        result.identities.push_back(std::move(identity));
    }
    return Status::ok;
}

template<class Query, class RawHit, class Callback>
Physics::Status invokeMeshProvider(
    std::uint64_t capability, Callback ProviderDispatch::*callback,
    const Query &query, const SelectedMeshes &selected, std::vector<RawHit> &hits) {
    auto &value = registry();
    std::shared_lock lock{value.mutex};
    const ProviderDispatch *dispatch = nullptr;
    if (const auto *game = activeGameProviderLocked(value);
        game != nullptr && (game->dispatch.mesh_capability_bits & capability) != 0)
        dispatch = &game->dispatch;
    if (dispatch == nullptr) {
        if (const auto *engine = findByOwner(value, internal::engineRegistrationOwner);
            engine != nullptr && (engine->dispatch.mesh_capability_bits & capability) != 0)
            dispatch = &engine->dispatch;
    }
#if PELICAN_WITH_BUILTIN_PHYSICS || PELICAN_WITH_JOLT_PHYSICS
    BuiltinMeshQueryContext mesh_context{selected.acceleration};
    const ProviderDispatch builtin_mesh{
        .context = &mesh_context,
        .mesh_capability_bits = Physics::query_mesh_raycast_all | Physics::query_mesh_sphere_overlap_all | Physics::query_mesh_box_overlap_all | Physics::query_mesh_capsule_overlap_all | Physics::query_mesh_sphere_shape_cast_all | Physics::query_mesh_box_shape_cast_all | Physics::query_mesh_capsule_shape_cast_all,
        .mesh_raycast_all = builtinMeshRaycastAll,
        .mesh_sphere_overlap_all = builtinMeshSphereOverlapAll,
        .mesh_box_overlap_all = builtinMeshBoxOverlapAll,
        .mesh_capsule_overlap_all = builtinMeshCapsuleOverlapAll,
        .mesh_sphere_shape_cast_all = builtinMeshSphereShapeCastAll,
        .mesh_box_shape_cast_all = builtinMeshBoxShapeCastAll,
        .mesh_capsule_shape_cast_all = builtinMeshCapsuleShapeCastAll,
    };
    if (dispatch == nullptr) dispatch = &builtin_mesh;
#endif
    if (dispatch == nullptr) return Physics::Status::unavailable;
    // Mesh capabilities occupy a separate domain from primitive capabilities.
    auto invocation = *dispatch;
    invocation.capability_bits = dispatch->mesh_capability_bits;
    return invokeProvider(invocation, capability, dispatch->*callback, query, selected.meshes.size(), hits);
}

bool validProvider(const Physics::ProviderV2 &provider, bool allow_empty = false) {
    constexpr auto known_capabilities = Physics::builtinQueryCapabilitiesV2;
    if (provider.struct_size < sizeof(Physics::ProviderV2) ||
        provider.version != Physics::descriptorVersionV1 ||
        provider.reserved0 != 0 || provider.reserved1 != 0 || provider.reserved2 != 0 ||
        provider.provider_version != Physics::providerVersionV2 ||
        provider.minimum_engine_provider_version > Physics::providerVersionV2 ||
        (provider.capability_bits & ~known_capabilities) != 0 ||
        (!allow_empty && provider.capability_bits == 0) || provider.name_utf8 == nullptr ||
        provider.name_size == 0 ||
        provider.name_size > Physics::maximumProviderNameBytesV1) {
        return false;
    }
    if ((provider.capability_bits & Physics::query_raycast_all) != 0 &&
        provider.raycast_all == nullptr) {
        return false;
    }
    if ((provider.capability_bits & Physics::query_overlap_all) != 0 &&
        provider.overlap_all == nullptr) {
        return false;
    }
    if ((provider.capability_bits & Physics::query_shape_cast_all) != 0 &&
        provider.shape_cast_all == nullptr) {
        return false;
    }
    return true;
}

Physics::Status registerProviderDispatch(ProviderDispatch dispatch,
                                         const char *name_utf8,
                                         std::uint32_t name_size,
                                         internal::RegistrationOwner owner,
                                         Physics::ProviderHandleV2 &out_handle) {
    auto &value = registry();
    std::unique_lock lock{value.mutex};
    if (findByOwner(value, owner) != nullptr) {
        return Physics::Status::duplicate_provider;
    }
    if (value.next_identity == 0) {
        return Physics::Status::out_of_memory;
    }
    RegisteredProvider registered;
    registered.handle = Physics::ProviderHandleV2{value.next_identity++, 1, 0};
    registered.owner = owner;
    registered.dispatch = dispatch;
    registered.name.assign(name_utf8, name_size);
    value.providers.push_back(std::move(registered));
    out_handle = value.providers.back().handle;
    return Physics::Status::ok;
}

bool validShapeCastHit(const Physics::ProviderShapeCastHitV2 &raw,
                       Physics::Vec3V1 &unit_normal) {
    constexpr float epsilon = Physics::shapeCastContactEpsilonV2;
    const bool initial = (raw.flags & Physics::shape_cast_initial_overlap) != 0;
    return raw.reserved == 0 && (raw.flags & ~Physics::shape_cast_initial_overlap) == 0 &&
        std::isfinite(raw.time_of_impact) && raw.time_of_impact >= 0 &&
        raw.time_of_impact <= 1.0F + epsilon &&
        std::isfinite(raw.penetration_depth) && raw.penetration_depth >= 0 &&
        (initial ? (raw.time_of_impact <= epsilon && raw.penetration_depth > epsilon)
                 : raw.penetration_depth <= epsilon) &&
        finite(raw.position) && normalized(raw.normal, unit_normal);
}

} // namespace

Physics::Status registerProvider(const Physics::ProviderV2 &provider,
                                 internal::RegistrationOwner owner,
                                 Physics::ProviderHandleV2 &out_handle) noexcept {
    out_handle = {};
    if (!validProvider(provider)) {
        return Physics::Status::invalid_argument;
    }
    try {
        return registerProviderDispatch(dispatchFor(provider), provider.name_utf8,
                                        provider.name_size, owner, out_handle);
    } catch (const std::bad_alloc &) {
        return Physics::Status::out_of_memory;
    } catch (...) {
        return Physics::Status::provider_error;
    }
}

Physics::Status registerProvider(const Physics::ProviderV3 &provider,
                                 internal::RegistrationOwner owner,
                                 Physics::ProviderHandleV2 &out_handle) noexcept {
    out_handle = {};
    if (provider.struct_size < sizeof(Physics::ProviderV3) ||
        provider.version != Physics::descriptorVersionV1 ||
        provider.reserved0 != 0 || provider.reserved1 != 0 ||
        provider.provider_version != Physics::providerVersionV3 ||
        provider.minimum_engine_provider_version > Physics::providerVersionV3 ||
        !validProvider(provider.base, true) ||
        (provider.mesh_capability_bits & ~(Physics::query_mesh_raycast_all | Physics::query_mesh_sphere_overlap_all | Physics::query_mesh_box_overlap_all | Physics::query_mesh_capsule_overlap_all | Physics::query_mesh_sphere_shape_cast_all | Physics::query_mesh_box_shape_cast_all | Physics::query_mesh_capsule_shape_cast_all)) != 0 ||
        (provider.mesh_capability_bits == 0 && provider.base.capability_bits == 0) ||
        ((provider.mesh_capability_bits & Physics::query_mesh_raycast_all) != 0 && provider.mesh_raycast_all == nullptr) ||
        ((provider.mesh_capability_bits & Physics::query_mesh_sphere_overlap_all) != 0 && provider.mesh_sphere_overlap_all == nullptr) ||
        ((provider.mesh_capability_bits & Physics::query_mesh_box_overlap_all) != 0 && provider.mesh_box_overlap_all == nullptr) ||
        ((provider.mesh_capability_bits & Physics::query_mesh_capsule_overlap_all) != 0 && provider.mesh_capsule_overlap_all == nullptr) ||
        ((provider.mesh_capability_bits & Physics::query_mesh_sphere_shape_cast_all) != 0 && provider.mesh_sphere_shape_cast_all == nullptr) ||
        ((provider.mesh_capability_bits & Physics::query_mesh_box_shape_cast_all) != 0 && provider.mesh_box_shape_cast_all == nullptr) ||
        ((provider.mesh_capability_bits & Physics::query_mesh_capsule_shape_cast_all) != 0 && provider.mesh_capsule_shape_cast_all == nullptr))
        return Physics::Status::invalid_argument;
    try {
        auto dispatch = dispatchFor(provider.base);
        dispatch.mesh_capability_bits = provider.mesh_capability_bits;
        dispatch.mesh_raycast_all = provider.mesh_raycast_all;
        dispatch.mesh_sphere_overlap_all = provider.mesh_sphere_overlap_all;
        dispatch.mesh_box_overlap_all = provider.mesh_box_overlap_all;
        dispatch.mesh_capsule_overlap_all = provider.mesh_capsule_overlap_all;
        dispatch.mesh_sphere_shape_cast_all = provider.mesh_sphere_shape_cast_all;
        dispatch.mesh_box_shape_cast_all = provider.mesh_box_shape_cast_all;
        dispatch.mesh_capsule_shape_cast_all = provider.mesh_capsule_shape_cast_all;
        return registerProviderDispatch(dispatch, provider.base.name_utf8,
                                         provider.base.name_size, owner, out_handle);
    } catch (const std::bad_alloc &) {
        return Physics::Status::out_of_memory;
    } catch (...) {
        return Physics::Status::provider_error;
    }
}

Physics::Status unregisterProvider(Physics::ProviderHandleV2 handle,
                                   internal::RegistrationOwner owner) noexcept {
    if (!validHandle(handle)) {
        return Physics::Status::invalid_argument;
    }
    auto &value = registry();
    std::unique_lock lock{value.mutex};
    const auto found = std::find_if(value.providers.begin(), value.providers.end(),
                                    [handle](const RegisteredProvider &provider) {
                                        return provider.handle.identity == handle.identity &&
                                               provider.handle.generation == handle.generation;
                                    });
    if (found == value.providers.end()) {
        return Physics::Status::stale_provider;
    }
    if (found->owner != owner) {
        return Physics::Status::wrong_owner;
    }
    if (value.active_game_provider == found->handle.identity) {
        value.active_game_provider = 0;
    }
    value.providers.erase(found);
    return Physics::Status::ok;
}

void activateProviderOwner(internal::RegistrationOwner owner) noexcept {
    auto &value = registry();
    std::unique_lock lock{value.mutex};
    const auto *provider = findByOwner(value, owner);
    value.active_game_provider = provider == nullptr ? 0 : provider->handle.identity;
}

void releaseProviderOwner(internal::RegistrationOwner owner) noexcept {
    auto &value = registry();
    std::unique_lock lock{value.mutex};
    std::erase_if(value.providers, [&](const RegisteredProvider &provider) {
        if (provider.owner != owner) return false;
        if (value.active_game_provider == provider.handle.identity) {
            value.active_game_provider = 0;
        }
        return true;
    });
}

std::string activeProviderName() {
    auto &value = registry();
    std::shared_lock lock{value.mutex};
    if (const auto *game_provider = activeGameProviderLocked(value)) {
        return game_provider->name;
    }
    std::string_view name;
    (void)configuredProviderLocked(value, &name);
    return std::string{name};
}

Physics::Status raycastMeshes(
    const phys::Ray &ray, std::span<const Physics::TriangleMeshV3> meshes,
    std::span<const phys::ColliderIdentity> identities,
    std::span<const phys::ColliderQueryMetadata> metadata,
    const phys::QueryFilter *filter,
    std::vector<phys::RaycastQueryHit> &out_hits,
    std::span<const phys::TriangleMesh *const> acceleration) noexcept {
    using Physics::Status;
    out_hits.clear();
    try {
        if ((!acceleration.empty() && acceleration.size() != meshes.size()) ||
            meshes.size() != identities.size() || meshes.size() != metadata.size() ||
            !finite(toAbi(ray.origin)) || !finite(toAbi(ray.direction)) ||
            !std::isfinite(ray.max_distance) || ray.max_distance < 0.0F)
            return Status::invalid_argument;
        if (meshes.size() > std::numeric_limits<std::uint32_t>::max())
            return Status::out_of_memory;
        Physics::Vec3V1 direction{};
        if (!normalized(toAbi(ray.direction), direction)) return Status::ok;
        SelectedMeshes selection;
        const auto selected_status = selectMeshes(meshes, identities, metadata, filter, acceleration, selection);
        if (selected_status != Status::ok) return selected_status;
        const auto &selected = selection.meshes;
        const Physics::ProviderMeshRaycastQueryV3 query{
            .ray = Physics::RayV1{toAbi(ray.origin), direction, ray.max_distance, 0},
            .colliders = selected.empty() ? nullptr : selected.data(),
            .collider_count = static_cast<std::uint32_t>(selected.size()),
        };
        std::vector<Physics::ProviderRaycastHitV1> raw_hits;
        const auto status = invokeMeshProvider(Physics::query_mesh_raycast_all,
            &ProviderDispatch::mesh_raycast_all, query, selection, raw_hits);
        if (status != Status::ok) return status;
        std::vector<bool> seen(selected.size(), false);
        for (const auto &raw : raw_hits) {
            Physics::Vec3V1 normal{};
            if (raw.reserved != 0 || raw.collider_index >= selected.size() ||
                seen[raw.collider_index] || !std::isfinite(raw.distance) ||
                raw.distance < 0.0F || raw.distance > ray.max_distance + 1.0e-5F ||
                !normalized(raw.normal, normal)) {
                out_hits.clear();
                return Status::provider_error;
            }
            const Physics::Vec3V1 position{
                ray.origin.x + direction.x * raw.distance,
                ray.origin.y + direction.y * raw.distance,
                ray.origin.z + direction.z * raw.distance};
            if (!finite(position)) {
                out_hits.clear();
                return Status::provider_error;
            }
            seen[raw.collider_index] = true;
            const auto &identity = selection.identities[raw.collider_index];
            out_hits.push_back(phys::RaycastQueryHit{
                identity.name, raw.distance, fromAbi(position), fromAbi(normal),
                identity, metadata[selection.source_indices[raw.collider_index]]});
        }
        phys::internal::orderRaycastHits(out_hits);
        return Status::ok;
    } catch (const std::invalid_argument &) {
        out_hits.clear();
        return Status::invalid_argument;
    } catch (const std::bad_alloc &) {
        out_hits.clear();
        return Status::out_of_memory;
    } catch (...) {
        out_hits.clear();
        return Status::provider_error;
    }
}

Physics::Status overlapMeshes(
    const phys::Shape &shape, std::span<const Physics::TriangleMeshV3> meshes,
    std::span<const phys::ColliderIdentity> identities,
    std::span<const phys::ColliderQueryMetadata> metadata,
    const phys::QueryFilter *filter, std::vector<phys::OverlapHit> &out_hits,
    std::span<const phys::TriangleMesh *const> acceleration) noexcept {
    using Physics::Status;
    out_hits.clear();
    try {
        const auto query_shape = toAbi(shape);
        SelectedMeshes selected;
        const auto selected_status = selectMeshes(meshes, identities, metadata, filter, acceleration, selected);
        if (selected_status != Status::ok) return selected_status;
        std::vector<Physics::ProviderOverlapHitV1> raw_hits;
        Status status;
        if (query_shape.kind == Physics::ShapeKindV1::sphere) {
            const Physics::ProviderMeshSphereOverlapQueryV3 query{
                .center = query_shape.center, .radius = query_shape.radius,
                .colliders = selected.meshes.data(),
                .collider_count = static_cast<std::uint32_t>(selected.meshes.size()),
            };
            status = invokeMeshProvider(Physics::query_mesh_sphere_overlap_all,
                &ProviderDispatch::mesh_sphere_overlap_all, query, selected, raw_hits);
        } else if (query_shape.kind == Physics::ShapeKindV1::box) {
            const Physics::ProviderMeshBoxOverlapQueryV3 query{
                .center = query_shape.center, .rotation = query_shape.rotation,
                .half_extents = query_shape.half_extents,
                .colliders = selected.meshes.data(),
                .collider_count = static_cast<std::uint32_t>(selected.meshes.size()),
            };
            status = invokeMeshProvider(Physics::query_mesh_box_overlap_all,
                &ProviderDispatch::mesh_box_overlap_all, query, selected, raw_hits);
        } else {
            const Physics::ProviderMeshCapsuleOverlapQueryV3 query{
                .center = query_shape.center, .rotation = query_shape.rotation,
                .half_height = query_shape.half_height, .radius = query_shape.radius,
                .colliders = selected.meshes.data(),
                .collider_count = static_cast<std::uint32_t>(selected.meshes.size()),
            };
            status = invokeMeshProvider(Physics::query_mesh_capsule_overlap_all,
                &ProviderDispatch::mesh_capsule_overlap_all, query, selected, raw_hits);
        }
        if (status != Status::ok) return status;
        std::vector<bool> seen(selected.meshes.size(), false);
        out_hits.reserve(raw_hits.size());
        for (const auto &raw : raw_hits) {
            if (raw.reserved != 0 || raw.collider_index >= selected.meshes.size() || seen[raw.collider_index]) {
                out_hits.clear();
                return Status::provider_error;
            }
            seen[raw.collider_index] = true;
            const auto &identity = selected.identities[raw.collider_index];
            out_hits.push_back(phys::OverlapHit{
                identity.name, identity, metadata[selected.source_indices[raw.collider_index]]});
        }
        phys::internal::orderOverlapHits(out_hits);
        return Status::ok;
    } catch (const std::invalid_argument &) {
        out_hits.clear();
        return Status::invalid_argument;
    } catch (const std::bad_alloc &) {
        out_hits.clear();
        return Status::out_of_memory;
    } catch (...) {
        out_hits.clear();
        return Status::provider_error;
    }
}

Physics::Status raycastAll(const phys::Ray &ray,
                           std::span<const phys::Collider> colliders,
                           const phys::QueryFilter *filter,
                           std::vector<phys::RaycastQueryHit> &out_hits) noexcept {
    out_hits.clear();
    try {
        const auto origin = toAbi(ray.origin);
        const auto direction = toAbi(ray.direction);
        if (!finite(origin) || !finite(direction) ||
            !std::isfinite(ray.max_distance) || ray.max_distance < 0.0F) {
            return Physics::Status::invalid_argument;
        }
        Physics::Vec3V1 unit_direction{};
        if (!normalized(direction, unit_direction)) return Physics::Status::ok;
        if (colliders.size() > std::numeric_limits<std::uint32_t>::max()) {
            return Physics::Status::out_of_memory;
        }
        std::vector<Physics::ShapeV1> shapes;
        const auto selected = selectColliders(colliders, filter, shapes);
        const Physics::ProviderRaycastQueryV1 query{
            .ray = Physics::RayV1{
                .origin = origin,
                .direction = unit_direction,
                .max_distance = ray.max_distance,
            },
            .colliders = shapes.empty() ? nullptr : shapes.data(),
            .collider_count = static_cast<std::uint32_t>(shapes.size()),
        };

        std::vector<Physics::ProviderRaycastHitV1> raw_hits;
        auto &value = registry();
        {
            std::shared_lock lock{value.mutex};
            const auto *provider = providerForCapabilityLocked(
                value, Physics::query_raycast_all);
            if (provider == nullptr) return Physics::Status::unavailable;
            const auto status = invokeProvider(*provider, Physics::query_raycast_all,
                                               provider->raycast_all, query,
                                               selected.size(), raw_hits);
            if (status != Physics::Status::ok) return status;
        }

        std::vector<bool> seen(selected.size(), false);
        out_hits.reserve(raw_hits.size());
        for (const auto &raw : raw_hits) {
            Physics::Vec3V1 unit_normal{};
            if (raw.reserved != 0 || raw.collider_index >= selected.size() ||
                seen[raw.collider_index] || !std::isfinite(raw.distance) ||
                raw.distance < 0.0F || raw.distance > ray.max_distance + 1.0e-5F ||
                !normalized(raw.normal, unit_normal)) {
                out_hits.clear();
                return Physics::Status::provider_error;
            }
            const Physics::Vec3V1 canonical_position{
                origin.x + unit_direction.x * raw.distance,
                origin.y + unit_direction.y * raw.distance,
                origin.z + unit_direction.z * raw.distance,
            };
            if (!finite(canonical_position)) {
                out_hits.clear();
                return Physics::Status::provider_error;
            }
            seen[raw.collider_index] = true;
            const auto &selected_collider = selected[raw.collider_index];
            out_hits.push_back(phys::RaycastQueryHit{
                selected_collider.identity.name,
                raw.distance,
                fromAbi(canonical_position),
                fromAbi(unit_normal),
                selected_collider.identity,
                selected_collider.collider->metadata,
            });
        }
        phys::internal::orderRaycastHits(out_hits);
        return Physics::Status::ok;
    } catch (const std::invalid_argument &) {
        out_hits.clear();
        return Physics::Status::invalid_argument;
    } catch (const std::bad_alloc &) {
        out_hits.clear();
        return Physics::Status::out_of_memory;
    } catch (...) {
        out_hits.clear();
        return Physics::Status::provider_error;
    }
}

Physics::Status overlapAll(const phys::Shape &shape,
                           std::span<const phys::Collider> colliders,
                           const phys::QueryFilter *filter,
                           std::vector<phys::OverlapHit> &out_hits) noexcept {
    out_hits.clear();
    try {
        if (colliders.size() > std::numeric_limits<std::uint32_t>::max()) {
            return Physics::Status::out_of_memory;
        }
        std::vector<Physics::ShapeV1> shapes;
        const auto selected = selectColliders(colliders, filter, shapes);
        const Physics::ProviderOverlapQueryV1 query{
            .shape = toAbi(shape),
            .colliders = shapes.empty() ? nullptr : shapes.data(),
            .collider_count = static_cast<std::uint32_t>(shapes.size()),
        };

        std::vector<Physics::ProviderOverlapHitV1> raw_hits;
        auto &value = registry();
        {
            std::shared_lock lock{value.mutex};
            const auto *provider = providerForCapabilityLocked(
                value, Physics::query_overlap_all);
            if (provider == nullptr) return Physics::Status::unavailable;
            const auto status = invokeProvider(*provider, Physics::query_overlap_all,
                                               provider->overlap_all, query,
                                               selected.size(), raw_hits);
            if (status != Physics::Status::ok) return status;
        }

        std::vector<bool> seen(selected.size(), false);
        out_hits.reserve(raw_hits.size());
        for (const auto &raw : raw_hits) {
            if (raw.reserved != 0 || raw.collider_index >= selected.size() ||
                seen[raw.collider_index]) {
                out_hits.clear();
                return Physics::Status::provider_error;
            }
            seen[raw.collider_index] = true;
            const auto &selected_collider = selected[raw.collider_index];
            out_hits.push_back(phys::OverlapHit{
                selected_collider.identity.name,
                selected_collider.identity,
                selected_collider.collider->metadata,
            });
        }
        phys::internal::orderOverlapHits(out_hits);
        return Physics::Status::ok;
    } catch (const std::invalid_argument &) {
        out_hits.clear();
        return Physics::Status::invalid_argument;
    } catch (const std::bad_alloc &) {
        out_hits.clear();
        return Physics::Status::out_of_memory;
    } catch (...) {
        out_hits.clear();
        return Physics::Status::provider_error;
    }
}

Physics::Status shapeCastMeshes(
    const phys::Shape &shape, vec3 delta, std::span<const Physics::TriangleMeshV3> meshes,
    std::span<const phys::ColliderIdentity> identities,
    std::span<const phys::ColliderQueryMetadata> metadata,
    const phys::QueryFilter *filter, std::vector<phys::ShapeCastQueryHit> &out_hits,
    std::span<const phys::TriangleMesh *const> acceleration) noexcept {
    using Physics::Status;
    out_hits.clear();
    try {
        const auto query_shape = toAbi(shape);
        if (!finite(toAbi(delta))) return Status::invalid_argument;
        SelectedMeshes selected;
        const auto selected_status = selectMeshes(meshes, identities, metadata, filter, acceleration, selected);
        if (selected_status != Status::ok) return selected_status;
        if (selected.meshes.empty()) return Status::ok;
        std::vector<Physics::ProviderShapeCastHitV2> raw_hits;
        Status status;
        if (query_shape.kind == Physics::ShapeKindV1::sphere) {
            const Physics::ProviderMeshSphereShapeCastQueryV3 query{
                .center = query_shape.center, .radius = query_shape.radius, .delta = toAbi(delta),
                .colliders = selected.meshes.data(),
                .collider_count = static_cast<std::uint32_t>(selected.meshes.size()),
            };
            status = invokeMeshProvider(Physics::query_mesh_sphere_shape_cast_all,
                &ProviderDispatch::mesh_sphere_shape_cast_all, query, selected, raw_hits);
        } else if (query_shape.kind == Physics::ShapeKindV1::box) {
            const Physics::ProviderMeshBoxShapeCastQueryV3 query{
                .center = query_shape.center, .rotation = query_shape.rotation,
                .half_extents = query_shape.half_extents, .delta = toAbi(delta),
                .colliders = selected.meshes.data(),
                .collider_count = static_cast<std::uint32_t>(selected.meshes.size()),
            };
            status = invokeMeshProvider(Physics::query_mesh_box_shape_cast_all,
                &ProviderDispatch::mesh_box_shape_cast_all, query, selected, raw_hits);
        } else {
            const Physics::ProviderMeshCapsuleShapeCastQueryV3 query{
                .center = query_shape.center, .rotation = query_shape.rotation,
                .half_height = query_shape.half_height, .radius = query_shape.radius,
                .delta = toAbi(delta), .colliders = selected.meshes.data(),
                .collider_count = static_cast<std::uint32_t>(selected.meshes.size()),
            };
            status = invokeMeshProvider(Physics::query_mesh_capsule_shape_cast_all,
                &ProviderDispatch::mesh_capsule_shape_cast_all, query, selected, raw_hits);
        }
        if (status != Status::ok) return status;
        std::vector<bool> seen(selected.meshes.size(), false);
        out_hits.reserve(raw_hits.size());
        for (const auto &raw : raw_hits) {
            Physics::Vec3V1 unit_normal{};
            if (raw.collider_index >= selected.meshes.size() || seen[raw.collider_index] ||
                !validShapeCastHit(raw, unit_normal)) {
                out_hits.clear();
                return Status::provider_error;
            }
            seen[raw.collider_index] = true;
            const auto &identity = selected.identities[raw.collider_index];
            const bool initial = (raw.flags & Physics::shape_cast_initial_overlap) != 0;
            out_hits.push_back(phys::ShapeCastQueryHit{
                identity.name, std::clamp(raw.time_of_impact, 0.0F, 1.0F),
                initial ? raw.penetration_depth : 0.0F,
                fromAbi(raw.position), fromAbi(unit_normal), initial, identity,
                metadata[selected.source_indices[raw.collider_index]]});
        }
        phys::internal::orderShapeCastHits(out_hits);
        return Status::ok;
    } catch (const std::invalid_argument &) {
        out_hits.clear();
        return Status::invalid_argument;
    } catch (const std::bad_alloc &) {
        out_hits.clear();
        return Status::out_of_memory;
    } catch (...) {
        out_hits.clear();
        return Status::provider_error;
    }
}

Physics::Status shapeCastAll(const phys::Shape &moving_shape, vec3 delta,
                             std::span<const phys::Collider> colliders,
                             const phys::QueryFilter *filter,
                             std::vector<phys::ShapeCastQueryHit> &out_hits) noexcept {
    out_hits.clear();
    try {
        const auto abi_delta = toAbi(delta);
        if (!finite(abi_delta)) {
            return Physics::Status::invalid_argument;
        }
        if (colliders.size() > std::numeric_limits<std::uint32_t>::max()) {
            return Physics::Status::out_of_memory;
        }

        std::vector<Physics::ShapeV1> shapes;
        const auto selected = selectColliders(colliders, filter, shapes);
        const Physics::ProviderShapeCastQueryV2 query{
            .shape = toAbi(moving_shape),
            .delta = abi_delta,
            .colliders = shapes.empty() ? nullptr : shapes.data(),
            .collider_count = static_cast<std::uint32_t>(shapes.size()),
        };

        std::vector<Physics::ProviderShapeCastHitV2> raw_hits;
        auto &value = registry();
        {
            std::shared_lock lock{value.mutex};
            const auto *provider = providerForCapabilityLocked(
                value, Physics::query_shape_cast_all);
            if (provider == nullptr) return Physics::Status::unavailable;
            const auto status = invokeProvider(
                *provider, Physics::query_shape_cast_all, provider->shape_cast_all,
                query, selected.size(), raw_hits);
            if (status != Physics::Status::ok) return status;
        }

        std::vector<bool> seen(selected.size(), false);
        out_hits.reserve(raw_hits.size());
        for (const auto &raw : raw_hits) {
            Physics::Vec3V1 unit_normal{};
            const bool initial_overlap =
                (raw.flags & Physics::shape_cast_initial_overlap) != 0;
            if (raw.collider_index >= selected.size() || seen[raw.collider_index] ||
                !validShapeCastHit(raw, unit_normal)) {
                out_hits.clear();
                return Physics::Status::provider_error;
            }

            seen[raw.collider_index] = true;
            const auto &selected_collider = selected[raw.collider_index];
            out_hits.push_back(phys::ShapeCastQueryHit{
                selected_collider.identity.name,
                std::clamp(raw.time_of_impact, 0.0F, 1.0F),
                initial_overlap ? raw.penetration_depth : 0.0F,
                fromAbi(raw.position),
                fromAbi(unit_normal),
                initial_overlap,
                selected_collider.identity,
                selected_collider.collider->metadata,
            });
        }
        phys::internal::orderShapeCastHits(out_hits);
        return Physics::Status::ok;
    } catch (const std::invalid_argument &) {
        out_hits.clear();
        return Physics::Status::invalid_argument;
    } catch (const std::bad_alloc &) {
        out_hits.clear();
        return Physics::Status::out_of_memory;
    } catch (...) {
        out_hits.clear();
        return Physics::Status::provider_error;
    }
}

} // namespace Pelican::physics_internal
