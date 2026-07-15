#include "physicsruntime.hpp"

#include "physqueryinternal.hpp"
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

struct RegisteredProvider {
    Physics::ProviderHandleV1 handle{};
    internal::RegistrationOwner owner = internal::engineRegistrationOwner;
    Physics::ProviderV1 table{};
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

bool validHandle(Physics::ProviderHandleV1 handle) {
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

const Physics::ProviderV1 *configuredProviderLocked(const Registry &value,
                                                    std::string_view *name = nullptr) {
    if (const auto *provider = findByOwner(value, internal::engineRegistrationOwner)) {
        if (name != nullptr) *name = provider->name;
        return &provider->table;
    }
#if PELICAN_WITH_JOLT_PHYSICS
    if (name != nullptr) *name = "pelican.jolt";
    return &joltProviderV1();
#elif PELICAN_WITH_BUILTIN_PHYSICS
    if (name != nullptr) *name = "pelican.builtin";
    return &builtinProviderV1();
#else
    if (name != nullptr) *name = {};
    return nullptr;
#endif
}

const Physics::ProviderV1 *providerForCapabilityLocked(const Registry &value,
                                                       std::uint64_t capability) {
    if (const auto *game_provider = activeGameProviderLocked(value);
        game_provider != nullptr &&
        (game_provider->table.capability_bits & capability) != 0) {
        return &game_provider->table;
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
Physics::Status invokeProvider(const Physics::ProviderV1 &provider,
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

bool validProvider(const Physics::ProviderV1 &provider) {
    constexpr auto known_capabilities = Physics::builtinQueryCapabilitiesV1;
    if (provider.struct_size < sizeof(Physics::ProviderV1) ||
        provider.version != Physics::descriptorVersionV1 ||
        provider.reserved0 != 0 || provider.reserved1 != 0 || provider.reserved2 != 0 ||
        provider.provider_version != Physics::providerVersionV1 ||
        provider.minimum_engine_provider_version > Physics::providerVersionV1 ||
        (provider.capability_bits & ~known_capabilities) != 0 ||
        provider.capability_bits == 0 || provider.name_utf8 == nullptr ||
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
    return true;
}

} // namespace

Physics::Status registerProvider(const Physics::ProviderV1 &provider,
                                 internal::RegistrationOwner owner,
                                 Physics::ProviderHandleV1 &out_handle) noexcept {
    out_handle = {};
    if (!validProvider(provider)) {
        return Physics::Status::invalid_argument;
    }
    try {
        auto &value = registry();
        std::unique_lock lock{value.mutex};
        if (findByOwner(value, owner) != nullptr) {
            return Physics::Status::duplicate_provider;
        }
        if (value.next_identity == 0) {
            return Physics::Status::out_of_memory;
        }
        RegisteredProvider registered;
        registered.handle = Physics::ProviderHandleV1{value.next_identity++, 1, 0};
        registered.owner = owner;
        registered.table = provider;
        registered.name.assign(provider.name_utf8, provider.name_size);
        registered.table.name_utf8 = nullptr;
        registered.table.name_size = 0;
        value.providers.push_back(std::move(registered));
        out_handle = value.providers.back().handle;
        return Physics::Status::ok;
    } catch (const std::bad_alloc &) {
        return Physics::Status::out_of_memory;
    } catch (...) {
        return Physics::Status::provider_error;
    }
}

Physics::Status unregisterProvider(Physics::ProviderHandleV1 handle,
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

} // namespace Pelican::physics_internal
