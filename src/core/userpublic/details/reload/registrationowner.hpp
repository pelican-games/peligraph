#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "../../export.hpp"

namespace Pelican::internal {

using RegistrationOwner = std::uint64_t;
inline constexpr RegistrationOwner engineRegistrationOwner = 0;

enum class RegistrationKind : std::uint8_t {
    system,
    component,
    event,
    behavior,
};

class RegistrationToken;
PELICAN_API RegistrationToken acquireRegistrationToken(RegistrationKind kind,
                                                       RegistrationOwner owner,
                                                       std::string_view name);
PELICAN_API bool releaseRegistrationToken(RegistrationToken token,
                                          RegistrationKind kind) noexcept;
PELICAN_API std::vector<RegistrationToken>
registrationTokens(RegistrationOwner owner, RegistrationKind kind);

class RegistrationToken {
    std::uint32_t identity_ = 0;
    std::uint32_t generation_ = 0;

    constexpr RegistrationToken(std::uint32_t identity, std::uint32_t generation) noexcept
        : identity_{identity}, generation_{generation} {}

    friend PELICAN_API RegistrationToken
    acquireRegistrationToken(RegistrationKind kind, RegistrationOwner owner,
                             std::string_view name);
    friend PELICAN_API bool releaseRegistrationToken(
        RegistrationToken token, RegistrationKind kind) noexcept;
    friend PELICAN_API std::vector<RegistrationToken>
    registrationTokens(RegistrationOwner owner, RegistrationKind kind);

  public:
    constexpr RegistrationToken() noexcept = default;
    constexpr explicit operator bool() const noexcept {
        return identity_ != 0 && generation_ != 0;
    }
    friend constexpr bool operator==(RegistrationToken,
                                     RegistrationToken) noexcept = default;
};

PELICAN_API RegistrationOwner currentRegistrationOwner() noexcept;
PELICAN_API RegistrationOwner allocateRegistrationOwner() noexcept;
PELICAN_API void releaseRegistrationOwner(RegistrationOwner owner) noexcept;
PELICAN_API bool isRegistrationOwnerCurrent(RegistrationOwner owner) noexcept;
constexpr std::uint32_t registrationOwnerIdentity(RegistrationOwner owner) noexcept {
    return static_cast<std::uint32_t>(owner);
}
constexpr std::uint32_t registrationOwnerGeneration(RegistrationOwner owner) noexcept {
    return static_cast<std::uint32_t>(owner >> 32U);
}

class PELICAN_API ScopedRegistrationOwner {
    RegistrationOwner previous;

  public:
    explicit ScopedRegistrationOwner(RegistrationOwner owner) noexcept;
    ~ScopedRegistrationOwner();

    ScopedRegistrationOwner(const ScopedRegistrationOwner &) = delete;
    ScopedRegistrationOwner &operator=(const ScopedRegistrationOwner &) = delete;
};

} // namespace Pelican::internal
