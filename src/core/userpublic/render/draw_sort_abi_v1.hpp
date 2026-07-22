#pragma once

#include "../export.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Pelican::RenderPolicy {

inline constexpr std::uint32_t abiVersionV1 = 1;
inline constexpr std::uint32_t descriptorVersionV1 = 1;
inline constexpr std::uint32_t providerVersionV1 = 1;
inline constexpr std::uint32_t maximumProviderNameBytesV1 = 127;

enum class Status : std::uint32_t {
    ok = 0,
    buffer_too_small = 1,
    invalid_argument = 2,
    unsupported_version = 3,
    reserved_not_zero = 4,
    duplicate_provider = 5,
    stale_provider = 6,
    wrong_owner = 7,
    stale_owner = 8,
    provider_error = 9,
    out_of_memory = 10,
    unavailable = 11,
};

template <class T> constexpr T descriptor() noexcept {
    T value{};
    value.struct_size = sizeof(T);
    value.version = descriptorVersionV1;
    return value;
}

enum DrawSortProviderCapabilityBitsV1 : std::uint64_t {
    provider_key_pair_v1 = 1ULL << 0U,
};

inline constexpr std::uint64_t builtinProviderCapabilitiesV1 =
    provider_key_pair_v1;

enum class DrawSortPhaseV1 : std::uint32_t {
    mixed = 0,
    opaque = 1,
    transparent = 2,
};

enum class DrawSortLogicalViewV1 : std::uint32_t {
    shared = 0,
    third_person = 1,
    first_person = 2,
};

enum class MaterialRouteV1 : std::uint32_t {
    deferred_geometry = 0,
    forward_opaque = 1,
    forward_transparent = 2,
};

enum class MaterialPhaseV1 : std::uint32_t {
    opaque = 0,
    transparent = 1,
};

enum DrawSortItemFlagBitsV1 : std::uint32_t {
    item_skinned = 1U << 0U,
};

enum DrawSortViewMaskBitsV1 : std::uint32_t {
    view_third_person = 1U << 0U,
    view_first_person = 1U << 1U,
    view_both = view_third_person | view_first_person,
};

struct Vec3V1 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

// Data-only snapshot copied by the engine for the duration of one callback.
// Providers must not retain any pointer supplied through DrawSortInputV1.
struct DrawSortItemV1 {
    std::uint32_t instance_index = 0;
    std::uint32_t instance_generation = 0;
    std::uint64_t scene_epoch = 0;
    std::uint32_t mesh_index = 0;
    std::uint32_t primitive_index = 0;
    std::uint32_t node_index = 0;
    std::int32_t material_id = -1;
    std::uint32_t source_material_index = 0;
    std::uint64_t declaration_ordinal = 0;
    std::uint32_t flags = 0;
    MaterialRouteV1 route = MaterialRouteV1::deferred_geometry;
    MaterialPhaseV1 phase = MaterialPhaseV1::opaque;
    std::uint32_t view_mask = view_both;
    std::uint32_t has_world_bounds = 0;
    Vec3V1 world_bounds_minimum{};
    Vec3V1 world_bounds_maximum{};
    std::uint32_t reserved = 0;
};

struct DrawSortInputV1 {
    std::uint32_t struct_size = sizeof(DrawSortInputV1);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    DrawSortPhaseV1 target_phase = DrawSortPhaseV1::mixed;
    DrawSortLogicalViewV1 logical_view = DrawSortLogicalViewV1::shared;
    const DrawSortItemV1 *items = nullptr;
    std::uint32_t item_count = 0;
    std::uint32_t reserved2 = 0;
    // Additive v1 tail. Older providers may ignore it by using the prefix
    // size they were compiled against. New depth policies require the flag
    // and consume this canonical world-space origin/forward snapshot.
    std::uint32_t has_logical_view_snapshot = 0;
    std::uint32_t reserved3 = 0;
    Vec3V1 logical_view_origin{};
    Vec3V1 logical_view_forward{0.0F, 0.0F, -1.0F};
};

