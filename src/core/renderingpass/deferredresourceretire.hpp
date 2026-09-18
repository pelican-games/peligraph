#pragma once

#include "../container.hpp"
#include "../vkcore/deletionqueue.hpp"

#include <utility>

namespace Pelican {

// Retirement helper shared by the frame-graph buffer container and the
// compute task container. Both hand an extracted record to the deletion
// queue when one is still accepting resources, and otherwise let the
// caller's stack destroy it during teardown.
template <typename T>
void deferOrDestroy(T &&resource) noexcept {
    try {
        auto *queue = FastModuleContainer::tryGet<DeletionQueue>();
        if (queue != nullptr &&
            queue->acceptingResources()) {
            queue->defer(std::forward<T>(resource));
        }
    } catch (...) {
        // The moved-from value (or the original value when defer failed)
        // is destroyed by the caller's stack during teardown.
    }
}

} // namespace Pelican
