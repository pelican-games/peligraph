#pragma once

#include <cstdint>
#include "../../export.hpp"

namespace Pelican::internal {

using RegistrationOwner = std::uint64_t;
inline constexpr RegistrationOwner engineRegistrationOwner = 0;

PELICAN_API RegistrationOwner currentRegistrationOwner() noexcept;
PELICAN_API RegistrationOwner allocateRegistrationOwner() noexcept;

class PELICAN_API ScopedRegistrationOwner {
    RegistrationOwner previous;

  public:
    explicit ScopedRegistrationOwner(RegistrationOwner owner) noexcept;
    ~ScopedRegistrationOwner();

    ScopedRegistrationOwner(const ScopedRegistrationOwner &) = delete;
    ScopedRegistrationOwner &operator=(const ScopedRegistrationOwner &) = delete;
};

} // namespace Pelican::internal
