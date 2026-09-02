#pragma once

#include "userpublic/details/reload/registrationowner.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Pelican::internal {

// Allocation and activation are host capabilities.  Game DLLs only observe
// the owner selected by the loader through currentRegistrationOwner().
RegistrationOwner allocateRegistrationOwner() noexcept;

enum class RegistrationOwnerReleaseResult : std::uint8_t {
    released,
    pending_registrations,
    stale_owner,
    unmanaged_owner,
};

RegistrationOwnerReleaseResult
releaseRegistrationOwner(RegistrationOwner owner) noexcept;
constexpr std::string_view registrationOwnerReleaseResultName(
    RegistrationOwnerReleaseResult result) noexcept {
    switch (result) {
    case RegistrationOwnerReleaseResult::released:
        return "registration_owner_released@1";
    case RegistrationOwnerReleaseResult::pending_registrations:
        return "registration_owner_release_pending@1";
    case RegistrationOwnerReleaseResult::stale_owner:
        return "registration_owner_release_stale@1";
    case RegistrationOwnerReleaseResult::unmanaged_owner:
        return "registration_owner_release_unmanaged@1";
    }
    return "registration_owner_release_unknown@1";
}

bool isRegistrationOwnerCurrent(RegistrationOwner owner) noexcept;
std::size_t registrationOwnerCount() noexcept;

constexpr std::uint32_t
registrationOwnerIdentity(RegistrationOwner owner) noexcept {
    return static_cast<std::uint32_t>(owner);
}
constexpr std::uint32_t
registrationOwnerGeneration(RegistrationOwner owner) noexcept {
    return static_cast<std::uint32_t>(owner >> 32U);
}

class ScopedRegistrationOwner {
    RegistrationOwner previous;

  public:
    explicit ScopedRegistrationOwner(RegistrationOwner owner) noexcept;
    ~ScopedRegistrationOwner();

    ScopedRegistrationOwner(const ScopedRegistrationOwner &) = delete;
    ScopedRegistrationOwner &operator=(const ScopedRegistrationOwner &) = delete;
};

} // namespace Pelican::internal

namespace Pelican::TestSupport {

// Test and benchmark component registrations are external by construction.
// A release requested before the module container drops its tokens is marked
// pending and is completed by the final token release.
class ScopedTestRegistrationOwner final {
    internal::RegistrationOwner owner_ =
        internal::allocateRegistrationOwner();
    internal::ScopedRegistrationOwner scope_{owner_};

  public:
    ScopedTestRegistrationOwner() = default;
    ~ScopedTestRegistrationOwner() {
        (void)internal::releaseRegistrationOwner(owner_);
    }

    ScopedTestRegistrationOwner(const ScopedTestRegistrationOwner &) = delete;
    ScopedTestRegistrationOwner &operator=(
        const ScopedTestRegistrationOwner &) = delete;

    internal::RegistrationOwner owner() const noexcept { return owner_; }
};

} // namespace Pelican::TestSupport
