#include "registrationowner.hpp"

#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace Pelican::internal {
namespace {
thread_local RegistrationOwner active_owner = engineRegistrationOwner;

struct OwnerSlot {
    std::uint32_t generation = 0;
    bool active = false;
};

struct TokenSlot {
    std::uint32_t generation = 0;
    bool active = false;
    RegistrationKind kind = RegistrationKind::system;
    RegistrationOwner owner = engineRegistrationOwner;
    std::string name;
};

struct RegistrationState {
    std::mutex mutex;
    std::vector<OwnerSlot> owners;
    std::vector<std::uint32_t> free_owners;
    std::vector<TokenSlot> tokens;
    std::vector<std::uint32_t> free_tokens;
};

RegistrationState &state() {
    // Registration callbacks are used by process and DLL static objects. Keep
    // the ledger alive through static destruction in either module.
    static auto *value = new RegistrationState;
    return *value;
}

std::uint32_t nextGeneration(std::uint32_t generation) {
    if (++generation == 0) ++generation;
    return generation;
}

RegistrationOwner makeOwner(std::uint32_t identity,
                            std::uint32_t generation) noexcept {
    return (static_cast<RegistrationOwner>(generation) << 32U) | identity;
}

bool ownerIsCurrentLocked(const RegistrationState &value,
                          RegistrationOwner owner) noexcept {
    if (owner == engineRegistrationOwner) return true;
    const auto generation = registrationOwnerGeneration(owner);
    // Numeric owners predate the generation allocator and remain accepted by
    // focused fixtures and internal services. Allocated DLL owners always have
    // a non-zero generation and receive strict stale-handle validation.
    if (generation == 0) return true;
    const auto identity = registrationOwnerIdentity(owner);
    if (identity == 0 || identity > value.owners.size()) return false;
    const auto &slot = value.owners[identity - 1];
    return slot.active && slot.generation == generation;
}
} // namespace

RegistrationOwner currentRegistrationOwner() noexcept {
    return active_owner;
}

RegistrationOwner allocateRegistrationOwner() noexcept {
    auto &value = state();
    std::scoped_lock lock{value.mutex};
    std::uint32_t identity = 0;
    if (!value.free_owners.empty()) {
        identity = value.free_owners.back();
        value.free_owners.pop_back();
    } else {
        if (value.owners.size() == std::numeric_limits<std::uint32_t>::max()) {
            std::terminate();
        }
        value.owners.emplace_back();
        identity = static_cast<std::uint32_t>(value.owners.size());
    }
    auto &slot = value.owners[identity - 1];
    slot.generation = nextGeneration(slot.generation);
    slot.active = true;
    return makeOwner(identity, slot.generation);
}

void releaseRegistrationOwner(RegistrationOwner owner) noexcept {
    if (owner == engineRegistrationOwner ||
        registrationOwnerGeneration(owner) == 0) {
        return;
    }
    auto &value = state();
    std::scoped_lock lock{value.mutex};
    const auto identity = registrationOwnerIdentity(owner);
    if (!ownerIsCurrentLocked(value, owner)) return;
    for (const auto &token : value.tokens) {
        if (token.active && token.owner == owner) return;
    }
    value.owners[identity - 1].active = false;
    value.free_owners.push_back(identity);
}

bool isRegistrationOwnerCurrent(RegistrationOwner owner) noexcept {
    auto &value = state();
    std::scoped_lock lock{value.mutex};
    return ownerIsCurrentLocked(value, owner);
}

RegistrationToken acquireRegistrationToken(RegistrationKind kind,
                                           RegistrationOwner owner,
                                           std::string_view name) {
    auto &value = state();
    std::scoped_lock lock{value.mutex};
    if (!ownerIsCurrentLocked(value, owner)) {
        throw std::runtime_error("stale registration owner rejected while registering '" +
                                 std::string{name} + "'");
    }

    std::uint32_t identity = 0;
    if (!value.free_tokens.empty()) {
        identity = value.free_tokens.back();
        value.free_tokens.pop_back();
    } else {
        if (value.tokens.size() == std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("registration token capacity exhausted while registering '" +
                                     std::string{name} + "'");
        }
        value.tokens.emplace_back();
        identity = static_cast<std::uint32_t>(value.tokens.size());
    }
    auto &slot = value.tokens[identity - 1];
    slot.generation = nextGeneration(slot.generation);
    slot.active = true;
    slot.kind = kind;
    slot.owner = owner;
    slot.name.assign(name);
    return RegistrationToken{identity, slot.generation};
}

bool releaseRegistrationToken(RegistrationToken token,
                              RegistrationKind kind) noexcept {
    if (!token) return false;
    auto &value = state();
    std::scoped_lock lock{value.mutex};
    if (token.identity_ > value.tokens.size()) return false;
    auto &slot = value.tokens[token.identity_ - 1];
    if (!slot.active || slot.generation != token.generation_ ||
        slot.kind != kind) {
        return false;
    }
    slot.active = false;
    slot.owner = engineRegistrationOwner;
    slot.name.clear();
    value.free_tokens.push_back(token.identity_);
    return true;
}

std::vector<RegistrationToken>
registrationTokens(RegistrationOwner owner, RegistrationKind kind) {
    auto &value = state();
    std::scoped_lock lock{value.mutex};
    std::vector<RegistrationToken> result;
    for (std::size_t i = value.tokens.size(); i != 0; --i) {
        const auto &slot = value.tokens[i - 1];
        if (slot.active && slot.owner == owner && slot.kind == kind) {
            result.push_back(
                RegistrationToken{static_cast<std::uint32_t>(i), slot.generation});
        }
    }
    return result;
}

ScopedRegistrationOwner::ScopedRegistrationOwner(RegistrationOwner owner) noexcept
    : previous{active_owner} {
    active_owner = owner;
}

ScopedRegistrationOwner::~ScopedRegistrationOwner() {
    active_owner = previous;
}

} // namespace Pelican::internal
