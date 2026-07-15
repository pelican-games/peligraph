#pragma once

#include "../userpublic/physics/abi_v2.hpp"

namespace Pelican::physics_internal {

[[nodiscard]] const Physics::ProviderV2 &builtinProviderV2() noexcept;

} // namespace Pelican::physics_internal
