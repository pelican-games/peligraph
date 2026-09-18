#include "renderpolicyregistry.hpp"
#include "../registrationownerhost.hpp"
#include "../renderingpass/providerregistrycommon.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Pelican {
namespace {

using RenderPolicy::Status;

bool validPhase(RenderPolicy::DrawSortPhaseV1 phase) noexcept {
    switch (phase) {
    case RenderPolicy::DrawSortPhaseV1::mixed:
    case RenderPolicy::DrawSortPhaseV1::opaque:
    case RenderPolicy::DrawSortPhaseV1::transparent:
        return true;
    }
    return false;
}

bool validLogicalView(RenderPolicy::DrawSortLogicalViewV1 view) noexcept {
    switch (view) {
    case RenderPolicy::DrawSortLogicalViewV1::shared:
    case RenderPolicy::DrawSortLogicalViewV1::third_person:
    case RenderPolicy::DrawSortLogicalViewV1::first_person:
        return true;
    }
    return false;
}

bool validRoutePhase(RenderPolicy::MaterialRouteV1 route,
                     RenderPolicy::MaterialPhaseV1 phase) noexcept {
    switch (route) {
    case RenderPolicy::MaterialRouteV1::deferred_geometry:
    case RenderPolicy::MaterialRouteV1::forward_opaque:
        return phase == RenderPolicy::MaterialPhaseV1::opaque;
    case RenderPolicy::MaterialRouteV1::forward_transparent:
        return phase == RenderPolicy::MaterialPhaseV1::transparent;
    }
    return false;
}

bool finiteVec3(RenderPolicy::Vec3V1 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool validViewSnapshot(const RenderPolicy::DrawSortInputV1 &input) noexcept {
    if (input.has_logical_view_snapshot > 1 || input.reserved3 != 0)
        return false;
    if (input.has_logical_view_snapshot == 0) return true;
    if (!finiteVec3(input.logical_view_origin) ||
        !finiteVec3(input.logical_view_forward))
        return false;
    const auto length_squared =
        input.logical_view_forward.x * input.logical_view_forward.x +
        input.logical_view_forward.y * input.logical_view_forward.y +
        input.logical_view_forward.z * input.logical_view_forward.z;
    return std::isfinite(length_squared) && length_squared > 0.0F;
}

bool validItem(const RenderPolicy::DrawSortItemV1 &item) noexcept {
    constexpr std::uint32_t known_flags = RenderPolicy::item_skinned;
    if (item.material_id < 0 || (item.flags & ~known_flags) != 0 ||
        !validRoutePhase(item.route, item.phase) ||
        (item.view_mask != RenderPolicy::view_third_person &&
         item.view_mask != RenderPolicy::view_first_person &&
         item.view_mask != RenderPolicy::view_both) ||
        item.has_world_bounds > 1 || item.reserved != 0) {
        return false;
    }
    if (item.has_world_bounds == 0) return true;
    if (!finiteVec3(item.world_bounds_minimum) ||
        !finiteVec3(item.world_bounds_maximum))
        return false;
    return item.world_bounds_minimum.x <= item.world_bounds_maximum.x &&
           item.world_bounds_minimum.y <= item.world_bounds_maximum.y &&
           item.world_bounds_minimum.z <= item.world_bounds_maximum.z;
}

Status stateBatchedSort(
    void *, const RenderPolicy::DrawSortInputV1 *input,
    RenderPolicy::DrawSortKeyV1 *output, std::uint32_t output_capacity,
    std::uint32_t *out_count) noexcept {
    if (input == nullptr || out_count == nullptr ||
        input->struct_size < sizeof(RenderPolicy::DrawSortInputV1) ||
        input->version != RenderPolicy::descriptorVersionV1 ||
        input->reserved0 != 0 || input->reserved1 != 0 ||
        input->reserved2 != 0 || !validPhase(input->target_phase) ||
        !validLogicalView(input->logical_view) || !validViewSnapshot(*input) ||
        (input->item_count != 0 && input->items == nullptr) ||
        (output_capacity != 0 && output == nullptr)) {
        return Status::invalid_argument;
    }
    *out_count = input->item_count;
    if (output_capacity < input->item_count) {
        return Status::buffer_too_small;
    }

    for (std::uint32_t index = 0; index < input->item_count; ++index) {
        const auto &item = input->items[index];
        if (!validItem(item)) {
            return Status::invalid_argument;
        }

        const auto material = static_cast<std::uint32_t>(item.material_id);
        const auto skinned =
            (item.flags & RenderPolicy::item_skinned) != 0 ? 1ULL : 0ULL;
        std::uint64_t visibility = 0;
        if (item.view_mask == RenderPolicy::view_both) {
            visibility = 1;
        } else if (item.view_mask == RenderPolicy::view_first_person) {
            visibility = 2;
        }
        output[index] = RenderPolicy::DrawSortKeyV1{
            .primary = (static_cast<std::uint64_t>(material) << 32U) |
                       item.source_material_index,
            .secondary = (skinned << 32U) | visibility,
        };
    }
    return Status::ok;
}

std::uint32_t descendingFloatKey(float value) noexcept {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto ascending = (bits & 0x80000000U) != 0
                               ? ~bits
                               : (bits ^ 0x80000000U);
    return ~ascending;
}

Status backToFrontSort(
    void *, const RenderPolicy::DrawSortInputV1 *input,
    RenderPolicy::DrawSortKeyV1 *output, std::uint32_t output_capacity,
    std::uint32_t *out_count) noexcept {
    if (input == nullptr || out_count == nullptr ||
        input->struct_size < sizeof(RenderPolicy::DrawSortInputV1) ||
        input->version != RenderPolicy::descriptorVersionV1 ||
        input->reserved0 != 0 || input->reserved1 != 0 ||
        input->reserved2 != 0 || input->reserved3 != 0 ||
        input->target_phase != RenderPolicy::DrawSortPhaseV1::transparent ||
        !validLogicalView(input->logical_view) || !validViewSnapshot(*input) ||
        input->has_logical_view_snapshot == 0 ||
        (input->item_count != 0 && input->items == nullptr) ||
        (output_capacity != 0 && output == nullptr)) {
        return Status::invalid_argument;
    }
    *out_count = input->item_count;
    if (output_capacity < input->item_count) {
        return Status::buffer_too_small;
    }

    for (std::uint32_t index = 0; index < input->item_count; ++index) {
        const auto &item = input->items[index];
        if (!validItem(item) || item.has_world_bounds == 0 ||
            item.phase != RenderPolicy::MaterialPhaseV1::transparent) {
            return Status::invalid_argument;
        }
        const RenderPolicy::Vec3V1 center{
            item.world_bounds_minimum.x * 0.5F +
                item.world_bounds_maximum.x * 0.5F,
            item.world_bounds_minimum.y * 0.5F +
                item.world_bounds_maximum.y * 0.5F,
            item.world_bounds_minimum.z * 0.5F +
                item.world_bounds_maximum.z * 0.5F,
        };
        const auto depth =
            (center.x - input->logical_view_origin.x) *
                input->logical_view_forward.x +
            (center.y - input->logical_view_origin.y) *
                input->logical_view_forward.y +
            (center.z - input->logical_view_origin.z) *
                input->logical_view_forward.z;
        if (!std::isfinite(depth)) return Status::invalid_argument;
        output[index] = RenderPolicy::DrawSortKeyV1{
            .primary = descendingFloatKey(depth),
            .secondary =
                (static_cast<std::uint64_t>(
                     static_cast<std::uint32_t>(item.material_id))
                 << 32U) |
                item.source_material_index,
        };
    }
    return Status::ok;
}

bool validProvider(const RenderPolicy::ProviderV1 &provider) noexcept {
    constexpr auto known_capabilities =
        RenderPolicy::builtinProviderCapabilitiesV1;
    if (provider.struct_size < sizeof(RenderPolicy::ProviderV1) ||
        provider.version != RenderPolicy::descriptorVersionV1 ||
        provider.reserved0 != 0 || provider.reserved1 != 0 ||
        provider.reserved2 != 0 ||
        provider.provider_version != RenderPolicy::providerVersionV1 ||
        provider.minimum_engine_provider_version >
            RenderPolicy::providerVersionV1 ||
        provider.capability_bits != known_capabilities ||
        provider.name_utf8 == nullptr || provider.name_size == 0 ||
        provider.name_size > RenderPolicy::maximumProviderNameBytesV1 ||
        provider.sort_items == nullptr) {
        return false;
    }
    return std::find(provider.name_utf8,
                     provider.name_utf8 + provider.name_size, '\0') ==
           provider.name_utf8 + provider.name_size;
}

int api_context_token = 0;

} // namespace

struct DrawSortProviderLease::Impl {
    std::shared_lock<std::shared_mutex> lock;
    void *context = nullptr;
    RenderPolicy::SortItemsV1Fn sort_items = nullptr;

