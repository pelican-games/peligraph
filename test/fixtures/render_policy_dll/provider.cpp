#include <render/draw_sort_abi_v1.hpp>
#include <render/graph_transform_abi_v1.hpp>
#include <render/pass_implementation_abi_v1.hpp>
#include <render/render_strategy_abi_v1.hpp>
#include <render/subgraph_replacement_abi_v1.hpp>

#include <atomic>
#include <cstdint>
#include <limits>
#include <string_view>

#ifdef _WIN32
#define PELICAN_FIXTURE_EXPORT extern "C" __declspec(dllexport)
#else
#define PELICAN_FIXTURE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#ifndef PELICAN_RENDER_POLICY_FIXTURE_VERSION
#define PELICAN_RENDER_POLICY_FIXTURE_VERSION 1
#endif

namespace {

using namespace Pelican;

constexpr char provider_name[] = "fixture.draw_sort";
constexpr char pass_provider_name[] = "fixture.fullscreen";
constexpr char subgraph_provider_name[] =
    "fixture.subgraph";
constexpr char graph_transform_provider_name[] =
    "fixture.graph_transform";
constexpr char render_strategy_provider_name[] =
    "fixture.render_strategy";
constexpr char pass_vertex_shader[] = "shaders/fixture_fullscreen";
#if PELICAN_RENDER_POLICY_FIXTURE_VERSION == 1
constexpr char pass_implementation_id[] =
    "fixture.render.fullscreen_v1@1";
constexpr char pass_fragment_shader[] =
    "shaders/fixture_composite_v1";
constexpr char subgraph_implementation_id[] =
    "fixture.render.subgraph_v1@1";
constexpr char graph_transform_implementation_id[] =
    "fixture.render.graph_transform_v1@1";
constexpr char render_strategy_implementation_id[] =
    "fixture.render.strategy_v1@1";
constexpr char subgraph_json[] =
    R"json([{
      "name": "fixture_tone_v1",
      "type": "fullscreen",
      "input": ["scene_in"],
      "output": {"color": "scene_out", "depth": null},
      "shader": {
        "vertex": "engine://fullscreen",
        "fragment": "engine://scene_present"
      }
    }])json";
#elif PELICAN_RENDER_POLICY_FIXTURE_VERSION == 2
constexpr char pass_implementation_id[] =
    "fixture.render.fullscreen_v2@1";
constexpr char pass_fragment_shader[] =
    "shaders/fixture_composite_v2";
constexpr char subgraph_implementation_id[] =
    "fixture.render.subgraph_v2@1";
constexpr char graph_transform_implementation_id[] =
    "fixture.render.graph_transform_v2@1";
constexpr char render_strategy_implementation_id[] =
    "fixture.render.strategy_v2@1";
constexpr char subgraph_json[] =
    R"json([{
      "name": "fixture_tone_v2",
      "type": "fullscreen",
      "input": ["scene_in"],
      "output": {"color": "scene_out", "depth": null},
      "shader": {
        "vertex": "engine://fullscreen",
        "fragment": "engine://scene_present"
      }
    }])json";
#else
#error Unsupported PELICAN_RENDER_POLICY_FIXTURE_VERSION
#endif

std::atomic_bool block_next_sort{false};
std::atomic_bool sort_entered{false};
std::atomic_bool resume_sort{false};
std::atomic<std::uint32_t> registration_status{
    static_cast<std::uint32_t>(RenderPolicy::Status::unavailable)};
std::atomic<std::uint32_t> pass_registration_status{
    static_cast<std::uint32_t>(RenderPass::Status::unavailable)};
std::atomic<std::uint32_t> subgraph_registration_status{
    static_cast<std::uint32_t>(
        RenderSubgraph::Status::unavailable)};
std::atomic<std::uint32_t>
    graph_transform_registration_status{
        static_cast<std::uint32_t>(
            RenderGraphTransform::Status::unavailable)};
std::atomic<std::uint32_t>
    render_strategy_registration_status{
        static_cast<std::uint32_t>(
            RenderStrategy::Status::unavailable)};

