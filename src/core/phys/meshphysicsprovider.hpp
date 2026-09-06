#pragma once

#include "../userpublic/physics/abi_v3.hpp"

#include <span>
namespace Pelican::phys { class TriangleMesh; }

namespace Pelican::physics_internal {
Physics::Status builtinMeshBoxOverlapAll(
    void *, const Physics::ProviderMeshBoxOverlapQueryV3 *,
    Physics::ProviderOverlapHitV1 *, std::uint32_t, std::uint32_t *) noexcept;
Physics::Status builtinMeshCapsuleOverlapAll(
    void *, const Physics::ProviderMeshCapsuleOverlapQueryV3 *,
    Physics::ProviderOverlapHitV1 *, std::uint32_t, std::uint32_t *) noexcept;

Physics::Status builtinMeshSphereShapeCastAll(
    void *, const Physics::ProviderMeshSphereShapeCastQueryV3 *,
    Physics::ProviderShapeCastHitV2 *, std::uint32_t, std::uint32_t *) noexcept;

struct BuiltinMeshQueryContext {
    std::span<const phys::TriangleMesh *const> acceleration;
};

Physics::Status builtinMeshRaycastAll(
    void *, const Physics::ProviderMeshRaycastQueryV3 *,
    Physics::ProviderRaycastHitV1 *, std::uint32_t, std::uint32_t *) noexcept;

Physics::Status builtinMeshSphereOverlapAll(
    void *, const Physics::ProviderMeshSphereOverlapQueryV3 *,
    Physics::ProviderOverlapHitV1 *, std::uint32_t, std::uint32_t *) noexcept;

} // namespace Pelican::physics_internal
