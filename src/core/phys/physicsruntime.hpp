#pragma once

#include "physquery.hpp"
#include "../userpublic/details/reload/registrationowner.hpp"
#include "../userpublic/physics/abi_v2.hpp"
#include "../userpublic/physics/abi_v3.hpp"

#include <span>
#include <string>
#include <vector>

namespace Pelican::phys { class TriangleMesh; }

namespace Pelican::physics_internal {

Physics::Status registerProvider(const Physics::ProviderV2 &provider,
                                 internal::RegistrationOwner owner,
                                 Physics::ProviderHandleV2 &out_handle) noexcept;
Physics::Status registerProvider(const Physics::ProviderV3 &provider,
                                 internal::RegistrationOwner owner,
                                 Physics::ProviderHandleV2 &out_handle) noexcept;
Physics::Status unregisterProvider(Physics::ProviderHandleV2 handle,
                                   internal::RegistrationOwner owner) noexcept;

// Game DLL providers are registered during LoadLibrary but become authoritative
// only after the loader accepts that owner. Candidate validation never changes
// the live provider.
void activateProviderOwner(internal::RegistrationOwner owner) noexcept;
void releaseProviderOwner(internal::RegistrationOwner owner) noexcept;

[[nodiscard]] std::string activeProviderName();

Physics::Status raycastMeshes(
    const phys::Ray &ray, std::span<const Physics::TriangleMeshV3> meshes,
    std::span<const phys::ColliderIdentity> identities,
    std::span<const phys::ColliderQueryMetadata> metadata,
    const phys::QueryFilter *filter,
    std::vector<phys::RaycastQueryHit> &out_hits,
    // Optional engine-owned immutable BVHs matching the mesh descriptors.
    // External provider callbacks continue to receive only the public payload.
    std::span<const phys::TriangleMesh *const> acceleration = {}) noexcept;

Physics::Status overlapMeshes(
    const phys::Shape &shape, std::span<const Physics::TriangleMeshV3> meshes,
    std::span<const phys::ColliderIdentity> identities,
    std::span<const phys::ColliderQueryMetadata> metadata,
    const phys::QueryFilter *filter, std::vector<phys::OverlapHit> &out_hits,
    std::span<const phys::TriangleMesh *const> acceleration = {}) noexcept;

Physics::Status raycastAll(const phys::Ray &ray,
                           std::span<const phys::Collider> colliders,
                           const phys::QueryFilter *filter,
                           std::vector<phys::RaycastQueryHit> &out_hits) noexcept;
Physics::Status overlapAll(const phys::Shape &shape,
                           std::span<const phys::Collider> colliders,
                           const phys::QueryFilter *filter,
                           std::vector<phys::OverlapHit> &out_hits) noexcept;
Physics::Status shapeCastAll(const phys::Shape &moving_shape, vec3 delta,
                             std::span<const phys::Collider> colliders,
                             const phys::QueryFilter *filter,
                             std::vector<phys::ShapeCastQueryHit> &out_hits) noexcept;

} // namespace Pelican::physics_internal
