#pragma once

// Shared ABI plumbing for the render provider registries
// (graph transform / subgraph replacement / render strategy /
// pass implementation). Two layers live here: the pure helpers
// (handle and envelope validation, generation stepping, the v1
// size guard and the Status name table) and ProviderRegistry,
// the owner lifecycle itself -- registration, unregistration,
// owner activation and owner retirement -- which the four
// registries repeated line for line before it was lifted here.
// Each registry keeps only its ABI struct packing and its
// public facade.

#include "../registrationownerhost.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Pelican::provider_registry_common {

// Generation 0 means "no provider", so the counter skips it on
// wrap instead of handing out a handle that validHandle rejects.
inline std::uint32_t nextGeneration(
    std::uint32_t generation) noexcept {
    if (++generation == 0) ++generation;
    return generation;
}

// ProviderHandleV1 has the same shape in every render ABI:
// identity / generation / reserved.
template <typename Handle>
bool validHandle(Handle handle) noexcept {
    return handle.identity != 0 && handle.generation != 0 &&
           handle.reserved == 0;
}

inline bool validByteRange(
    const char *data, std::uint32_t size,
    std::uint32_t maximum) noexcept {
    return data != nullptr && size != 0 && size <= maximum &&
           std::find(data, data + size, '\0') == data + size;
}

// The provider envelope (struct_size, descriptor version, the
// three reserved words, the provider version handshake, the
// capability bits and the name byte range) is identical in
// every render ABI. Only the resolve callback member and the
// ABI's own constants differ, so the registry passes both in.
template <typename Provider, typename Resolve>
bool validProviderEnvelope(
    const Provider &provider, Resolve Provider::*resolve,
    std::uint32_t descriptor_version,
    std::uint32_t provider_version,
    std::uint64_t capability_bits,
    std::uint32_t maximum_name_bytes) noexcept {
    if (provider.struct_size < sizeof(Provider) ||
        provider.version != descriptor_version ||
        provider.reserved0 != 0 || provider.reserved1 != 0 ||
        provider.reserved2 != 0 ||
        provider.provider_version != provider_version ||
        provider.minimum_engine_provider_version >
            provider_version ||
        provider.capability_bits != capability_bits ||
        provider.*resolve == nullptr) {
        return false;
    }
    return validByteRange(
        provider.name_utf8, provider.name_size,
        maximum_name_bytes);
}

// The message reads "<subject> <field> exceeds the v1 ABI size
// limit for <target_noun>'<target>'", where target_noun is empty
// or a qualifier such as "region " or "pass ".
inline std::uint32_t abiSizeChecked(
    std::size_t size, std::string_view field,
    std::string_view subject, std::string_view target_noun,
    std::string_view target) {
    if (size >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            std::string{subject} + " " + std::string{field} +
            " exceeds the v1 ABI size limit for " +
            std::string{target_noun} + "'" +
            std::string{target} + "'");
    }
    return static_cast<std::uint32_t>(size);
}

// The four render provider ABIs number these eleven Status
// values identically (pinned by static_assert next to each
// enum). An ABI that adds an enumerator (RenderPolicy has
// buffer_too_small) needs its own table.
template <typename Status>
const char *statusName(Status status) noexcept {
    switch (status) {
    case Status::ok: return "ok";
    case Status::invalid_argument:
        return "invalid_argument";
    case Status::unsupported_version:
        return "unsupported_version";
    case Status::reserved_not_zero:
        return "reserved_not_zero";
    case Status::duplicate_provider:
        return "duplicate_provider";
    case Status::stale_provider:
        return "stale_provider";
    case Status::wrong_owner: return "wrong_owner";
    case Status::stale_owner: return "stale_owner";
    case Status::provider_error:
        return "provider_error";
    case Status::out_of_memory:
        return "out_of_memory";
    case Status::unavailable: return "unavailable";
    }
    return "unknown_status";
}

