#include <render/draw_sort_abi_v1.hpp>
#include <render/pass_implementation_abi_v1.hpp>

#include <atomic>
#include <cstdint>
#include <limits>

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
constexpr char pass_vertex_shader[] = "shaders/fixture_fullscreen";
#if PELICAN_RENDER_POLICY_FIXTURE_VERSION == 1
constexpr char pass_implementation_id[] =
    "fixture.render.fullscreen_v1@1";
constexpr char pass_fragment_shader[] =
    "shaders/fixture_composite_v1";
#elif PELICAN_RENDER_POLICY_FIXTURE_VERSION == 2
constexpr char pass_implementation_id[] =
    "fixture.render.fullscreen_v2@1";
constexpr char pass_fragment_shader[] =
    "shaders/fixture_composite_v2";
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
