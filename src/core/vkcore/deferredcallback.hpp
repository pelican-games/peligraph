#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>

namespace Pelican {

// A deferred callback may keep a raw pointer to its module owner only while it
// also holds a Lease. closeAndWait() prevents new leases and waits for every
// callback which already acquired one. This closes the weak_ptr check/use race
// without imposing a global module teardown graph.
class DeferredCallbackLifetime {
    struct State {
        std::mutex mutex;
        std::condition_variable callbacks_finished;
        std::size_t active_callbacks = 0;
        bool accepting_callbacks = true;
    };

  public:
    class Callback;

    class Lease {
        std::shared_ptr<State> state;

        explicit Lease(std::shared_ptr<State> value) noexcept
            : state{std::move(value)} {}
        friend class Callback;

      public:
        Lease() = default;
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        Lease(Lease &&other) noexcept : state{std::move(other.state)} {}
        Lease &operator=(Lease &&) = delete;

        ~Lease() {
            if (!state) return;
            std::unique_lock lock{state->mutex};
            --state->active_callbacks;
            const bool finished = state->active_callbacks == 0;
            lock.unlock();
            if (finished) state->callbacks_finished.notify_all();
        }

        explicit operator bool() const noexcept { return state != nullptr; }
    };

    class Callback {
        std::weak_ptr<State> state;

        explicit Callback(const std::shared_ptr<State> &value) noexcept
            : state{value} {}
        friend class DeferredCallbackLifetime;

      public:
        Callback() = default;

        Lease acquire() const noexcept {
            try {
                auto shared = state.lock();
                if (!shared) return {};
                std::scoped_lock lock{shared->mutex};
                if (!shared->accepting_callbacks) return {};
                ++shared->active_callbacks;
                return Lease{std::move(shared)};
            } catch (...) {
                return {};
            }
        }
    };

  private:
    std::shared_ptr<State> state = std::make_shared<State>();

  public:
    DeferredCallbackLifetime() = default;
    DeferredCallbackLifetime(const DeferredCallbackLifetime &) = delete;
    DeferredCallbackLifetime &operator=(const DeferredCallbackLifetime &) = delete;
    DeferredCallbackLifetime(DeferredCallbackLifetime &&) = delete;
    DeferredCallbackLifetime &operator=(DeferredCallbackLifetime &&) = delete;

    ~DeferredCallbackLifetime() { closeAndWait(); }

    Callback callback() const noexcept { return Callback{state}; }

    void closeAndWait() noexcept {
        auto shared = state;
        if (!shared) return;
        std::unique_lock lock{shared->mutex};
        shared->accepting_callbacks = false;
        shared->callbacks_finished.wait(
            lock, [&] { return shared->active_callbacks == 0; });
        lock.unlock();
        state.reset();
    }
};

} // namespace Pelican
