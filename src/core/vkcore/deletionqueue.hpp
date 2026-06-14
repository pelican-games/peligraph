#pragma once

#include "../container.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace Pelican {

class DeletionQueueCore {
    class DeferredResourceBase {
      public:
        virtual ~DeferredResourceBase() = default;
        virtual void release() = 0;
    };

    template <class T> class DeferredResource final : public DeferredResourceBase {
        std::optional<T> resource;

      public:
        template <class U> explicit DeferredResource(U &&value) : resource{std::forward<U>(value)} {}
        void release() override { resource.reset(); }
    };

    struct PendingResource {
        uint64_t frame;
        std::unique_ptr<DeferredResourceBase> resource;
    };

    uint64_t current_frame = 0;
    uint32_t in_flight_frames;
    std::function<void()> wait_idle_hook;
    std::vector<PendingResource> pending;

    void releaseEligible(uint64_t oldest_frame);

  public:
    DeletionQueueCore(uint32_t in_flight_frames, std::function<void()> wait_idle_hook);
    DeletionQueueCore(const DeletionQueueCore &) = delete;
    DeletionQueueCore &operator=(const DeletionQueueCore &) = delete;
    DeletionQueueCore(DeletionQueueCore &&) noexcept = default;
    DeletionQueueCore &operator=(DeletionQueueCore &&) noexcept = default;
    ~DeletionQueueCore() noexcept;

    template <class T> void defer(T &&resource) {
        using Resource = std::decay_t<T>;
        pending.push_back(PendingResource{
            current_frame,
            std::make_unique<DeferredResource<Resource>>(std::forward<T>(resource)),
        });
    }

    void beginFrame();
    void flushAll();
    size_t pendingCount() const;
};

DECLARE_MODULE(DeletionQueue) {
    DeletionQueueCore core;

  public:
    DeletionQueue();

    template <class T> void defer(T &&resource) { core.defer(std::forward<T>(resource)); }
    void beginFrame();
    void flushAll();
};

} // namespace Pelican
