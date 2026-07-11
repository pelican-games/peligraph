#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace Pelican {

// Work completion is intentionally hidden behind an index-ordered result
// vector. Callers commit results in declaration order after every CPU job has
// joined, so scheduling cannot leak into ECS or GPU registration order.
template <class Result, class Prepare>
std::vector<Result> parallelPrepareOrdered(std::size_t count, Prepare &&prepare,
                                           std::size_t requested_workers = 0) {
    if (count == 0) {
        return {};
    }

    const auto hardware_workers = std::max(1u, std::thread::hardware_concurrency());
    const auto desired_workers = requested_workers == 0 ? static_cast<std::size_t>(hardware_workers)
                                                         : requested_workers;
    const auto worker_count = std::max<std::size_t>(1, std::min(count, desired_workers));
    std::vector<std::optional<Result>> slots(count);
    std::atomic_size_t next_index{0};
    std::atomic_bool cancelled{false};
    std::exception_ptr first_error;
    std::mutex error_mutex;

    const auto worker = [&] {
        while (!cancelled.load(std::memory_order_relaxed)) {
            const auto index = next_index.fetch_add(1, std::memory_order_relaxed);
            if (index >= count) {
                break;
            }
            try {
                slots[index].emplace(std::invoke(prepare, index));
            } catch (...) {
                {
                    std::lock_guard lock{error_mutex};
                    if (!first_error) {
                        first_error = std::current_exception();
                    }
                }
                cancelled.store(true, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
        workers.emplace_back(worker);
    }
    workers.clear(); // jthread joins here before results or errors are observed.

    if (first_error) {
        std::rethrow_exception(first_error);
    }

    std::vector<Result> results;
    results.reserve(count);
    for (auto &slot : slots) {
        results.push_back(std::move(*slot));
    }
    return results;
}

} // namespace Pelican