RenderPolicy::Status sortItems(
    void *, const RenderPolicy::DrawSortInputV1 *input,
    RenderPolicy::DrawSortKeyV1 *output, std::uint32_t capacity,
    std::uint32_t *out_count) noexcept {
    if (input == nullptr || out_count == nullptr ||
        input->struct_size < sizeof(RenderPolicy::DrawSortInputV1) ||
        input->version != RenderPolicy::descriptorVersionV1 ||
        input->reserved0 != 0 || input->reserved1 != 0 ||
        input->reserved2 != 0 ||
        (input->item_count != 0 && input->items == nullptr) ||
        (capacity != 0 && output == nullptr)) {
        return RenderPolicy::Status::invalid_argument;
    }

    if (block_next_sort.exchange(false, std::memory_order_acq_rel)) {
        sort_entered.store(true, std::memory_order_release);
        sort_entered.notify_all();
        while (!resume_sort.load(std::memory_order_acquire)) {
            resume_sort.wait(false, std::memory_order_relaxed);
        }
    }

    *out_count = input->item_count;
    if (capacity < input->item_count) {
        return RenderPolicy::Status::buffer_too_small;
    }
    for (std::uint32_t index = 0; index < input->item_count; ++index) {
#if PELICAN_RENDER_POLICY_FIXTURE_VERSION == 1
        const auto primary = std::numeric_limits<std::uint64_t>::max() -
                             input->items[index].declaration_ordinal;
#elif PELICAN_RENDER_POLICY_FIXTURE_VERSION == 2
        const auto primary = input->items[index].declaration_ordinal;
#else
#error Unsupported PELICAN_RENDER_POLICY_FIXTURE_VERSION
#endif
        output[index] = {.primary = primary};
    }
    return RenderPolicy::Status::ok;
}

RenderPass::Status resolveFullscreen(
    void *, const RenderPass::ResolveFullscreenInputV1 *input,
    RenderPass::FullscreenImplementationV1 *output) noexcept {
    if (input == nullptr || output == nullptr ||
        input->struct_size <
            sizeof(RenderPass::ResolveFullscreenInputV1) ||
        input->version != RenderPass::descriptorVersionV1 ||
        input->reserved0 != 0 || input->reserved1 != 0 ||
        input->contract == nullptr ||
        input->authored_implementation == nullptr ||
        input->contract->struct_size <
            sizeof(RenderPass::PassContractV1) ||
        input->contract->version !=
            RenderPass::descriptorVersionV1 ||
        input->contract->reserved0 != 0 ||
        input->contract->reserved1 != 0 ||
        input->contract->reserved2 != 0 ||
        input->contract->reserved3 != 0 ||
        input->contract->reserved4 != 0 ||
        input->contract->kind !=
            RenderPass::PassKindV1::fullscreen ||
        input->contract->contract_id_utf8 == nullptr ||
        input->contract->contract_id_size !=
            sizeof(RenderPass::fullscreenContractIdV1) - 1 ||
        input->contract->port_count == 0 ||
        input->contract->ports == nullptr ||
        output->struct_size <
            sizeof(RenderPass::FullscreenImplementationV1) ||
        output->version != RenderPass::descriptorVersionV1) {
        return RenderPass::Status::invalid_argument;
    }
    for (std::uint32_t index = 0;
         index < input->contract->port_count; ++index) {
        const auto &port =
            input->contract->ports[index];
        if (port.struct_size <
                sizeof(RenderPass::PortContractV1) ||
            port.version !=
                RenderPass::descriptorVersionV1 ||
            port.reserved0 != 0 ||
            port.reserved1 != 0 ||
            port.reserved2 != 0 ||
            port.reserved3 != 0 ||
            port.reserved4 != 0 ||
            port.reserved5 != 0 ||
            port.name_utf8 == nullptr ||
            port.name_size == 0 ||
            port.type_pattern_json_utf8 == nullptr ||
            port.type_pattern_json_size == 0 ||
            port.relations_json_utf8 == nullptr ||
            port.relations_json_size == 0 ||
            port.has_footprint_radius > 1 ||
            (port.has_footprint_radius == 0 &&
             port.footprint_radius != 0)) {
            return RenderPass::Status::invalid_argument;
        }
    }

    *output =
        RenderPass::descriptor<
            RenderPass::FullscreenImplementationV1>();
    output->implementation_id_utf8 =
        pass_implementation_id;
    output->implementation_id_size =
        static_cast<std::uint32_t>(
            sizeof(pass_implementation_id) - 1);
    output->vertex_shader_utf8 =
        pass_vertex_shader;
    output->vertex_shader_size =
        static_cast<std::uint32_t>(
            sizeof(pass_vertex_shader) - 1);
    output->fragment_shader_utf8 =
        pass_fragment_shader;
    output->fragment_shader_size =
        static_cast<std::uint32_t>(
            sizeof(pass_fragment_shader) - 1);
    return RenderPass::Status::ok;
}

