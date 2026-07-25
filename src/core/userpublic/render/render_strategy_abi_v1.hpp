#pragma once

#include "../export.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Pelican::RenderStrategy {

inline constexpr std::uint32_t abiVersionV1 = 1;
inline constexpr std::uint32_t descriptorVersionV1 = 1;
inline constexpr std::uint32_t providerVersionV1 = 1;
inline constexpr std::uint32_t maximumProviderNameBytesV1 = 127;
inline constexpr std::uint32_t maximumImplementationIdBytesV1 = 255;
inline constexpr std::uint32_t maximumStrategyNameBytesV1 = 255;
inline constexpr std::uint32_t maximumParametersJsonBytesV1 = 65536;
inline constexpr std::uint32_t maximumConfigJsonBytesV1 =
    4U * 1024U * 1024U;
inline constexpr char rendererFacadeContractIdV1[] =
    "pelican.render.renderer_facade@1";
inline constexpr char authoredConfigContractIdV1[] =
    "pelican.render.authored_config@1";

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

enum RenderStrategyProviderCapabilityBitsV1 : std::uint64_t {
    provider_authored_config_json_v1 = 1ULL << 0U,
};

inline constexpr std::uint64_t builtinProviderCapabilitiesV1 =
    provider_authored_config_json_v1;

enum RendererFacadeCapabilityBitsV1 : std::uint64_t {
    facade_feature_composition_v1 = 1ULL << 0U,
    facade_canonical_color_pipeline_v1 = 1ULL << 1U,
    facade_graph_variant_v1 = 1ULL << 2U,
    facade_logical_graph_compile_v1 = 1ULL << 3U,
    facade_graph_transform_chain_v1 = 1ULL << 4U,
    facade_tagged_subgraph_v1 = 1ULL << 5U,
    facade_vulkan_target_lowering_v1 = 1ULL << 6U,
};

inline constexpr std::uint64_t builtinFacadeCapabilitiesV1 =
    facade_feature_composition_v1 |
    facade_canonical_color_pipeline_v1 |
    facade_graph_variant_v1 |
    facade_logical_graph_compile_v1 |
    facade_graph_transform_chain_v1 |
    facade_tagged_subgraph_v1 |
    facade_vulkan_target_lowering_v1;

enum class GraphVariantV1 : std::uint32_t {
    flat = 0,
    preview = 1,
    xr = 2,
};

enum class HistoryPolicyV1 : std::uint32_t {
    preserve = 0,
    forbid = 1,
};

enum class ProjectionJitterPolicyV1 : std::uint32_t {
    preserve = 0,
    forbid = 1,
};

enum class ViewFamilyV1 : std::uint32_t {
    caller_defined = 0,
    mono = 1,
    stereo = 2,
};

enum class ViewExecutionV1 : std::uint32_t {
    caller_defined = 0,
    single_view = 1,
    sequential = 2,
};

enum class ResourceLayoutV1 : std::uint32_t {
    shared_2d = 0,
    sequential_2d = 1,
};

enum class TerminalV1 : std::uint32_t {
    presentation = 0,
    request_local_capture = 1,
    external_view = 2,
};

enum class MirrorOutputV1 : std::uint32_t {
    none = 0,
    left_eye = 1,
};

struct GraphVariantPolicyV1 {
    std::uint32_t struct_size = sizeof(GraphVariantPolicyV1);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    GraphVariantV1 variant = GraphVariantV1::flat;
    HistoryPolicyV1 history = HistoryPolicyV1::preserve;
    ProjectionJitterPolicyV1 projection_jitter =
        ProjectionJitterPolicyV1::preserve;
    ViewFamilyV1 view_family = ViewFamilyV1::caller_defined;
    ViewExecutionV1 view_execution =
        ViewExecutionV1::caller_defined;
    ResourceLayoutV1 resource_layout =
        ResourceLayoutV1::shared_2d;
    TerminalV1 terminal = TerminalV1::presentation;
    MirrorOutputV1 mirror_output = MirrorOutputV1::none;
    std::uint32_t view_count = 0;
    std::uint32_t reserved2 = 0;
    std::uint32_t reserved3 = 0;
    std::uint32_t reserved4 = 0;
};