// The owner lifecycle every render provider registry shares.
// Traits names the ABI types (Provider / Handle / Status /
// Resolve), the resolve callback member inside Provider, the
// builtin provider this registry seeds itself with, and the
// per-ABI descriptor and envelope validation entry points:
//
//   using Provider = RenderX::ProviderV1;
//   using Handle   = RenderX::ProviderHandleV1;
//   using Status   = RenderX::Status;
//   using Resolve  = RenderX::ResolveXV1Fn;
//   static constexpr auto provider_resolve =
//       &Provider::resolve_x;
//   static constexpr std::string_view builtin_name = ...;
//   static constexpr std::uint64_t
//       builtin_capability_bits = ...;
//   static constexpr Resolve builtin_resolve = ...;
//   static constexpr std::string_view
//       builtin_registration_failure = ...;
//   static Provider descriptor() noexcept;
//   static bool valid(const Provider &) noexcept;
//
// Retirement is the hot-reload safety rule and is identical in
// every registry: releaseOwner records the retirement under the
// same exclusive lock that clears the owner's slots, before the
// game DLL is unloaded, so a registration racing the unload is
// rejected with stale_owner instead of landing in a slot that
// outlives its code.
template <typename Traits>
struct ProviderRegistry {
    using Provider = typename Traits::Provider;
    using Handle = typename Traits::Handle;
    using Status = typename Traits::Status;
    using Resolve = typename Traits::Resolve;

    struct ProviderSlot {
        std::uint32_t generation = 0;
        bool active = false;
        internal::RegistrationOwner owner =
            internal::engineRegistrationOwner;
        std::uint32_t provider_version = 0;
        std::uint64_t capability_bits = 0;
        std::string name;
        void *context = nullptr;
        Resolve resolve = nullptr;
    };

    mutable std::shared_mutex mutex;
    std::vector<ProviderSlot> providers;
    std::vector<std::uint32_t> free_slots;
    // One generation per reusable RegistrationOwner slot bounds
    // retirement bookkeeping by peak concurrent owners rather
    // than by reload count.
    std::unordered_map<std::uint32_t, std::uint32_t>
        retired_owner_generations;
    internal::RegistrationOwner active_game_owner =
        internal::engineRegistrationOwner;