RenderSubgraph::Status resolveSubgraph(
    void *,
    const RenderSubgraph::ResolveRegionInputV1 *input,
    RenderSubgraph::RegionReplacementV1 *output) noexcept {
    if (input == nullptr || output == nullptr ||
        input->struct_size <
            sizeof(
                RenderSubgraph::ResolveRegionInputV1) ||
        input->version !=
            RenderSubgraph::descriptorVersionV1 ||
        input->reserved0 != 0 ||
        input->reserved1 != 0 ||
        input->reserved2 != 0 ||
        input->contract == nullptr ||
        input->contract->struct_size <
            sizeof(RenderSubgraph::RegionContractV1) ||
        input->contract->version !=
            RenderSubgraph::descriptorVersionV1 ||
        input->contract->reserved0 != 0 ||
        input->contract->reserved1 != 0 ||
        input->contract->reserved2 != 0 ||
        input->contract->reserved3 != 0 ||
        input->contract->reserved4 != 0 ||
        input->contract->reserved5 != 0 ||
        input->contract->contract_id_utf8 == nullptr ||
        input->contract->contract_id_size !=
            sizeof(
                RenderSubgraph::regionContractIdV1) -
                1 ||
        input->contract->boundary_port_count == 0 ||
        input->contract->boundary_ports == nullptr ||
        input->authored_subgraph_json_utf8 == nullptr ||
        input->authored_subgraph_json_size == 0 ||
        output->struct_size <
            sizeof(
                RenderSubgraph::RegionReplacementV1) ||
        output->version !=
            RenderSubgraph::descriptorVersionV1) {
        return RenderSubgraph::Status::invalid_argument;
    }
    *output =
        RenderSubgraph::descriptor<
            RenderSubgraph::RegionReplacementV1>();
    output->implementation_id_utf8 =
        subgraph_implementation_id;
    output->implementation_id_size =
        static_cast<std::uint32_t>(
            sizeof(subgraph_implementation_id) - 1);
    output->subgraph_json_utf8 = subgraph_json;
    output->subgraph_json_size =
        static_cast<std::uint32_t>(
            sizeof(subgraph_json) - 1);
    return RenderSubgraph::Status::ok;
}

