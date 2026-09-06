#pragma once

#include "../userpublic/physics/abi_v3.hpp"

#include <span>
namespace Pelican::phys { class TriangleMesh; }

namespace Pelican::physics_internal {

struct BuiltinMeshQueryContext {
    std::span<const phys::TriangleMesh *const> acceleration;
};

Physics::Status builtinMeshRaycastAll(
    void *, const Physics::ProviderMeshRaycastQueryV3 *,
    Physics::ProviderRaycastHitV1 *, std::uint32_t, std::uint32_t *) noexcept;

} // namespace Pelican::physics_internal
