#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>

namespace Pelican {

enum class RuntimeTeardownStep {
    wait_idle,
    owner_callbacks,
    physics,
    ecs,
    model_instances,
    deferred_mutations,
    pending_events,
    deletion_queue,
};

inline constexpr std::array runtime_teardown_order{
    RuntimeTeardownStep::wait_idle,
    RuntimeTeardownStep::owner_callbacks,
    RuntimeTeardownStep::physics,
    RuntimeTeardownStep::ecs,
    RuntimeTeardownStep::model_instances,
    RuntimeTeardownStep::deferred_mutations,
    RuntimeTeardownStep::pending_events,
    RuntimeTeardownStep::deletion_queue,
};

constexpr std::string_view runtimeTeardownStepName(RuntimeTeardownStep step) {
    switch (step) {
    case RuntimeTeardownStep::wait_idle: return "wait-idle";
    case RuntimeTeardownStep::owner_callbacks: return "owner-callbacks";
    case RuntimeTeardownStep::physics: return "physics";
    case RuntimeTeardownStep::ecs: return "ecs";
    case RuntimeTeardownStep::model_instances: return "model-instances";
    case RuntimeTeardownStep::deferred_mutations: return "deferred-mutations";
    case RuntimeTeardownStep::pending_events: return "pending-events";
    case RuntimeTeardownStep::deletion_queue: return "deletion-queue";
    }
    return "unknown";
}

// Normative exceptional teardown order. Empty actions represent modules which
// were never initialized before startup failed.
struct RuntimeTeardownActions {
    std::function<void()> wait_idle;
    std::function<void()> owner_callbacks;
    std::function<void()> physics;
    std::function<void()> ecs;
    std::function<void()> model_instances;
    std::function<void()> deferred_mutations;
    std::function<void()> pending_events;
    std::function<void()> deletion_queue;
};

enum class RuntimeTeardownMode {
    runtime_reset,
    terminal_shutdown,
};

void teardownRuntimeNoThrow() noexcept;
void teardownRuntimeNoThrow(const RuntimeTeardownActions &actions) noexcept;

class RuntimeTeardownGuard {
    bool completed = false;
    RuntimeTeardownMode mode = RuntimeTeardownMode::runtime_reset;
    std::optional<RuntimeTeardownActions> actions;

  public:
    RuntimeTeardownGuard() = default;
    explicit RuntimeTeardownGuard(RuntimeTeardownMode teardown_mode)
        : mode{teardown_mode} {}
    explicit RuntimeTeardownGuard(
        RuntimeTeardownActions teardown_actions,
        RuntimeTeardownMode teardown_mode =
            RuntimeTeardownMode::terminal_shutdown)
        : mode{teardown_mode}, actions{std::move(teardown_actions)} {}
    RuntimeTeardownGuard(const RuntimeTeardownGuard &) = delete;
    RuntimeTeardownGuard &operator=(const RuntimeTeardownGuard &) = delete;
    ~RuntimeTeardownGuard();

    void run() noexcept;
};

} // namespace Pelican
