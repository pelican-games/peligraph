#pragma once

#include "../export.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Pelican::RenderGraphTransform {

inline constexpr std::uint32_t abiVersionV1 = 1;
inline constexpr std::uint32_t descriptorVersionV1 = 1;
inline constexpr std::uint32_t providerVersionV1 = 1;
inline constexpr std::uint32_t maximumProviderNameBytesV1 = 127;
inline constexpr std::uint32_t maximumImplementationIdBytesV1 = 255;
inline constexpr std::uint32_t maximumTransformNameBytesV1 = 255;
inline constexpr std::uint32_t maximumGraphNameBytesV1 = 255;
inline constexpr std::uint32_t maximumResourceNameBytesV1 = 255;
inline constexpr std::uint32_t maximumTypeJsonBytesV1 = 16384;
inline constexpr std::uint32_t maximumParametersJsonBytesV1 = 65536;
inline constexpr std::uint32_t maximumConfigJsonBytesV1 =
    4U * 1024U * 1024U;
inline constexpr std::uint32_t maximumLogicalGraphsJsonBytesV1 =
    4U * 1024U * 1024U;
inline constexpr std::uint32_t maximumGraphCountV1 = 64;
inline constexpr std::uint32_t maximumBoundaryPortsV1 = 4096;
inline constexpr char graphSetContractIdV1[] =
    "pelican.render.logical_graph_set@1";

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

enum GraphTransformProviderCapabilityBitsV1 : std::uint64_t {
    provider_logical_config_json_v1 = 1ULL << 0U,
};

inline constexpr std::uint64_t builtinProviderCapabilitiesV1 =
    provider_logical_config_json_v1;

enum class BoundaryRoleV1 : std::uint32_t {
    retained_resource = 0,
    input = 1,
    output = 2,
};

enum class MaterializationV1 : std::uint32_t {
    virtual_resource = 0,
    preferred = 1,
    required = 2,
    external = 3,
};

enum class ImportKindV1 : std::uint32_t {
    none = 0,
    graph_input = 1,
    previous_epoch = 2,
    external = 3,
};

// Every range is engine-owned and valid only during the provider callback.
// type_json is canonical ordered JSON for the concrete logical resource type.
struct BoundaryPortV1 {
    std::uint32_t struct_size = sizeof(BoundaryPortV1);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    const char *graph_name_utf8 = nullptr;
    std::uint32_t graph_name_size = 0;
    std::uint32_t reserved2 = 0;
    const char *resource_utf8 = nullptr;
    std::uint32_t resource_size = 0;
    std::uint32_t reserved3 = 0;
    const char *type_json_utf8 = nullptr;
    std::uint32_t type_json_size = 0;
    std::uint32_t reserved4 = 0;
    BoundaryRoleV1 role =
        BoundaryRoleV1::retained_resource;
    MaterializationV1 materialization =
        MaterializationV1::virtual_resource;
    ImportKindV1 import_kind = ImportKindV1::none;
    std::uint32_t reserved5 = 0;
};

struct GraphSetContractV1 {
    std::uint32_t struct_size = sizeof(GraphSetContractV1);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    const char *contract_id_utf8 = nullptr;
    std::uint32_t contract_id_size = 0;
    std::uint32_t graph_count = 0;
    const BoundaryPortV1 *boundary_ports = nullptr;
    std::uint32_t boundary_port_count = 0;
    std::uint32_t reserved2 = 0;
    std::uint64_t boundary_fingerprint = 0;
};

struct ResolveGraphTransformInputV1 {
    std::uint32_t struct_size =
        sizeof(ResolveGraphTransformInputV1);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    const GraphSetContractV1 *contract = nullptr;
    const char *transform_name_utf8 = nullptr;
    std::uint32_t transform_name_size = 0;
    std::uint32_t transform_index = 0;
    const char *parameters_json_utf8 = nullptr;
    std::uint32_t parameters_json_size = 0;
    std::uint32_t reserved2 = 0;
    const char *config_json_utf8 = nullptr;
    std::uint32_t config_json_size = 0;
    std::uint32_t reserved3 = 0;
    const char *logical_graphs_json_utf8 = nullptr;
    std::uint32_t logical_graphs_json_size = 0;
    std::uint32_t reserved4 = 0;
    std::uint64_t logical_graphs_fingerprint = 0;
};

struct GraphTransformOutputV1 {
    std::uint32_t struct_size =
        sizeof(GraphTransformOutputV1);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    const char *implementation_id_utf8 = nullptr;
    std::uint32_t implementation_id_size = 0;
    std::uint32_t reserved2 = 0;
    const char *config_json_utf8 = nullptr;
    std::uint32_t config_json_size = 0;
    std::uint32_t reserved3 = 0;
};

// Output ranges may alias input ranges. Otherwise they are provider-owned and
// remain valid until unregister. The engine copies them while holding the
// provider lease. Callbacks may be invoked concurrently in future and must
// not re-enter any render-provider registry while the compile lease is held.
using ResolveGraphTransformV1Fn = Status (*)(
    void *, const ResolveGraphTransformInputV1 *,
    GraphTransformOutputV1 *) noexcept;

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
    ResolveGraphTransformV1Fn resolve_graph_transform =
        nullptr;
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
static_assert(std::is_standard_layout_v<GraphSetContractV1>);
static_assert(std::is_trivially_copyable_v<GraphSetContractV1>);
static_assert(
    std::is_standard_layout_v<ResolveGraphTransformInputV1>);
static_assert(
    std::is_trivially_copyable_v<
        ResolveGraphTransformInputV1>);
static_assert(
    std::is_standard_layout_v<GraphTransformOutputV1>);
static_assert(
    std::is_trivially_copyable_v<GraphTransformOutputV1>);
static_assert(std::is_standard_layout_v<ProviderV1>);
static_assert(std::is_trivially_copyable_v<ProviderV1>);
static_assert(std::is_standard_layout_v<ProviderHandleV1>);
static_assert(std::is_trivially_copyable_v<ProviderHandleV1>);
static_assert(std::is_standard_layout_v<ApiV1>);
static_assert(std::is_trivially_copyable_v<ApiV1>);
static_assert(offsetof(BoundaryPortV1, struct_size) == 0);
static_assert(offsetof(GraphSetContractV1, struct_size) == 0);
static_assert(
    offsetof(ResolveGraphTransformInputV1, struct_size) ==
    0);
static_assert(
    offsetof(GraphTransformOutputV1, struct_size) == 0);
static_assert(offsetof(ProviderV1, struct_size) == 0);
static_assert(offsetof(ApiV1, struct_size) == 0);

} // namespace Pelican::RenderGraphTransform