// Locks the byte extent understood by providers compiled before the logical
// view tail was added. Such providers continue to consume this prefix while
// current providers gate tail access with struct_size.
inline constexpr std::uint32_t drawSortInputV1LegacyPrefixSize =
    static_cast<std::uint32_t>(
        offsetof(DrawSortInputV1, has_logical_view_snapshot));

// Keys are compared lexicographically in ascending order. A provider may
// invert a component when it needs descending order. The engine always adds a
// stable draw identity after these two keys.
struct DrawSortKeyV1 {
    std::uint64_t primary = 0;
    std::uint64_t secondary = 0;
};

// The engine keeps the selected provider leased while this callback runs so
// its owner DLL cannot be unloaded. A callback must not call register_provider
// or unregister_provider: those operations wait for outstanding leases.
using SortItemsV1Fn = Status (*)(
    void *, const DrawSortInputV1 *, DrawSortKeyV1 *, std::uint32_t,
    std::uint32_t *) noexcept;

struct ProviderV1 {
    std::uint32_t struct_size = sizeof(ProviderV1);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    std::uint32_t provider_version = providerVersionV1;
    std::uint32_t minimum_engine_provider_version = providerVersionV1;
    std::uint64_t capability_bits = 0;
    const char *name_utf8 = nullptr;
    std::uint32_t name_size = 0;
    std::uint32_t reserved2 = 0;
    void *context = nullptr;
    SortItemsV1Fn sort_items = nullptr;
};

struct ProviderHandleV1 {
    std::uint64_t identity = 0;
    std::uint32_t generation = 0;
    std::uint32_t reserved = 0;

    friend constexpr bool operator==(ProviderHandleV1,
                                     ProviderHandleV1) noexcept = default;
};

enum ApiCapabilityBitsV1 : std::uint64_t {
    api_provider_registration = 1ULL << 0U,
};

using RegisterProviderV1Fn = Status (*)(void *, const ProviderV1 *,
                                        ProviderHandleV1 *) noexcept;
using UnregisterProviderV1Fn = Status (*)(void *, ProviderHandleV1) noexcept;

struct ApiV1 {
    std::uint32_t struct_size = sizeof(ApiV1);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    std::uint32_t engine_abi_version = abiVersionV1;
    std::uint32_t minimum_client_abi_version = abiVersionV1;
    std::uint64_t capability_bits = 0;
    void *context = nullptr;
    RegisterProviderV1Fn register_provider = nullptr;
    UnregisterProviderV1Fn unregister_provider = nullptr;
};

PELICAN_API Status getApiV1(std::uint32_t client_abi_version,
                            ApiV1 *out_api) noexcept;

static_assert(std::is_standard_layout_v<Vec3V1>);
static_assert(std::is_trivially_copyable_v<Vec3V1>);
static_assert(std::is_standard_layout_v<DrawSortItemV1>);
static_assert(std::is_trivially_copyable_v<DrawSortItemV1>);
static_assert(std::is_standard_layout_v<DrawSortInputV1>);
static_assert(std::is_trivially_copyable_v<DrawSortInputV1>);
static_assert(std::is_standard_layout_v<DrawSortKeyV1>);
static_assert(std::is_trivially_copyable_v<DrawSortKeyV1>);
static_assert(std::is_standard_layout_v<ProviderV1>);
static_assert(std::is_trivially_copyable_v<ProviderV1>);
static_assert(std::is_standard_layout_v<ProviderHandleV1>);
static_assert(std::is_trivially_copyable_v<ProviderHandleV1>);
static_assert(std::is_standard_layout_v<ApiV1>);
static_assert(std::is_trivially_copyable_v<ApiV1>);
static_assert(offsetof(DrawSortInputV1, struct_size) == 0);
static_assert(drawSortInputV1LegacyPrefixSize ==
              offsetof(DrawSortInputV1, reserved2) +
                  sizeof(std::uint32_t));
static_assert(offsetof(ProviderV1, struct_size) == 0);
static_assert(offsetof(ApiV1, struct_size) == 0);

} // namespace Pelican::RenderPolicy
