#pragma once

#include "../export.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Pelican::RenderPass {

inline constexpr std::uint32_t abiVersionV1 = 1;
inline constexpr std::uint32_t descriptorVersionV1 = 1;
inline constexpr std::uint32_t providerVersionV1 = 1;
inline constexpr std::uint32_t maximumProviderNameBytesV1 = 127;
inline constexpr std::uint32_t maximumImplementationIdBytesV1 = 255;
inline constexpr std::uint32_t maximumShaderReferenceBytesV1 = 1023;
inline constexpr char fullscreenContractIdV1[] =
    "pelican.render.fullscreen_pass@1";

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

enum PassImplementationProviderCapabilityBitsV1 : std::uint64_t {
    provider_fullscreen_shader_pair_v1 = 1ULL << 0U,
};

inline constexpr std::uint64_t builtinProviderCapabilitiesV1 =
    provider_fullscreen_shader_pair_v1;

enum class PassKindV1 : std::uint32_t {
    fullscreen = 1,
};

enum class PortDirectionV1 : std::uint32_t {
    input = 0,
    output = 1,
    input_output = 2,
};

enum class AccessModeV1 : std::uint32_t {
    read = 0,
    write = 1,
    read_write = 2,
};

enum class AccessIntentV1 : std::uint32_t {
    automatic = 0,
    sampled = 1,
    attachment = 2,
    storage = 3,
    transfer = 4,
    host = 5,
};

enum class ReadFootprintV1 : std::uint32_t {
    none = 0,
    same_pixel = 1,
    neighborhood = 2,
    arbitrary = 3,
    temporal = 4,
};

enum FullscreenInterfaceFlagBitsV1 : std::uint32_t {
    interface_camera_position = 1U << 0U,
    interface_projection_view = 1U << 1U,
    interface_light_data = 1U << 2U,
};

// Every string is a non-owning UTF-8 byte range valid only for the duration
// of the provider callback. Providers must not retain any supplied pointer.
// type_pattern_json and relations_json are canonical ordered JSON so the C
// ABI can expose the complete logical contract without STL or engine types.
struct PortContractV1 {
    std::uint32_t struct_size = sizeof(PortContractV1);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    const char *name_utf8 = nullptr;
    std::uint32_t name_size = 0;
    std::uint32_t reserved2 = 0;
    const char *type_pattern_json_utf8 = nullptr;
    std::uint32_t type_pattern_json_size = 0;
    std::uint32_t reserved3 = 0;
    const char *relations_json_utf8 = nullptr;
    std::uint32_t relations_json_size = 0;
    std::uint32_t reserved4 = 0;
    PortDirectionV1 direction = PortDirectionV1::input;
    AccessModeV1 access = AccessModeV1::read;
    AccessIntentV1 intent = AccessIntentV1::automatic;
    ReadFootprintV1 footprint = ReadFootprintV1::none;
    std::uint32_t has_footprint_radius = 0;
    std::uint32_t footprint_radius = 0;
    std::uint32_t reserved5 = 0;
};

struct PassContractV1 {
    std::uint32_t struct_size = sizeof(PassContractV1);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    const char *contract_id_utf8 = nullptr;
    std::uint32_t contract_id_size = 0;
    std::uint32_t reserved2 = 0;
    const char *pass_name_utf8 = nullptr;
    std::uint32_t pass_name_size = 0;
    std::uint32_t reserved3 = 0;
    PassKindV1 kind = PassKindV1::fullscreen;
    std::uint32_t interface_flags = 0;
    const PortContractV1 *ports = nullptr;
    std::uint32_t port_count = 0;
    std::uint32_t reserved4 = 0;
    std::uint64_t fingerprint = 0;
};

// The v1 pass-provider slice deliberately permits only a fullscreen shader
// pair replacement. Push constants, light-data use, typed ports, resources,
// effects, and physical planning stay in the engine-owned contract.
struct FullscreenImplementationV1 {
    std::uint32_t struct_size = sizeof(FullscreenImplementationV1);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    const char *implementation_id_utf8 = nullptr;
    std::uint32_t implementation_id_size = 0;
    std::uint32_t reserved2 = 0;
    const char *vertex_shader_utf8 = nullptr;
    std::uint32_t vertex_shader_size = 0;
    std::uint32_t reserved3 = 0;
    const char *fragment_shader_utf8 = nullptr;
    std::uint32_t fragment_shader_size = 0;
    std::uint32_t reserved4 = 0;
};

struct ResolveFullscreenInputV1 {
    std::uint32_t struct_size = sizeof(ResolveFullscreenInputV1);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    const PassContractV1 *contract = nullptr;
    const FullscreenImplementationV1 *authored_implementation = nullptr;
};

// Input ranges are engine-owned and valid only during the callback. Output
// ranges may alias an input range; otherwise they are provider-owned and must
// remain valid until the provider is unregistered. The engine copies output
// ranges before releasing its provider lease.
// The callback may be invoked concurrently in the future and must not call
// register_provider or unregister_provider while its lease is held.
using ResolveFullscreenV1Fn = Status (*)(
    void *, const ResolveFullscreenInputV1 *,
    FullscreenImplementationV1 *) noexcept;

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
    ResolveFullscreenV1Fn resolve_fullscreen = nullptr;
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

static_assert(std::is_standard_layout_v<PortContractV1>);
static_assert(std::is_trivially_copyable_v<PortContractV1>);
static_assert(std::is_standard_layout_v<PassContractV1>);
static_assert(std::is_trivially_copyable_v<PassContractV1>);
static_assert(std::is_standard_layout_v<FullscreenImplementationV1>);
static_assert(std::is_trivially_copyable_v<FullscreenImplementationV1>);
static_assert(std::is_standard_layout_v<ResolveFullscreenInputV1>);
static_assert(std::is_trivially_copyable_v<ResolveFullscreenInputV1>);
static_assert(std::is_standard_layout_v<ProviderV1>);
static_assert(std::is_trivially_copyable_v<ProviderV1>);
static_assert(std::is_standard_layout_v<ProviderHandleV1>);
static_assert(std::is_trivially_copyable_v<ProviderHandleV1>);
static_assert(std::is_standard_layout_v<ApiV1>);
static_assert(std::is_trivially_copyable_v<ApiV1>);
static_assert(offsetof(PortContractV1, struct_size) == 0);
static_assert(offsetof(PassContractV1, struct_size) == 0);
static_assert(offsetof(FullscreenImplementationV1, struct_size) == 0);
static_assert(offsetof(ResolveFullscreenInputV1, struct_size) == 0);
static_assert(offsetof(ProviderV1, struct_size) == 0);
static_assert(offsetof(ApiV1, struct_size) == 0);

} // namespace Pelican::RenderPass
