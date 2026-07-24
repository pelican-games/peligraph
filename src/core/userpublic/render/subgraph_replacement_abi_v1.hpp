#pragma once

#include "../export.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Pelican::RenderSubgraph {

inline constexpr std::uint32_t abiVersionV1 = 1;
inline constexpr std::uint32_t descriptorVersionV1 = 1;
inline constexpr std::uint32_t providerVersionV1 = 1;
inline constexpr std::uint32_t maximumProviderNameBytesV1 = 127;
inline constexpr std::uint32_t maximumImplementationIdBytesV1 = 255;
inline constexpr std::uint32_t maximumGraphNameBytesV1 = 255;
inline constexpr std::uint32_t maximumRegionTagBytesV1 = 255;
inline constexpr std::uint32_t maximumResourceNameBytesV1 = 255;
inline constexpr std::uint32_t maximumTypeJsonBytesV1 = 16384;
inline constexpr std::uint32_t maximumSubgraphJsonBytesV1 =
    1024U * 1024U;
inline constexpr std::uint32_t maximumReplacementPassesV1 = 256;
inline constexpr char regionContractIdV1[] =
    "pelican.render.tagged_region@1";

enum class Status : std::uint32_t {
    ok = 0,
    invalid_argument = 1,
    unsupported_version = 2,
    reserved_not_zero = 3,
    duplicate_provider = 4,
    stale_provider = 5,
    wrong_owner = 6,
    stale_owner = 7,
    provider_error = 8,
    out_of_memory = 9,
    unavailable = 10,
};

template <class T> constexpr T descriptor() noexcept {
    T value{};
    value.struct_size = sizeof(T);
    value.version = descriptorVersionV1;
    return value;
}

enum SubgraphProviderCapabilityBitsV1 : std::uint64_t {
    provider_tagged_region_json_v1 = 1ULL << 0U,
};

inline constexpr std::uint64_t builtinProviderCapabilitiesV1 =
    provider_tagged_region_json_v1;

enum class BoundaryDirectionV1 : std::uint32_t {
    input = 0,
    output = 1,
};

enum class MaterializationV1 : std::uint32_t {
    virtual_resource = 0,
    preferred = 1,
    required = 2,
    external = 3,
};

// Every range is non-owning and valid only for the provider callback.
// type_json is canonical ordered JSON for the concrete logical resource type.
struct BoundaryPortV1 {
    std::uint32_t struct_size = sizeof(BoundaryPortV1);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    const char *resource_utf8 = nullptr;
    std::uint32_t resource_size = 0;
    std::uint32_t reserved2 = 0;
    const char *type_json_utf8 = nullptr;
    std::uint32_t type_json_size = 0;
    std::uint32_t reserved3 = 0;
    BoundaryDirectionV1 direction =
        BoundaryDirectionV1::input;
    MaterializationV1 materialization =
        MaterializationV1::virtual_resource;
    std::uint32_t reserved4 = 0;
    std::uint32_t reserved5 = 0;
};

struct RegionContractV1 {
    std::uint32_t struct_size = sizeof(RegionContractV1);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    const char *contract_id_utf8 = nullptr;
    std::uint32_t contract_id_size = 0;
    std::uint32_t reserved2 = 0;
    const char *graph_name_utf8 = nullptr;
    std::uint32_t graph_name_size = 0;
    std::uint32_t reserved3 = 0;
    const char *region_tag_utf8 = nullptr;
    std::uint32_t region_tag_size = 0;
    std::uint32_t reserved4 = 0;
    const BoundaryPortV1 *boundary_ports = nullptr;
    std::uint32_t boundary_port_count = 0;
    std::uint32_t reserved5 = 0;
    std::uint64_t fingerprint = 0;
};

struct RegionReplacementV1 {
    std::uint32_t struct_size = sizeof(RegionReplacementV1);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    const char *implementation_id_utf8 = nullptr;
    std::uint32_t implementation_id_size = 0;
    std::uint32_t reserved2 = 0;
    const char *subgraph_json_utf8 = nullptr;
    std::uint32_t subgraph_json_size = 0;
    std::uint32_t reserved3 = 0;
};

struct ResolveRegionInputV1 {
    std::uint32_t struct_size = sizeof(ResolveRegionInputV1);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    const RegionContractV1 *contract = nullptr;
    const char *authored_subgraph_json_utf8 = nullptr;
    std::uint32_t authored_subgraph_json_size = 0;
    std::uint32_t reserved2 = 0;
};

// Input ranges are engine-owned and valid only during the callback. Output
// ranges may alias an input range; otherwise they are provider-owned and must
// remain valid until the provider is unregistered. The engine copies output
// before releasing its provider lease.
// The callback may be invoked concurrently in the future and must not call
// register_provider or unregister_provider while its lease is held.
using ResolveRegionV1Fn = Status (*)(
    void *, const ResolveRegionInputV1 *,
    RegionReplacementV1 *) noexcept;

struct ProviderV1 {
    std::uint32_t struct_size = sizeof(ProviderV1);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    std::uint32_t provider_version = providerVersionV1;
    std::uint32_t minimum_engine_provider_version =
        providerVersionV1;
    std::uint64_t capability_bits = 0;
    const char *name_utf8 = nullptr;
    std::uint32_t name_size = 0;
    std::uint32_t reserved2 = 0;
    void *context = nullptr;
    ResolveRegionV1Fn resolve_region = nullptr;
};

struct ProviderHandleV1 {
    std::uint64_t identity = 0;
    std::uint32_t generation = 0;
    std::uint32_t reserved = 0;

    friend constexpr bool operator==(
        ProviderHandleV1, ProviderHandleV1) noexcept = default;
};

enum ApiCapabilityBitsV1 : std::uint64_t {
    api_provider_registration = 1ULL << 0U,
};

using RegisterProviderV1Fn = Status (*)(
    void *, const ProviderV1 *, ProviderHandleV1 *) noexcept;
using UnregisterProviderV1Fn = Status (*)(
    void *, ProviderHandleV1) noexcept;

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

PELICAN_API Status getApiV1(
    std::uint32_t client_abi_version,
    ApiV1 *out_api) noexcept;

static_assert(std::is_standard_layout_v<BoundaryPortV1>);
static_assert(std::is_trivially_copyable_v<BoundaryPortV1>);
static_assert(std::is_standard_layout_v<RegionContractV1>);
static_assert(std::is_trivially_copyable_v<RegionContractV1>);
static_assert(std::is_standard_layout_v<RegionReplacementV1>);
static_assert(std::is_trivially_copyable_v<RegionReplacementV1>);
static_assert(std::is_standard_layout_v<ResolveRegionInputV1>);
static_assert(std::is_trivially_copyable_v<ResolveRegionInputV1>);
static_assert(std::is_standard_layout_v<ProviderV1>);
static_assert(std::is_trivially_copyable_v<ProviderV1>);
static_assert(std::is_standard_layout_v<ProviderHandleV1>);
static_assert(std::is_trivially_copyable_v<ProviderHandleV1>);
static_assert(std::is_standard_layout_v<ApiV1>);
static_assert(std::is_trivially_copyable_v<ApiV1>);
static_assert(offsetof(BoundaryPortV1, struct_size) == 0);
static_assert(offsetof(RegionContractV1, struct_size) == 0);
static_assert(offsetof(RegionReplacementV1, struct_size) == 0);
static_assert(offsetof(ResolveRegionInputV1, struct_size) == 0);
static_assert(offsetof(ProviderV1, struct_size) == 0);
static_assert(offsetof(ApiV1, struct_size) == 0);

} // namespace Pelican::RenderSubgraph