RenderGraphTransform::Status resolveGraphTransform(
    void *,
    const RenderGraphTransform::
        ResolveGraphTransformInputV1 *input,
    RenderGraphTransform::GraphTransformOutputV1
        *output) noexcept {
    if (input == nullptr || output == nullptr ||
        input->struct_size <
            sizeof(RenderGraphTransform::
                       ResolveGraphTransformInputV1) ||
        input->version !=
            RenderGraphTransform::descriptorVersionV1 ||
        input->reserved0 != 0 ||
        input->reserved1 != 0 ||
        input->reserved2 != 0 ||
        input->reserved3 != 0 ||
        input->reserved4 != 0 ||
        input->contract == nullptr ||
        input->contract->struct_size <
            sizeof(RenderGraphTransform::
                       GraphSetContractV1) ||
        input->contract->version !=
            RenderGraphTransform::descriptorVersionV1 ||
        input->contract->reserved0 != 0 ||
        input->contract->reserved1 != 0 ||
        input->contract->reserved2 != 0 ||
        input->contract->contract_id_utf8 == nullptr ||
        input->contract->contract_id_size !=
            sizeof(RenderGraphTransform::
                       graphSetContractIdV1) -
                1 ||
        input->contract->graph_count == 0 ||
        input->transform_name_utf8 == nullptr ||
        input->transform_name_size == 0 ||
        input->parameters_json_utf8 == nullptr ||
        input->parameters_json_size == 0 ||
        input->config_json_utf8 == nullptr ||
        input->config_json_size == 0 ||
        input->logical_graphs_json_utf8 == nullptr ||
        input->logical_graphs_json_size == 0 ||
        output->struct_size <
            sizeof(RenderGraphTransform::
                       GraphTransformOutputV1) ||
        output->version !=
            RenderGraphTransform::descriptorVersionV1) {
        return RenderGraphTransform::Status::
            invalid_argument;
    }
    for (std::uint32_t index = 0;
         index <
         input->contract->boundary_port_count;
         ++index) {
        const auto &port =
            input->contract->boundary_ports[index];
        if (port.struct_size <
                sizeof(RenderGraphTransform::
                           BoundaryPortV1) ||
            port.version !=
                RenderGraphTransform::
                    descriptorVersionV1 ||
            port.reserved0 != 0 ||
            port.reserved1 != 0 ||
            port.reserved2 != 0 ||
            port.reserved3 != 0 ||
            port.reserved4 != 0 ||
            port.reserved5 != 0 ||
            port.graph_name_utf8 == nullptr ||
            port.graph_name_size == 0 ||
            port.resource_utf8 == nullptr ||
            port.resource_size == 0 ||
            port.type_json_utf8 == nullptr ||
            port.type_json_size == 0) {
            return RenderGraphTransform::Status::
                invalid_argument;
        }
    }
    *output =
        RenderGraphTransform::descriptor<
            RenderGraphTransform::
                GraphTransformOutputV1>();
    output->implementation_id_utf8 =
        graph_transform_implementation_id;
    output->implementation_id_size =
        static_cast<std::uint32_t>(
            sizeof(
                graph_transform_implementation_id) -
            1);
    output->config_json_utf8 =
        input->config_json_utf8;
    output->config_json_size =
        input->config_json_size;
    return RenderGraphTransform::Status::ok;
}

bool equalsRange(
    const char *data, std::uint32_t size,
    std::string_view expected) noexcept {
    return data != nullptr &&
           std::string_view{data, size} == expected;
}

RenderStrategy::Status resolveRenderStrategy(
    void *,
    const RenderStrategy::ResolveRenderStrategyInputV1
        *input,
    RenderStrategy::RenderStrategyOutputV1
        *output) noexcept {
    if (input == nullptr || output == nullptr ||
        input->struct_size <
            sizeof(RenderStrategy::
                       ResolveRenderStrategyInputV1) ||
        input->version !=
            RenderStrategy::descriptorVersionV1 ||
        input->reserved0 != 0 ||
        input->reserved1 != 0 ||
        input->reserved2 != 0 ||
        input->reserved3 != 0 ||
        input->reserved4 != 0 ||
        input->contract == nullptr ||
        input->strategy_name_utf8 == nullptr ||
        input->strategy_name_size == 0 ||
        input->parameters_json_utf8 == nullptr ||
        input->parameters_json_size == 0 ||
        input->seed_config_json_utf8 == nullptr ||
        input->seed_config_json_size == 0 ||
        input->seed_config_fingerprint == 0 ||
        output->struct_size <
            sizeof(RenderStrategy::
                       RenderStrategyOutputV1) ||
        output->version !=
            RenderStrategy::descriptorVersionV1) {
        return RenderStrategy::Status::invalid_argument;
    }

    const auto &contract = *input->contract;
    const auto &policy =
        contract.graph_variant_policy;
    if (contract.struct_size <
            sizeof(RenderStrategy::
                       RendererFacadeContractV1) ||
        contract.version !=
            RenderStrategy::descriptorVersionV1 ||
        contract.reserved0 != 0 ||
        contract.reserved1 != 0 ||
        contract.reserved2 != 0 ||
        contract.reserved3 != 0 ||
        contract.reserved4 != 0 ||
        contract.reserved5 != 0 ||
        !equalsRange(
            contract.contract_id_utf8,
            contract.contract_id_size,
            RenderStrategy::rendererFacadeContractIdV1) ||
        !equalsRange(
            contract.output_contract_id_utf8,
            contract.output_contract_id_size,
            RenderStrategy::authoredConfigContractIdV1) ||
        contract.capability_bits !=
            RenderStrategy::builtinFacadeCapabilitiesV1 ||
        contract.runtime_shader_compiler_enabled > 1 ||
        policy.struct_size <
            sizeof(RenderStrategy::GraphVariantPolicyV1) ||
        policy.version !=
            RenderStrategy::descriptorVersionV1 ||
        policy.reserved0 != 0 ||
        policy.reserved1 != 0 ||
        policy.reserved2 != 0 ||
        policy.reserved3 != 0 ||
        policy.reserved4 != 0) {
        return RenderStrategy::Status::invalid_argument;
    }

    *output =
        RenderStrategy::descriptor<
            RenderStrategy::RenderStrategyOutputV1>();
    output->implementation_id_utf8 =
        render_strategy_implementation_id;
    output->implementation_id_size =
        static_cast<std::uint32_t>(
            sizeof(render_strategy_implementation_id) -
            1);
    output->config_json_utf8 =
        input->seed_config_json_utf8;
    output->config_json_size =
        input->seed_config_json_size;
    return RenderStrategy::Status::ok;
}