    Impl(std::shared_lock<std::shared_mutex> provider_lock, void *provider_context,
         RenderPolicy::SortItemsV1Fn callback) noexcept
        : lock{std::move(provider_lock)}, context{provider_context},
          sort_items{callback} {}
};

DrawSortProviderLease::DrawSortProviderLease() noexcept = default;
DrawSortProviderLease::~DrawSortProviderLease() = default;
DrawSortProviderLease::DrawSortProviderLease(DrawSortProviderLease &&) noexcept =
    default;
DrawSortProviderLease &
DrawSortProviderLease::operator=(DrawSortProviderLease &&) noexcept = default;

DrawSortProviderLease::DrawSortProviderLease(std::unique_ptr<Impl> impl,
                                             DrawSortProviderInfo info) noexcept
    : impl_{std::move(impl)}, info_{std::move(info)} {}

RenderPolicy::Status DrawSortProviderLease::sort(
    const RenderPolicy::DrawSortInputV1 &input,
    std::span<RenderPolicy::DrawSortKeyV1> output,
    std::uint32_t &out_count) const noexcept {
    out_count = 0;
    if (!impl_ || impl_->sort_items == nullptr ||
        output.size() > std::numeric_limits<std::uint32_t>::max()) {
        return Status::invalid_argument;
    }
    return impl_->sort_items(
        impl_->context, &input, output.data(),
        static_cast<std::uint32_t>(output.size()), &out_count);
}

// The owner lifecycle is the one the four renderingpass provider
// registries share, so it comes from the same template. Only the
// two builtin draw sort providers and the lease-shaped resolve
// below are this registry's own -- this registry seeds itself in
// its own constructor and never calls registerBuiltinProvider(),
// which is why the builtin_* members of the traits are absent.
struct RenderPolicyRegistryTraits {
    using Provider = RenderPolicy::ProviderV1;
    using Handle = RenderPolicy::ProviderHandleV1;
    using Status = RenderPolicy::Status;
    using Resolve = RenderPolicy::SortItemsV1Fn;

