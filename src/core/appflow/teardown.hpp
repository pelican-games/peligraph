#pragma once

namespace Pelican {

void teardownRuntimeNoThrow() noexcept;

class RuntimeTeardownGuard {
    bool completed = false;

  public:
    RuntimeTeardownGuard() = default;
    RuntimeTeardownGuard(const RuntimeTeardownGuard &) = delete;
    RuntimeTeardownGuard &operator=(const RuntimeTeardownGuard &) = delete;
    ~RuntimeTeardownGuard();

    void run() noexcept;
};

} // namespace Pelican
