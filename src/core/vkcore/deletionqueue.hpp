#pragma once

#include "../container.hpp"
#include "frametarget.hpp"

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
    struct PendingCounter {
        std::size_t count = 0;
    };

    class DeferredResourceBase {
      public:
        virtual ~DeferredResourceBase() = default;
        virtual void release() = 0;
    };

    template <class T> class DeferredResource final : public DeferredResourceBase {
        std::optional<T> resource;
        std::shared_ptr<PendingCounter> pending_counter;

      public:
        template <class U>
        DeferredResource(U &&value,
                         std::shared_ptr<PendingCounter> counter)
            : resource{std::forward<U>(value)},
              pending_counter{std::move(counter)} {
            ++pending_counter->count;
        }

        ~DeferredResource() override { release(); }

        void release() override {
            if (!resource) return;
            resource.reset();
            --pending_counter->count;
        }
    };

    class RetirementBatch {
        std::vector<std::unique_ptr<DeferredResourceBase>> resources;

      public:
        template <class T>
        void add(T &&resource,
                 const std::shared_ptr<PendingCounter> &counter) {
            using Resource = std::decay_t<T>;
            resources.push_back(
                std::make_unique<DeferredResource<Resource>>(
                    std::forward<T>(resource), counter));
        }

        bool empty() const noexcept { return resources.empty(); }
        void releaseAll();
    };

    struct SubmissionLeaseBundle {
        GpuSubmissionLease upstream;
        std::shared_ptr<RetirementBatch> retirement;
    };

    uint64_t completed_submissions = 0;
    std::function<void()> wait_idle_hook;
    std::shared_ptr<PendingCounter> pending_counter;
    std::shared_ptr<RetirementBatch> current_batch;
    std::vector<std::weak_ptr<RetirementBatch>> batches;
    bool accepting = true;
    bool draining = false;

    void requireAccepting() const;

  public:
    explicit DeletionQueueCore(std::function<void()> wait_idle_hook);
    DeletionQueueCore(const DeletionQueueCore &) = delete;
    DeletionQueueCore &operator=(const DeletionQueueCore &) = delete;
    DeletionQueueCore(DeletionQueueCore &&) = delete;
    DeletionQueueCore &operator=(DeletionQueueCore &&) = delete;
    ~DeletionQueueCore() noexcept;

    template <class T> void defer(T &&resource) {
        requireAccepting();
        current_batch->add(std::forward<T>(resource), pending_counter);
    }

    // Bind the current retirement batch to a real GPU submission. The target
    // must retain the returned lease until its completion fence is observed.
    GpuSubmissionLease leaseForNextSubmission(GpuSubmissionLease upstream);
    // Call only after that submission has succeeded and retained the lease.
    // Deferred resources then move to a fresh batch for the next submission.
    void confirmSubmission();
    void flushAll();
    void drainForTeardown();
    size_t pendingCount() const;
    uint64_t currentFrame() const { return completed_submissions; }
    bool acceptingResources() const noexcept { return accepting; }
};

DECLARE_MODULE(DeletionQueue) {
    DeletionQueueCore core;

  public:
    DeletionQueue();

    template <class T> void defer(T &&resource) { core.defer(std::forward<T>(resource)); }
    GpuSubmissionLease leaseForNextSubmission(
        GpuSubmissionLease upstream = {});
    void confirmSubmission();
    void flushAll();
    void drainForTeardown();
    bool acceptingResources() const noexcept {
        return core.acceptingResources();
    }
    size_t pendingCountForTesting() const { return core.pendingCount(); }
    uint64_t currentFrameForTesting() const { return core.currentFrame(); }
    bool acceptingResourcesForTesting() const {
        return acceptingResources();
    }
};

} // namespace Pelican