    static constexpr auto provider_resolve = &Provider::sort_items;

    static bool valid(const Provider &provider) noexcept {
        return validProvider(provider);
    }
};

struct RenderPolicyRegistry::Impl
    : provider_registry_common::ProviderRegistry<
          RenderPolicyRegistryTraits> {};

RenderPolicyRegistry::RenderPolicyRegistry() : impl_{std::make_unique<Impl>()} {
    const auto register_builtin = [&](std::string_view name,
                                      RenderPolicy::SortItemsV1Fn sort) {
        auto provider = RenderPolicy::descriptor<RenderPolicy::ProviderV1>();
        provider.capability_bits =
            RenderPolicy::builtinProviderCapabilitiesV1;
        provider.name_utf8 = name.data();
        provider.name_size = static_cast<std::uint32_t>(name.size());
        provider.sort_items = sort;
        RenderPolicy::ProviderHandleV1 handle{};
        if (registerDrawSortProvider(provider,
                                     internal::engineRegistrationOwner,
                                     handle) != Status::ok) {
            throw std::logic_error(
                "failed to register builtin draw sort provider '" +
                std::string{name} + "'");
        }
    };
    register_builtin(builtinStateBatchedDrawSortProvider, stateBatchedSort);
    register_builtin(builtinBackToFrontDrawSortProvider, backToFrontSort);
}

RenderPolicyRegistry::~RenderPolicyRegistry() = default;

RenderPolicy::Status RenderPolicyRegistry::registerDrawSortProvider(
    const RenderPolicy::ProviderV1 &provider,
    internal::RegistrationOwner owner,
    RenderPolicy::ProviderHandleV1 &out_handle) noexcept {
    return impl_->registerProvider(provider, owner, out_handle);
}

RenderPolicy::Status RenderPolicyRegistry::unregisterDrawSortProvider(
    RenderPolicy::ProviderHandleV1 handle,
    internal::RegistrationOwner owner) noexcept {
    return impl_->unregisterProvider(handle, owner);
}

DrawSortProviderLease RenderPolicyRegistry::resolveDrawSortProvider(
    std::string_view name) const {
    std::shared_lock lock{impl_->mutex};
    const Impl::ProviderSlot *selected = impl_->findActiveProvider(name);
    if (selected == nullptr) {
        throw std::runtime_error("draw sort provider '" + std::string{name} +
                                 "' is not registered for the active owner");
    }

    const auto slot_index = static_cast<std::uint64_t>(
        selected - impl_->providers.data());
    DrawSortProviderInfo info{
        .name = selected->name,
        .handle = RenderPolicy::ProviderHandleV1{
            .identity = slot_index + 1,
            .generation = selected->generation,
        },
        .owner = selected->owner,
        .provider_version = selected->provider_version,
        .capability_bits = selected->capability_bits,
    };
    auto lease = std::make_unique<DrawSortProviderLease::Impl>(
        std::move(lock), selected->context, selected->resolve);
    return DrawSortProviderLease{std::move(lease), std::move(info)};
}

void RenderPolicyRegistry::activateOwner(
    internal::RegistrationOwner owner) noexcept {
    impl_->activateOwner(owner);
}

void RenderPolicyRegistry::releaseOwner(
    internal::RegistrationOwner owner) noexcept {
    impl_->releaseOwner(owner);
}

RenderPolicyRegistry &renderPolicyRegistry() {
    static auto *value = new RenderPolicyRegistry;
    return *value;
}

namespace render_policy_internal {

void activateProviderOwner(internal::RegistrationOwner owner) noexcept {
    try {
        renderPolicyRegistry().activateOwner(owner);
    } catch (...) {
        std::terminate();
    }
}

void releaseProviderOwner(internal::RegistrationOwner owner) noexcept {
    try {
        renderPolicyRegistry().releaseOwner(owner);
    } catch (...) {
        std::terminate();
    }
}

} // namespace render_policy_internal

namespace {

Status registerProviderApi(void *context,
                           const RenderPolicy::ProviderV1 *provider,
                           RenderPolicy::ProviderHandleV1 *out_handle) noexcept {
    if (context != &api_context_token || provider == nullptr ||
        out_handle == nullptr) {
        return Status::invalid_argument;
    }
    const auto owner = internal::currentRegistrationOwner();
    if (owner == internal::engineRegistrationOwner) return Status::wrong_owner;
    if (!internal::isRegistrationOwnerCurrent(owner)) return Status::stale_owner;
    try {
        return renderPolicyRegistry().registerDrawSortProvider(*provider, owner,
                                                               *out_handle);
    } catch (const std::bad_alloc &) {
        return Status::out_of_memory;
    } catch (...) {
        return Status::provider_error;
    }
}

Status unregisterProviderApi(
    void *context, RenderPolicy::ProviderHandleV1 handle) noexcept {
    if (context != &api_context_token) return Status::invalid_argument;
    const auto owner = internal::currentRegistrationOwner();
    if (owner == internal::engineRegistrationOwner) return Status::wrong_owner;
    if (!internal::isRegistrationOwnerCurrent(owner)) return Status::stale_owner;
    try {
        return renderPolicyRegistry().unregisterDrawSortProvider(handle, owner);
    } catch (...) {
        return Status::provider_error;
    }
}

} // namespace

namespace RenderPolicy {

Status getApiV1(std::uint32_t client_abi_version, ApiV1 *out_api) noexcept {
    if (out_api == nullptr) return Status::invalid_argument;
    if (out_api->struct_size < sizeof(ApiV1)) return Status::invalid_argument;
    if (out_api->version != descriptorVersionV1) {
        return Status::unsupported_version;
    }
    if (out_api->reserved0 != 0 || out_api->reserved1 != 0) {
        return Status::reserved_not_zero;
    }
    if (client_abi_version != abiVersionV1) {
        return Status::unsupported_version;
    }
    try {
        (void)renderPolicyRegistry();
        auto produced = descriptor<ApiV1>();
        produced.capability_bits = api_provider_registration;
        produced.context = &api_context_token;
        produced.register_provider = registerProviderApi;
        produced.unregister_provider = unregisterProviderApi;
        *out_api = produced;
        return Status::ok;
    } catch (const std::bad_alloc &) {
        return Status::out_of_memory;
    } catch (...) {
        return Status::provider_error;
    }
}

} // namespace RenderPolicy

} // namespace Pelican
