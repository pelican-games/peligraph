#pragma once

#include "abi_v2.hpp"

namespace Pelican::Physics {

inline constexpr std::uint32_t abiVersionV3 = 3;
inline constexpr std::uint32_t providerVersionV3 = 3;
inline constexpr std::uint64_t query_mesh_raycast_all = 1ULL << 0U;
inline constexpr std::uint64_t query_mesh_sphere_overlap_all = 1ULL << 1U;
inline constexpr std::uint64_t query_mesh_box_overlap_all = 1ULL << 2U;

// Borrowed, immutable memory valid for the callback duration. Explicit index
// triples describe double-sided triangles. Normals follow transformed winding.
// Matrix is affine column-major; empty geometry is valid. No provider may retain
// these pointers. Degenerate triangles contribute no surface hits.
struct TriangleMeshV3 {
    const Vec3V1 *vertices = nullptr;
    std::uint32_t vertex_count = 0;
    std::uint32_t reserved0 = 0;
    const std::uint32_t *indices = nullptr;
    std::uint32_t index_count = 0;
    std::uint32_t reserved1 = 0;
    float local_to_world[16] = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

struct ProviderMeshRaycastQueryV3 {
    // Same world-distance and direction semantics as ProviderRaycastQueryV1.
    RayV1 ray{};
    const TriangleMeshV3 *colliders = nullptr;
    std::uint32_t collider_count = 0;
    std::uint32_t reserved = 0;
};

using ProviderMeshRaycastAllV3Fn = Status (*)(
    void *, const ProviderMeshRaycastQueryV3 *, ProviderRaycastHitV1 *,
    std::uint32_t, std::uint32_t *) noexcept;

// Touching a triangle surface counts as overlap. A closed mesh does not imply
// a filled volume: a sphere entirely inside without touching has no overlap.
struct ProviderMeshSphereOverlapQueryV3 {
    Vec3V1 center{};
    float radius = 0;
    const TriangleMeshV3 *colliders = nullptr;
    std::uint32_t collider_count = 0;
    std::uint32_t reserved = 0;
};

using ProviderMeshSphereOverlapAllV3Fn = Status (*)(
    void *, const ProviderMeshSphereOverlapQueryV3 *, ProviderOverlapHitV1 *,
    std::uint32_t, std::uint32_t *) noexcept;

struct ProviderMeshBoxOverlapQueryV3 {
    Vec3V1 center{};
    QuatV1 rotation{};
    Vec3V1 half_extents{};
    const TriangleMeshV3 *colliders = nullptr;
    std::uint32_t collider_count = 0;
    std::uint32_t reserved = 0;
};

using ProviderMeshBoxOverlapAllV3Fn = Status (*)(
    void *, const ProviderMeshBoxOverlapQueryV3 *, ProviderOverlapHitV1 *,
    std::uint32_t, std::uint32_t *) noexcept;

struct ProviderV3 {
    std::uint32_t struct_size = sizeof(ProviderV3);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    std::uint32_t provider_version = providerVersionV3;
    std::uint32_t minimum_engine_provider_version = providerVersionV3;
    // Unchanged primitive descriptor. Its context/name and registration owner
    // also apply to mesh callbacks. Zero primitive capabilities allow mesh-only
    // providers; at least one primitive or mesh capability is required.
    ProviderV2 base{};
    std::uint64_t mesh_capability_bits = 0;
    ProviderMeshRaycastAllV3Fn mesh_raycast_all = nullptr;
    ProviderMeshSphereOverlapAllV3Fn mesh_sphere_overlap_all = nullptr;
    ProviderMeshBoxOverlapAllV3Fn mesh_box_overlap_all = nullptr;
};

using RegisterProviderV3Fn = Status (*)(void *, const ProviderV3 *, ProviderHandleV2 *) noexcept;

struct ApiV3 {
    std::uint32_t struct_size = sizeof(ApiV3);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    std::uint32_t engine_abi_version = abiVersionV3;
    std::uint32_t minimum_client_abi_version = abiVersionV3;
    ApiV2 base{};
    RegisterProviderV3Fn register_provider_v3 = nullptr;
};

PELICAN_API Status getApiV3(std::uint32_t client_abi_version, ApiV3 *out_api) noexcept;

static_assert(std::is_standard_layout_v<ProviderV3>);
static_assert(std::is_standard_layout_v<ApiV3>);
static_assert(std::is_trivially_copyable_v<TriangleMeshV3>);

} // namespace Pelican::Physics