struct Registration {
    Registration() noexcept {
        auto api = RenderPolicy::descriptor<RenderPolicy::ApiV1>();
        auto status =
            RenderPolicy::getApiV1(RenderPolicy::abiVersionV1, &api);
        if (status == RenderPolicy::Status::ok) {
            auto provider =
                RenderPolicy::descriptor<RenderPolicy::ProviderV1>();
            provider.capability_bits =
                RenderPolicy::builtinProviderCapabilitiesV1;
            provider.name_utf8 = provider_name;
            provider.name_size =
                static_cast<std::uint32_t>(sizeof(provider_name) - 1);
            provider.sort_items = sortItems;
            RenderPolicy::ProviderHandleV1 handle{};
            status = api.register_provider(api.context, &provider, &handle);
        }
        registration_status.store(static_cast<std::uint32_t>(status),
                                  std::memory_order_release);

        auto pass_api =
            RenderPass::descriptor<RenderPass::ApiV1>();
        auto pass_status =
            RenderPass::getApiV1(
                RenderPass::abiVersionV1, &pass_api);
        if (pass_status == RenderPass::Status::ok) {
            auto provider =
                RenderPass::descriptor<
                    RenderPass::ProviderV1>();
            provider.capability_bits =
                RenderPass::builtinProviderCapabilitiesV1;
            provider.name_utf8 =
                pass_provider_name;
            provider.name_size =
                static_cast<std::uint32_t>(
                    sizeof(pass_provider_name) - 1);
            provider.resolve_fullscreen =
                resolveFullscreen;
            RenderPass::ProviderHandleV1 handle{};
            pass_status =
                pass_api.register_provider(
                    pass_api.context, &provider, &handle);
        }
        pass_registration_status.store(
            static_cast<std::uint32_t>(pass_status),
            std::memory_order_release);

        auto subgraph_api =
            RenderSubgraph::descriptor<
                RenderSubgraph::ApiV1>();
        auto subgraph_status =
            RenderSubgraph::getApiV1(
                RenderSubgraph::abiVersionV1,
                &subgraph_api);
        if (subgraph_status ==
            RenderSubgraph::Status::ok) {
            auto provider =
                RenderSubgraph::descriptor<
                    RenderSubgraph::ProviderV1>();
            provider.capability_bits =
                RenderSubgraph::
                    builtinProviderCapabilitiesV1;
            provider.name_utf8 =
                subgraph_provider_name;
            provider.name_size =
                static_cast<std::uint32_t>(
                    sizeof(subgraph_provider_name) - 1);
            provider.resolve_region =
                resolveSubgraph;
            RenderSubgraph::ProviderHandleV1
                handle{};
            subgraph_status =
                subgraph_api.register_provider(
                    subgraph_api.context, &provider,
                    &handle);
        }
        subgraph_registration_status.store(
            static_cast<std::uint32_t>(
                subgraph_status),
            std::memory_order_release);

        auto transform_api =
            RenderGraphTransform::descriptor<
                RenderGraphTransform::ApiV1>();
        auto transform_status =
            RenderGraphTransform::getApiV1(
                RenderGraphTransform::abiVersionV1,
                &transform_api);
        if (transform_status ==
            RenderGraphTransform::Status::ok) {
            auto provider =
                RenderGraphTransform::descriptor<
                    RenderGraphTransform::ProviderV1>();
            provider.capability_bits =
                RenderGraphTransform::
                    builtinProviderCapabilitiesV1;
            provider.name_utf8 =
                graph_transform_provider_name;
            provider.name_size =
                static_cast<std::uint32_t>(
                    sizeof(
                        graph_transform_provider_name) -
                    1);
            provider.resolve_graph_transform =
                resolveGraphTransform;
            RenderGraphTransform::ProviderHandleV1
                handle{};
            transform_status =
                transform_api.register_provider(
                    transform_api.context, &provider,
                    &handle);
        }
        graph_transform_registration_status.store(
            static_cast<std::uint32_t>(
                transform_status),
            std::memory_order_release);

        auto strategy_api =
            RenderStrategy::descriptor<
                RenderStrategy::ApiV1>();
        auto strategy_status =
            RenderStrategy::getApiV1(
                RenderStrategy::abiVersionV1,
                &strategy_api);
        if (strategy_status ==
            RenderStrategy::Status::ok) {
            auto provider =
                RenderStrategy::descriptor<
                    RenderStrategy::ProviderV1>();
            provider.capability_bits =
                RenderStrategy::
                    builtinProviderCapabilitiesV1;
            provider.name_utf8 =
                render_strategy_provider_name;
            provider.name_size =
                static_cast<std::uint32_t>(
                    sizeof(
                        render_strategy_provider_name) -
                    1);
            provider.resolve_render_strategy =
                resolveRenderStrategy;
            RenderStrategy::ProviderHandleV1
                handle{};
            strategy_status =
                strategy_api.register_provider(
                    strategy_api.context, &provider,
                    &handle);
        }
        render_strategy_registration_status.store(
            static_cast<std::uint32_t>(
                strategy_status),
            std::memory_order_release);
    }
};

