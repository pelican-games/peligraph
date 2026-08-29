#pragma once

#include "../loader/resolvedscene.hpp"
#include "../userpublic/details/reload/registrationowner.hpp"

#include <optional>
#include <span>

namespace Pelican::internal {

// Core-private bridge between the resolved scene and the public behavior
// registry. It accepts a neutral DTO and never scans authored JSON.
void validateBehaviorReloadSources(
    RegistrationOwner active_owner, RegistrationOwner candidate_owner,
    std::optional<std::span<const BehaviorReloadSourceParams>> sources);

} // namespace Pelican::internal
