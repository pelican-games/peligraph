#include "../src/core/vkcore/deletionqueue.hpp"
#include "../src/core/vkcore/deferredcallback.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <semaphore>
#include <thread>

namespace Pelican {

namespace {

struct CountingResource {
    int *destroyed = nullptr;
    bool owns = true;

    explicit CountingResource(int &counter) : destroyed{&counter} {}
    CountingResource(const CountingResource &) = delete;
    CountingResource &operator=(const CountingResource &) = delete;
    CountingResource(CountingResource &&other) noexcept : destroyed{other.destroyed}, owns{other.owns} {
        other.owns = false;
    }
    CountingResource &operator=(CountingResource &&) = delete;

    ~CountingResource() {
        if (owns && destroyed != nullptr) {
            ++(*destroyed);
        }
    }
};

} // namespace

TEST_CASE("DeletionQueueCore releases resources after in-flight frames pass", "[deletionqueue]") {
    int wait_idle_count = 0;
    int destroyed = 0;
    DeletionQueueCore queue{2, [&wait_idle_count]() { ++wait_idle_count; }};

    queue.defer(CountingResource{destroyed});
    REQUIRE(queue.pendingCount() == 1);

    queue.beginFrame();
    REQUIRE(destroyed == 0);
    REQUIRE(queue.pendingCount() == 1);

    queue.beginFrame();
    REQUIRE(destroyed == 1);
    REQUIRE(queue.pendingCount() == 0);
    REQUIRE(wait_idle_count == 0);
}

TEST_CASE("DeletionQueueCore releases resources according to their defer frame", "[deletionqueue]") {
    int destroyed_a = 0;
    int destroyed_b = 0;
    DeletionQueueCore queue{2, []() {}};

    queue.defer(CountingResource{destroyed_a});
    queue.beginFrame();
    queue.defer(CountingResource{destroyed_b});

    queue.beginFrame();
    REQUIRE(destroyed_a == 1);
    REQUIRE(destroyed_b == 0);
    REQUIRE(queue.pendingCount() == 1);

    queue.beginFrame();
    REQUIRE(destroyed_b == 1);
    REQUIRE(queue.pendingCount() == 0);
}

TEST_CASE("DeletionQueueCore flushAll releases all pending resources without waiting", "[deletionqueue]") {
    int wait_idle_count = 0;
    int destroyed_a = 0;
    int destroyed_b = 0;
    DeletionQueueCore queue{2, [&wait_idle_count]() { ++wait_idle_count; }};

    queue.defer(CountingResource{destroyed_a});
    queue.defer(CountingResource{destroyed_b});

    queue.flushAll();

    REQUIRE(destroyed_a == 1);
    REQUIRE(destroyed_b == 1);
    REQUIRE(queue.pendingCount() == 0);
    REQUIRE(wait_idle_count == 0);
}

TEST_CASE("DeletionQueueCore destructor waits and flushes pending resources as a safety net", "[deletionqueue]") {
    int wait_idle_count = 0;
    int destroyed = 0;

    {
        DeletionQueueCore queue{2, [&wait_idle_count]() { ++wait_idle_count; }};
        queue.defer(CountingResource{destroyed});
    }

    REQUIRE(wait_idle_count == 1);
    REQUIRE(destroyed == 1);
}

TEST_CASE("DeletionQueueCore teardown drain closes the queue and rejects late resources",
          "[deletionqueue][teardown][fail-fast]") {
    int destroyed = 0;
    DeletionQueueCore queue{2, [] {}};
    queue.defer(CountingResource{destroyed});

    queue.drainForTeardown();

    REQUIRE(destroyed == 1);
    REQUIRE(queue.pendingCount() == 0);
    REQUIRE_FALSE(queue.acceptingResources());
    REQUIRE_THROWS_WITH(
        queue.defer(CountingResource{destroyed}),
        Catch::Matchers::ContainsSubstring("after teardown drain"));
    REQUIRE_THROWS_WITH(queue.beginFrame(),
                        Catch::Matchers::ContainsSubstring("after teardown drain"));
}

TEST_CASE("Deferred callback lease pins its owner until the callback returns",
          "[deletionqueue][teardown][owner-lifetime]") {
    using namespace std::chrono_literals;

    DeferredCallbackLifetime lifetime;
    const auto callback = lifetime.callback();
    std::binary_semaphore callback_entered{0};
    std::binary_semaphore resume_callback{0};
    std::binary_semaphore close_started{0};
    std::binary_semaphore close_finished{0};
    std::atomic_bool callback_acquired = false;

    std::thread callback_thread{[&] {
        const auto lease = callback.acquire();
        callback_acquired = static_cast<bool>(lease);
        callback_entered.release();
        if (lease) resume_callback.acquire();
    }};

    const bool entered = callback_entered.try_acquire_for(2s);
    if (!entered) {
        resume_callback.release();
        callback_thread.join();
        REQUIRE(entered);
    }
    REQUIRE(callback_acquired.load());

    std::thread close_thread{[&] {
        close_started.release();
        lifetime.closeAndWait();
        close_finished.release();
    }};
    close_started.acquire();
    REQUIRE_FALSE(close_finished.try_acquire_for(100ms));

    resume_callback.release();
    callback_thread.join();
    REQUIRE(close_finished.try_acquire_for(2s));
    close_thread.join();
    REQUIRE_FALSE(static_cast<bool>(callback.acquire()));
}

} // namespace Pelican
