#pragma once

namespace Pelican {

class RuntimeTeardownGuard;

namespace internal {

// The player and lifecycle acceptance share this exact terminal ordering.
void runTerminalShutdown(RuntimeTeardownGuard &teardown) noexcept;

} // namespace internal
} // namespace Pelican