// This contract describes only mechanisms that the current renderer facade
// actually consumes. Scene material/light/geometry inventories are not
// advertised by v1; a future ABI may add them without pretending the current
// startup-only config compiler owns live scene data.
struct RendererFacadeContractV1 {
    std::uint32_t struct_size = sizeof(RendererFacadeContractV1);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    const char *contract_id_utf8 = nullptr;
    std::uint32_t contract_id_size = 0;
    std::uint32_t reserved2 = 0;
    const char *output_contract_id_utf8 = nullptr;
    std::uint32_t output_contract_id_size = 0;
    std::uint32_t reserved3 = 0;
    std::uint64_t capability_bits = 0;
    GraphVariantPolicyV1 graph_variant_policy;
    std::uint32_t runtime_shader_compiler_enabled = 0;
    std::uint32_t reserved4 = 0;
    std::uint64_t reserved5 = 0;
};

struct ResolveRenderStrategyInputV1 {
    std::uint32_t struct_size =
        sizeof(ResolveRenderStrategyInputV1);
    std::uint32_t version = descriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    const RendererFacadeContractV1 *contract = nullptr;
    const char *strategy_name_utf8 = nullptr;
    std::uint32_t strategy_name_size = 0;
    std::uint32_t reserved2 = 0;
    const char *parameters_json_utf8 = nullptr;
    std::uint32_t parameters_json_size = 0;
    std::uint32_t reserved3 = 0;
    const char *seed_config_json_utf8 = nullptr;
    std::uint32_t seed_config_json_size = 0;
    std::uint32_t reserved4 = 0;
    std::uint64_t seed_config_fingerprint = 0;
};

struct RenderStrategyOutputV1 {
    std::uint32_t struct_size =
        sizeof(RenderStrategyOutputV1);
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

// Input ranges are engine-owned and valid only during the callback. Output
// ranges may alias input ranges; otherwise they remain provider-owned until
// unregister. The engine copies them while holding the provider lease.
// Callbacks may be invoked concurrently in future and must not re-enter any
// render-provider registry while the compile lease is held.
using ResolveRenderStrategyV1Fn = Status (*)(
    void *, const ResolveRenderStrategyInputV1 *,
    RenderStrategyOutputV1 *) noexcept;

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
    ResolveRenderStrategyV1Fn resolve_render_strategy = nullptr;
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

static_assert(std::is_standard_layout_v<GraphVariantPolicyV1>);
static_assert(std::is_trivially_copyable_v<GraphVariantPolicyV1>);
static_assert(std::is_standard_layout_v<RendererFacadeContractV1>);
static_assert(
    std::is_trivially_copyable_v<RendererFacadeContractV1>);
static_assert(
    std::is_standard_layout_v<ResolveRenderStrategyInputV1>);
static_assert(
    std::is_trivially_copyable_v<ResolveRenderStrategyInputV1>);
static_assert(std::is_standard_layout_v<RenderStrategyOutputV1>);
static_assert(std::is_trivially_copyable_v<RenderStrategyOutputV1>);
static_assert(std::is_standard_layout_v<ProviderV1>);
static_assert(std::is_trivially_copyable_v<ProviderV1>);
static_assert(std::is_standard_layout_v<ProviderHandleV1>);
static_assert(std::is_trivially_copyable_v<ProviderHandleV1>);
static_assert(std::is_standard_layout_v<ApiV1>);
static_assert(std::is_trivially_copyable_v<ApiV1>);
static_assert(offsetof(GraphVariantPolicyV1, struct_size) == 0);
static_assert(offsetof(RendererFacadeContractV1, struct_size) == 0);
static_assert(
    offsetof(ResolveRenderStrategyInputV1, struct_size) == 0);
static_assert(offsetof(RenderStrategyOutputV1, struct_size) == 0);
static_assert(offsetof(ProviderV1, struct_size) == 0);
static_assert(offsetof(ApiV1, struct_size) == 0);

} // namespace Pelican::RenderStrategy