    bool ownerIsRetired(
        internal::RegistrationOwner owner) const noexcept {
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

    // Seeds the engine-owned builtin provider. The owning
    // registry's constructor is the only place a registry may
    // fail to come up, so this throws rather than returning.
    void registerBuiltinProvider() {
        auto provider = Traits::descriptor();
        provider.capability_bits =
            Traits::builtin_capability_bits;
        provider.name_utf8 = Traits::builtin_name.data();
        provider.name_size = static_cast<std::uint32_t>(
            Traits::builtin_name.size());
        provider.*(Traits::provider_resolve) =
            Traits::builtin_resolve;
        Handle handle{};
        if (registerProvider(
                provider, internal::engineRegistrationOwner,
                handle) != Status::ok) {
            throw std::logic_error(std::string{
                Traits::builtin_registration_failure});
        }
    }

    Status registerProvider(
        const Provider &provider,
        internal::RegistrationOwner owner,
        Handle &out_handle) noexcept {
        out_handle = {};
        if (!Traits::valid(provider)) {
            return Status::invalid_argument;
        }
        if (!internal::isRegistrationOwnerCurrent(owner)) {
            return Status::stale_owner;
        }
        try {
            std::unique_lock lock{mutex};
            // releaseOwner records retirement under this same
            // lock before the DLL is unloaded. This closes the
            // window between registry cleanup and
            // RegistrationOwner invalidation for a late
            // concurrent registration.
            if (ownerIsRetired(owner)) {
                return Status::stale_owner;
            }
            const std::string_view name{
                provider.name_utf8, provider.name_size};
            if (std::any_of(
                    providers.begin(), providers.end(),
                    [&](const ProviderSlot &registered) {
                        return registered.active &&
                               registered.owner == owner &&
                               registered.name == name;
                    })) {
                return Status::duplicate_provider;
            }

            std::uint32_t slot_index = 0;
            if (!free_slots.empty()) {
                slot_index = free_slots.back();
                free_slots.pop_back();
            } else {
                if (providers.size() >=
                    std::numeric_limits<
                        std::uint32_t>::max()) {
                    return Status::out_of_memory;
                }
                providers.emplace_back();
                slot_index = static_cast<std::uint32_t>(
                    providers.size() - 1);
            }

            auto &slot = providers[slot_index];
            slot.generation = nextGeneration(slot.generation);
            slot.active = true;
            slot.owner = owner;
            slot.provider_version = provider.provider_version;
            slot.capability_bits = provider.capability_bits;
            slot.name.assign(
                provider.name_utf8, provider.name_size);
            slot.context = provider.context;
            slot.resolve = provider.*(Traits::provider_resolve);
            out_handle = Handle{
                .identity =
                    static_cast<std::uint64_t>(slot_index) + 1,
                .generation = slot.generation,
            };
            return Status::ok;
        } catch (const std::bad_alloc &) {
            return Status::out_of_memory;
        } catch (...) {
            return Status::provider_error;
        }
    }

    Status unregisterProvider(
        Handle handle,
        internal::RegistrationOwner owner) noexcept {
        if (!validHandle(handle)) {
            return Status::invalid_argument;
        }
        if (!internal::isRegistrationOwnerCurrent(owner)) {
            return Status::stale_owner;
        }
        std::unique_lock lock{mutex};
        if (handle.identity > providers.size()) {
            return Status::stale_provider;
        }
        const auto index =
            static_cast<std::uint32_t>(handle.identity - 1);
        auto &slot = providers[index];
        if (!slot.active ||
            slot.generation != handle.generation) {
            return Status::stale_provider;
        }
        if (slot.owner != owner) {
            return Status::wrong_owner;
        }
        clearSlot(slot);
        free_slots.push_back(index);
        return Status::ok;
    }

    void activateOwner(
        internal::RegistrationOwner owner) noexcept {
        std::unique_lock lock{mutex};
        active_game_owner =
            !ownerIsRetired(owner) &&
                    internal::isRegistrationOwnerCurrent(owner)
                ? owner
                : internal::engineRegistrationOwner;
    }

    void releaseOwner(
        internal::RegistrationOwner owner) noexcept {
        if (owner == internal::engineRegistrationOwner) {
            return;
        }
        std::unique_lock lock{mutex};
        retireOwner(owner);
        if (active_game_owner == owner) {
            active_game_owner =
                internal::engineRegistrationOwner;
        }
        for (std::uint32_t index = 0;
             index < providers.size(); ++index) {
            auto &slot = providers[index];
            if (!slot.active || slot.owner != owner) {
                continue;
            }
            clearSlot(slot);
            free_slots.push_back(index);
        }
    }

    // A game owner's providers shadow the engine's under the
    // same name; the engine builtin is the fallback. The caller
    // holds the registry lock -- directly or through a snapshot
    // -- for as long as it uses the returned slot.
    const ProviderSlot *findActiveProvider(
        std::string_view name) const noexcept {
        const auto find_for_owner =
            [&](internal::RegistrationOwner owner)
            -> const ProviderSlot * {
            const auto found = std::find_if(
                providers.begin(), providers.end(),
                [&](const ProviderSlot &slot) {
                    return slot.active && slot.owner == owner &&
                           slot.name == name;
                });
            return found == providers.end() ? nullptr : &*found;
        };
        const ProviderSlot *selected = nullptr;
        if (active_game_owner !=
            internal::engineRegistrationOwner) {
            selected = find_for_owner(active_game_owner);
        }
        if (selected == nullptr) {
            selected = find_for_owner(
                internal::engineRegistrationOwner);
        }
        return selected;
    }

    // EL-1: the names a request may give -- each slot findActiveProvider
    // resolves its own name to -- sorted by name. A game provider that
    // shadows the engine's under one name is listed; the shadowed engine
    // slot is not. The caller holds the registry lock, as for
    // findActiveProvider.
    std::vector<const ProviderSlot *> visibleProviders() const {
        std::vector<const ProviderSlot *> result;
        for (const auto &slot : providers) {
            if (slot.active && findActiveProvider(slot.name) == &slot) {
                result.push_back(&slot);
            }
        }
        std::sort(result.begin(), result.end(),
                  [](const ProviderSlot *left, const ProviderSlot *right) {
                      return left->name < right->name;
                  });
        return result;
    }

  private:
    static void clearSlot(ProviderSlot &slot) noexcept {
        slot.active = false;
        slot.owner = internal::engineRegistrationOwner;
        slot.provider_version = 0;
        slot.capability_bits = 0;
        slot.name.clear();
        slot.context = nullptr;
        slot.resolve = nullptr;
    }
};

} // namespace Pelican::provider_registry_common
