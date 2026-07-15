#pragma once

#include "physquery.hpp"
#include "../userpublic/details/reload/registrationowner.hpp"
#include "../userpublic/physics/abi_v2.hpp"

#include <span>
#include <string>
#include <vector>

namespace Pelican::physics_internal {

Physics::Status registerProvider(const Physics::ProviderV1 &provider,
                                 internal::RegistrationOwner owner,
                                 Physics::ProviderHandleV1 &out_handle) noexcept;
Physics::Status registerProvider(const Physics::ProviderV2 &provider,
                                 internal::RegistrationOwner owner,
                                 Physics::ProviderHandleV2 &out_handle) noexcept;
Physics::Status unregisterProvider(Physics::ProviderHandleV1 handle,
                                   internal::RegistrationOwner owner) noexcept;

// Game DLL providers are registered during LoadLibrary but become authoritative
// only after the loader accepts that owner. Candidate validation never changes
// the live provider.
void activateProviderOwner(internal::RegistrationOwner owner) noexcept;
void releaseProviderOwner(internal::RegistrationOwner owner) noexcept;

[[nodiscard]] std::string activeProviderName();

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
