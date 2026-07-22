#include "renderpolicyregistry.hpp"

#include <algorithm>
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

std::uint32_t nextGeneration(std::uint32_t generation) noexcept {
    if (++generation == 0) ++generation;
    return generation;
}

bool validHandle(RenderPolicy::ProviderHandleV1 handle) noexcept {
    return handle.identity != 0 && handle.generation != 0 &&
           handle.reserved == 0;
}

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

Status stateBatchedSort(
    void *, const RenderPolicy::DrawSortInputV1 *input,
    RenderPolicy::DrawSortKeyV1 *output, std::uint32_t output_capacity,
    std::uint32_t *out_count) noexcept {
    if (input == nullptr || out_count == nullptr ||
        input->struct_size < sizeof(RenderPolicy::DrawSortInputV1) ||
        input->version != RenderPolicy::descriptorVersionV1 ||
        input->reserved0 != 0 || input->reserved1 != 0 ||
        input->reserved2 != 0 || !validPhase(input->target_phase) ||
        !validLogicalView(input->logical_view) ||
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
        constexpr std::uint32_t known_flags = RenderPolicy::item_skinned;
        if (item.material_id < 0 || (item.flags & ~known_flags) != 0 ||
            !validRoutePhase(item.route, item.phase) ||
            (item.view_mask != RenderPolicy::view_third_person &&
             item.view_mask != RenderPolicy::view_first_person &&
             item.view_mask != RenderPolicy::view_both) ||
            item.has_world_bounds > 1 || item.reserved != 0) {
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

struct RenderPolicyRegistry::Impl {
    struct ProviderSlot {
        std::uint32_t generation = 0;
        bool active = false;
        internal::RegistrationOwner owner = internal::engineRegistrationOwner;
        std::uint32_t provider_version = 0;
        std::uint64_t capability_bits = 0;
        std::string name;
        void *context = nullptr;
        RenderPolicy::SortItemsV1Fn sort_items = nullptr;
    };

    mutable std::shared_mutex mutex;
    std::vector<ProviderSlot> providers;
    std::vector<std::uint32_t> free_slots;
    // One generation per reusable RegistrationOwner slot bounds retirement
    // bookkeeping by peak concurrent owners rather than by reload count.
    std::unordered_map<std::uint32_t, std::uint32_t>
        retired_owner_generations;
    internal::RegistrationOwner active_game_owner =
        internal::engineRegistrationOwner;

    bool ownerIsRetired(internal::RegistrationOwner owner) const noexcept {
        const auto found = retired_owner_generations.find(
            internal::registrationOwnerIdentity(owner));
        return found != retired_owner_generations.end() &&
               found->second ==
                   internal::registrationOwnerGeneration(owner);
    }

    void retireOwner(internal::RegistrationOwner owner) {
        retired_owner_generations.insert_or_assign(
            internal::registrationOwnerIdentity(owner),
            internal::registrationOwnerGeneration(owner));
    }
};

RenderPolicyRegistry::RenderPolicyRegistry() : impl_{std::make_unique<Impl>()} {
    constexpr char builtin_name[] = "state_batched_v1";
    auto provider = RenderPolicy::descriptor<RenderPolicy::ProviderV1>();
    provider.capability_bits = RenderPolicy::builtinProviderCapabilitiesV1;
    provider.name_utf8 = builtin_name;
    provider.name_size = static_cast<std::uint32_t>(sizeof(builtin_name) - 1);
    provider.sort_items = stateBatchedSort;
    RenderPolicy::ProviderHandleV1 handle{};
    if (registerDrawSortProvider(provider, internal::engineRegistrationOwner,
                                 handle) != Status::ok) {
        throw std::logic_error("failed to register builtin draw sort provider");
    }
}

RenderPolicyRegistry::~RenderPolicyRegistry() = default;

RenderPolicy::Status RenderPolicyRegistry::registerDrawSortProvider(
    const RenderPolicy::ProviderV1 &provider,
    internal::RegistrationOwner owner,
    RenderPolicy::ProviderHandleV1 &out_handle) noexcept {
    out_handle = {};
    if (!validProvider(provider)) return Status::invalid_argument;
    if (!internal::isRegistrationOwnerCurrent(owner)) return Status::stale_owner;
    try {
        std::unique_lock lock{impl_->mutex};
        // releaseOwner records retirement under this same lock before the DLL
        // is unloaded. This closes the window between registry cleanup and
        // RegistrationOwner invalidation for a late concurrent registration.
        if (impl_->ownerIsRetired(owner)) return Status::stale_owner;
        const std::string_view name{provider.name_utf8, provider.name_size};
        if (std::any_of(
                impl_->providers.begin(), impl_->providers.end(),
                [&](const Impl::ProviderSlot &registered) {
                    return registered.active && registered.owner == owner &&
                           registered.name == name;
                })) {
            return Status::duplicate_provider;
        }

        std::uint32_t slot_index = 0;
        if (!impl_->free_slots.empty()) {
            slot_index = impl_->free_slots.back();
            impl_->free_slots.pop_back();
        } else {
            if (impl_->providers.size() >=
                std::numeric_limits<std::uint32_t>::max()) {
                return Status::out_of_memory;
            }
            impl_->providers.emplace_back();
            slot_index =
                static_cast<std::uint32_t>(impl_->providers.size() - 1);
        }

        auto &slot = impl_->providers[slot_index];
        slot.generation = nextGeneration(slot.generation);
        slot.active = true;
        slot.owner = owner;
        slot.provider_version = provider.provider_version;
        slot.capability_bits = provider.capability_bits;
        slot.name.assign(provider.name_utf8, provider.name_size);
        slot.context = provider.context;
        slot.sort_items = provider.sort_items;
        out_handle = RenderPolicy::ProviderHandleV1{
            .identity = static_cast<std::uint64_t>(slot_index) + 1,
            .generation = slot.generation,
        };
        return Status::ok;
    } catch (const std::bad_alloc &) {
        return Status::out_of_memory;
    } catch (...) {
        return Status::provider_error;
    }
}

RenderPolicy::Status RenderPolicyRegistry::unregisterDrawSortProvider(
    RenderPolicy::ProviderHandleV1 handle,
    internal::RegistrationOwner owner) noexcept {
    if (!validHandle(handle)) return Status::invalid_argument;
    if (!internal::isRegistrationOwnerCurrent(owner)) return Status::stale_owner;
    std::unique_lock lock{impl_->mutex};
    if (handle.identity > impl_->providers.size()) return Status::stale_provider;
    const auto slot_index = static_cast<std::uint32_t>(handle.identity - 1);
    auto &slot = impl_->providers[slot_index];
    if (!slot.active || slot.generation != handle.generation) {
        return Status::stale_provider;
    }
    if (slot.owner != owner) return Status::wrong_owner;
    slot.active = false;
    slot.owner = internal::engineRegistrationOwner;
    slot.provider_version = 0;
    slot.capability_bits = 0;
    slot.name.clear();
    slot.context = nullptr;
    slot.sort_items = nullptr;
    impl_->free_slots.push_back(slot_index);
    return Status::ok;
}

DrawSortProviderLease RenderPolicyRegistry::resolveDrawSortProvider(
    std::string_view name) const {
    std::shared_lock lock{impl_->mutex};
    const auto find_for_owner = [&](internal::RegistrationOwner owner)
        -> const Impl::ProviderSlot * {
        const auto found = std::find_if(
            impl_->providers.begin(), impl_->providers.end(),
            [&](const Impl::ProviderSlot &slot) {
                return slot.active && slot.owner == owner && slot.name == name;
            });
        return found == impl_->providers.end() ? nullptr : &*found;
    };

    const Impl::ProviderSlot *selected = nullptr;
    if (impl_->active_game_owner != internal::engineRegistrationOwner) {
        selected = find_for_owner(impl_->active_game_owner);
    }
    if (selected == nullptr) {
        selected = find_for_owner(internal::engineRegistrationOwner);
    }
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
        std::move(lock), selected->context, selected->sort_items);
    return DrawSortProviderLease{std::move(lease), std::move(info)};
}

void RenderPolicyRegistry::activateOwner(
    internal::RegistrationOwner owner) noexcept {
    std::unique_lock lock{impl_->mutex};
    impl_->active_game_owner =
        !impl_->ownerIsRetired(owner) &&
                internal::isRegistrationOwnerCurrent(owner)
            ? owner
            : internal::engineRegistrationOwner;
}

void RenderPolicyRegistry::releaseOwner(
    internal::RegistrationOwner owner) noexcept {
    if (owner == internal::engineRegistrationOwner) return;
    std::unique_lock lock{impl_->mutex};
    impl_->retireOwner(owner);
    if (impl_->active_game_owner == owner) {
        impl_->active_game_owner = internal::engineRegistrationOwner;
    }
    for (std::uint32_t index = 0; index < impl_->providers.size(); ++index) {
        auto &slot = impl_->providers[index];
        if (!slot.active || slot.owner != owner) continue;
        slot.active = false;
        slot.owner = internal::engineRegistrationOwner;
        slot.provider_version = 0;
        slot.capability_bits = 0;
        slot.name.clear();
        slot.context = nullptr;
        slot.sort_items = nullptr;
        impl_->free_slots.push_back(index);
    }
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
