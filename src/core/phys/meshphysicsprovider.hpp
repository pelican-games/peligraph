#pragma once

#include "../userpublic/physics/abi_v3.hpp"

namespace Pelican::physics_internal {

Physics::Status builtinMeshRaycastAll(
    void *, const Physics::ProviderMeshRaycastQueryV3 *,
    Physics::ProviderRaycastHitV1 *, std::uint32_t, std::uint32_t *) noexcept;

} // namespace Pelican::physics_internal