Registration registration;

} // namespace

PELICAN_FIXTURE_EXPORT std::uint32_t
pelican_render_policy_fixture_registration_status() {
    return registration_status.load(std::memory_order_acquire);
}

PELICAN_FIXTURE_EXPORT std::uint32_t
pelican_render_pass_fixture_registration_status() {
    return pass_registration_status.load(
        std::memory_order_acquire);
}

PELICAN_FIXTURE_EXPORT std::uint32_t
pelican_render_subgraph_fixture_registration_status() {
    return subgraph_registration_status.load(
        std::memory_order_acquire);
}

PELICAN_FIXTURE_EXPORT std::uint32_t
pelican_render_graph_transform_fixture_registration_status() {
    return graph_transform_registration_status.load(
        std::memory_order_acquire);
}

PELICAN_FIXTURE_EXPORT std::uint32_t
pelican_render_strategy_fixture_registration_status() {
    return render_strategy_registration_status.load(
        std::memory_order_acquire);
}

PELICAN_FIXTURE_EXPORT void pelican_render_policy_fixture_arm_next_sort() {
    sort_entered.store(false, std::memory_order_release);
    resume_sort.store(false, std::memory_order_release);
    block_next_sort.store(true, std::memory_order_release);
}

PELICAN_FIXTURE_EXPORT bool pelican_render_policy_fixture_sort_entered() {
    return sort_entered.load(std::memory_order_acquire);
}

PELICAN_FIXTURE_EXPORT void pelican_render_policy_fixture_resume_sort() {
    resume_sort.store(true, std::memory_order_release);
    resume_sort.notify_all();
}
