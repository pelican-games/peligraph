#pragma once

// std::atomic<std::shared_ptr<T>> where the standard library has it (C++20,
// P0718: MSVC's STL, libstdc++), and the same load/store surface over a mutex
// where it does not yet (libc++ 18 of NDK r27c). With the library type the
// alias is exactly std::atomic<std::shared_ptr<T>>.

#include <atomic>
#include <memory>
#include <version>

#if !defined(__cpp_lib_atomic_shared_ptr)
#include <cstddef>
#include <mutex>
#include <utility>
#endif

namespace Pelican {

#if defined(__cpp_lib_atomic_shared_ptr)

template <class T>
using AtomicSharedPtr = std::atomic<std::shared_ptr<T>>;

#else

template <class T>
class AtomicSharedPtr {
    mutable std::mutex mutex_;
    std::shared_ptr<T> value_;

  public:
    constexpr AtomicSharedPtr() noexcept = default;
    constexpr AtomicSharedPtr(std::nullptr_t) noexcept {}
    AtomicSharedPtr(std::shared_ptr<T> desired) noexcept : value_{std::move(desired)} {}
    AtomicSharedPtr(const AtomicSharedPtr &) = delete;
    AtomicSharedPtr &operator=(const AtomicSharedPtr &) = delete;

    std::shared_ptr<T> load(std::memory_order = std::memory_order_seq_cst) const noexcept {
        std::scoped_lock lock{mutex_};
        return value_;
    }

    void store(std::shared_ptr<T> desired, std::memory_order = std::memory_order_seq_cst) noexcept {
        std::scoped_lock lock{mutex_};
        // The previous value moves into the parameter and is released after
        // the lock, so a destructor never runs under it.
        value_.swap(desired);
    }

    std::shared_ptr<T> exchange(std::shared_ptr<T> desired,
                                std::memory_order = std::memory_order_seq_cst) noexcept {
        std::scoped_lock lock{mutex_};
        value_.swap(desired);
        return desired;
    }

    operator std::shared_ptr<T>() const noexcept { return load(); }
};

#endif

} // namespace Pelican
