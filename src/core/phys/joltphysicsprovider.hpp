#pragma once

#include "../userpublic/physics/abi_v1.hpp"

namespace Pelican::physics_internal {

[[nodiscard]] const Physics::ProviderV1 &joltProviderV1() noexcept;

} // namespace Pelican::physics_internal
