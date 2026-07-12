#include "registrationowner.hpp"

#include <atomic>

namespace Pelican::internal {
namespace {
thread_local RegistrationOwner active_owner = engineRegistrationOwner;
std::atomic<RegistrationOwner> next_owner{1};
} // namespace

RegistrationOwner currentRegistrationOwner() noexcept {
    return active_owner;
}

RegistrationOwner allocateRegistrationOwner() noexcept {
    return next_owner.fetch_add(1, std::memory_order_relaxed);
}

ScopedRegistrationOwner::ScopedRegistrationOwner(RegistrationOwner owner) noexcept
    : previous{active_owner} {
    active_owner = owner;
}

ScopedRegistrationOwner::~ScopedRegistrationOwner() {
    active_owner = previous;
}

} // namespace Pelican::internal
