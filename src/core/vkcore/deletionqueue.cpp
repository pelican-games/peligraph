#include "deletionqueue.hpp"

#include "../log.hpp"
#include "core.hpp"
#include "rendertarget.hpp"

#include <stdexcept>

namespace Pelican {

DeletionQueueCore::DeletionQueueCore(uint32_t in_flight_frames, std::function<void()> wait_idle_hook)
    : in_flight_frames{in_flight_frames}, wait_idle_hook{std::move(wait_idle_hook)} {
    if (this->in_flight_frames == 0) {
        throw std::runtime_error("DeletionQueueCore requires at least one in-flight frame");
    }
    if (!this->wait_idle_hook) {
        throw std::runtime_error("DeletionQueueCore requires a wait idle hook");
    }
}

DeletionQueueCore::~DeletionQueueCore() noexcept {
    accepting = false;
    if (pending.empty()) {
        return;
    }

    if (logger != nullptr) {
        LOG_WARNING(logger, "DeletionQueueCore destroyed with {} pending resources; flushing as safety net",
                    pending.size());
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

void DeletionQueueCore::requireAccepting() const {
    if (!accepting) {
        throw std::logic_error("DeletionQueue cannot accept resources after teardown drain");
    }
    if (draining) {
        throw std::logic_error("DeletionQueue cannot accept resources while callbacks are draining");
    }
}

void DeletionQueueCore::releaseEligible(uint64_t oldest_frame) {
    if (draining) {
        throw std::logic_error("DeletionQueue callbacks are already draining");
    }
    draining = true;
    struct DrainScope {
        bool &draining;
        ~DrainScope() { draining = false; }
    } drain_scope{draining};
    for (auto it = pending.begin(); it != pending.end();) {
        if (it->frame <= oldest_frame) {
            it->resource->release();
            it = pending.erase(it);
        } else {
            ++it;
        }
    }
}

void DeletionQueueCore::beginFrame() {
    requireAccepting();
    ++current_frame;
    if (current_frame < in_flight_frames) {
        return;
    }

    releaseEligible(current_frame - in_flight_frames);
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
    std::vector<PendingResource> draining_resources;
    draining_resources.swap(pending);
    for (auto &item : draining_resources) {
        item.resource->release();
    }
}

void DeletionQueueCore::drainForTeardown() {
    accepting = false;
    flushAll();
}

size_t DeletionQueueCore::pendingCount() const { return pending.size(); }

DeletionQueue::DeletionQueue()
    : core{static_cast<uint32_t>(in_flight_frames_num), []() { GET_MODULE(VulkanManageCore).waitIdle(); }} {
    GET_MODULE(VulkanManageCore);
}

void DeletionQueue::beginFrame() { core.beginFrame(); }

void DeletionQueue::flushAll() { core.flushAll(); }

void DeletionQueue::drainForTeardown() { core.drainForTeardown(); }

} // namespace Pelican
