#pragma once

#include "../export.hpp"
#include "query_types.hpp"

namespace Pelican::Physics {

inline constexpr std::uint32_t abiVersionV2 = 2;
inline constexpr std::uint32_t serviceVersionV2 = 2;
inline constexpr std::uint32_t providerVersionV2 = 2;
inline constexpr float shapeCastContactEpsilonV2 = 1.0e-5F;
inline constexpr float shapeCastTieEpsilonV2 = 1.0e-5F;

enum ShapeCastHitFlagBitsV2 : std::uint32_t {
    shape_cast_initial_overlap = 1U << 0U,
};

// The shape orientation is fixed. Its center moves from the encoded start pose
// by delta, and returned time_of_impact values are fractions in [0, 1]. A zero
// delta requests initial-overlap MTD results only. Penetrations no deeper than
// shapeCastContactEpsilonV2 use touching semantics. Equal-depth MTD candidates
// within shapeCastTieEpsilonV2 maximize the lexicographic key
// (dot(normal, preferred), normal.x, normal.y, normal.z), where preferred is
// normalized(-delta), or +X for zero delta.
struct ShapeCastQueryV2 {
    std::uint32_t struct_size = sizeof(ShapeCastQueryV2);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    ShapeV1 shape{};
    Vec3V1 delta{};
    std::uint32_t reserved2 = 0;
    QueryFilterV1 filter{};
};

struct ShapeCastHitV2 {
    ColliderIdentityV1 identity{};
    ColliderMetadataV1 metadata{};
    float time_of_impact = 0.0F;
    float penetration_depth = 0.0F;
    Vec3V1 position{};
    Vec3V1 normal{0.0F, 1.0F, 0.0F};
    std::uint32_t flags = 0;
    std::uint32_t reserved = 0;
};

struct ProviderShapeCastQueryV2 {
    ShapeV1 shape{};
    Vec3V1 delta{};
    std::uint32_t reserved0 = 0;
    const ShapeV1 *colliders = nullptr;
    std::uint32_t collider_count = 0;
    std::uint32_t reserved1 = 0;
};

struct ProviderShapeCastHitV2 {
    // At most one result may be returned for each input collider. For initial
    // overlap, normal * penetration_depth is the MTD for the moving shape.
    std::uint32_t collider_index = 0;
    std::uint32_t flags = 0;
    float time_of_impact = 0.0F;
    float penetration_depth = 0.0F;
    Vec3V1 position{};
    Vec3V1 normal{0.0F, 1.0F, 0.0F};
    std::uint32_t reserved = 0;
};

enum PhysicsQueryCapabilityBitsV2 : std::uint64_t {
    query_shape_cast_all = 1ULL << 2U,
};

inline constexpr std::uint64_t builtinQueryCapabilitiesV2 =
    builtinQueryCapabilitiesV1 | query_shape_cast_all;

using ProviderShapeCastAllV2Fn = Status (*)(
    void *, const ProviderShapeCastQueryV2 *, ProviderShapeCastHitV2 *,
    std::uint32_t, std::uint32_t *) noexcept;

// Provider callbacks are noexcept and may be invoked concurrently. The
// provider and its context must remain alive until unregister_provider returns.
// Report failures with Status; do not call register_provider or
// unregister_provider recursively from a callback.
struct ProviderV2 {
    std::uint32_t struct_size = sizeof(ProviderV2);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    std::uint32_t provider_version = providerVersionV2;
    std::uint32_t minimum_engine_provider_version = providerVersionV2;
    std::uint64_t capability_bits = 0;
    const char *name_utf8 = nullptr;
    std::uint32_t name_size = 0;
    std::uint32_t reserved2 = 0;
    void *context = nullptr;
    ProviderRaycastAllV1Fn raycast_all = nullptr;
    ProviderOverlapAllV1Fn overlap_all = nullptr;
    ProviderShapeCastAllV2Fn shape_cast_all = nullptr;
};

struct ProviderHandleV2 {
    std::uint64_t identity = 0;
    std::uint32_t generation = 0;
    std::uint32_t reserved = 0;
};

struct ServiceV2 {
    std::uint32_t struct_size = sizeof(ServiceV2);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    std::uint32_t service_version = serviceVersionV2;
    std::uint32_t minimum_client_service_version = serviceVersionV2;
    std::uint64_t capability_bits = 0;
    void *context = nullptr;
    Status (*raycast_all)(void *, const RaycastQueryV1 *, RaycastHitV1 *,
                          std::uint32_t, std::uint32_t *) noexcept = nullptr;
    Status (*overlap_all)(void *, const OverlapQueryV1 *, OverlapHitV1 *,
                          std::uint32_t, std::uint32_t *) noexcept = nullptr;
    Status (*shape_cast_all)(void *, const ShapeCastQueryV2 *, ShapeCastHitV2 *,
                             std::uint32_t, std::uint32_t *) noexcept = nullptr;
};

using GetServiceV2Fn = Status (*)(void *, std::uint32_t, ServiceV2 *) noexcept;
using RegisterProviderV2Fn = Status (*)(void *, const ProviderV2 *, ProviderHandleV2 *) noexcept;
using UnregisterProviderV2Fn = Status (*)(void *, ProviderHandleV2) noexcept;

enum PhysicsApiCapabilityBitsV2 : std::uint64_t {
    api_query_service = 1ULL << 0U,
    api_provider_registration = 1ULL << 1U,
};

// Game DLL providers register while their DLL is loading, under the loader's
// registration owner. Calls made later, outside that scope, return wrong_owner;
// this lets hot reload wait for callbacks and unload the DLL safely.
struct ApiV2 {
    std::uint32_t struct_size = sizeof(ApiV2);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    std::uint32_t engine_abi_version = abiVersionV2;
    std::uint32_t minimum_client_abi_version = abiVersionV2;
    std::uint64_t capability_bits = 0;
    void *context = nullptr;
    GetServiceV2Fn get_service = nullptr;
    RegisterProviderV2Fn register_provider = nullptr;
    UnregisterProviderV2Fn unregister_provider = nullptr;
};

PELICAN_API Status getApiV2(std::uint32_t client_abi_version,
                            ApiV2 *out_api) noexcept;

static_assert(std::is_standard_layout_v<ApiV2>);
static_assert(std::is_standard_layout_v<ServiceV2>);
static_assert(std::is_standard_layout_v<ProviderV2>);
static_assert(std::is_standard_layout_v<ProviderHandleV2>);
static_assert(std::is_trivially_copyable_v<ShapeCastQueryV2>);
static_assert(std::is_trivially_copyable_v<ProviderShapeCastHitV2>);
static_assert(offsetof(ApiV2, struct_size) == 0);
static_assert(offsetof(ServiceV2, struct_size) == 0);
static_assert(offsetof(ProviderV2, struct_size) == 0);
static_assert(
    offsetof(ProviderV2, shape_cast_all) ==
    offsetof(ProviderV2, overlap_all) + sizeof(ProviderOverlapAllV1Fn));
static_assert(
    offsetof(ServiceV2, shape_cast_all) ==
    offsetof(ServiceV2, overlap_all) + sizeof(decltype(ServiceV2::overlap_all)));

} // namespace Pelican::Physics
