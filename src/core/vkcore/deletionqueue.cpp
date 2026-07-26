#include "deletionqueue.hpp"

#include "../log.hpp"
#include "core.hpp"
#include "rendertarget.hpp"

#include <stdexcept>

namespace Pelican {

DeletionQueueCore::DeletionQueueCore(
    std::function<void()> wait_idle_hook)
    : wait_idle_hook{std::move(wait_idle_hook)},
      pending_counter{std::make_shared<PendingCounter>()},
      current_batch{std::make_shared<RetirementBatch>()} {
    if (!this->wait_idle_hook) {
        throw std::runtime_error("DeletionQueueCore requires a wait idle hook");
    }
    batches.push_back(current_batch);
}

DeletionQueueCore::~DeletionQueueCore() noexcept {
    accepting = false;
    if (pendingCount() == 0) {
        return;
    }

    if (logger != nullptr) {
        LOG_WARNING(logger, "DeletionQueueCore destroyed with {} pending resources; flushing as safety net",
                    pendingCount());
    }

    try {
        wait_idle_hook();
        flushAll();
    } catch (const std::exception &e) {
        if (logger != nullptr) {
            LOG_ERROR(logger, "DeletionQueueCore safety flush failed: {}", e.what());
        }
    } catch (...) {
        if (logger != nullptr) {
            LOG_ERROR(logger, "DeletionQueueCore safety flush failed with an unknown exception");
        }
    }
}

void DeletionQueueCore::RetirementBatch::releaseAll() {
    for (auto &resource : resources) {
        resource->release();
    }
    resources.clear();
}

void DeletionQueueCore::requireAccepting() const {
    if (!accepting) {
        throw std::logic_error("DeletionQueue cannot accept resources after teardown drain");
    }
    if (draining) {
        throw std::logic_error("DeletionQueue cannot accept resources while callbacks are draining");
    }
}

GpuSubmissionLease DeletionQueueCore::leaseForNextSubmission(
    GpuSubmissionLease upstream) {
    requireAccepting();
    return std::make_shared<SubmissionLeaseBundle>(
        SubmissionLeaseBundle{
            std::move(upstream),
            current_batch,
        });
}

void DeletionQueueCore::confirmSubmission() {
    requireAccepting();
    ++completed_submissions;
    if (current_batch->empty()) return;

    auto next_batch = std::make_shared<RetirementBatch>();
    batches.push_back(next_batch);
    current_batch = std::move(next_batch);
}

void DeletionQueueCore::flushAll() {
    if (draining) {
        throw std::logic_error("DeletionQueue callbacks are already draining");
    }
    draining = true;
    struct DrainScope {
        bool &draining;
        ~DrainScope() { draining = false; }
    } drain_scope{draining};

    for (auto it = batches.begin(); it != batches.end();) {
        if (auto batch = it->lock()) {
            batch->releaseAll();
            ++it;
        } else {
            it = batches.erase(it);
        }
    }
}

void DeletionQueueCore::drainForTeardown() {
    accepting = false;
    flushAll();
}

size_t DeletionQueueCore::pendingCount() const {
    return pending_counter->count;
}

DeletionQueue::DeletionQueue()
    : core{[]() { GET_MODULE(VulkanManageCore).waitIdle(); }} {
    GET_MODULE(VulkanManageCore);
}

GpuSubmissionLease DeletionQueue::leaseForNextSubmission(
    GpuSubmissionLease upstream) {
    return core.leaseForNextSubmission(std::move(upstream));
}

void DeletionQueue::confirmSubmission() {
    core.confirmSubmission();
}

void DeletionQueue::flushAll() { core.flushAll(); }

void DeletionQueue::drainForTeardown() { core.drainForTeardown(); }

} // namespace Pelican
