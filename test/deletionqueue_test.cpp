#include "../src/core/vkcore/deletionqueue.hpp"

#include <catch2/catch_test_macros.hpp>

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

} // namespace Pelican
