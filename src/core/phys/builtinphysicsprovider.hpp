#pragma once

#include "../userpublic/physics/abi_v1.hpp"

namespace Pelican::physics_internal {

[[nodiscard]] const Physics::ProviderV1 &builtinProviderV1() noexcept;

} // namespace Pelican::physics_internal
