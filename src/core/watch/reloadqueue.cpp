#include "reloadqueue.hpp"

namespace Pelican::watch {

void ReloadQueue::push(ReloadRequest request) {
    std::scoped_lock lock{mutex_};
    pending_.insert_or_assign(request.key, std::move(request));
}

void ReloadQueue::discardAll() {
    std::scoped_lock lock{mutex_};
    pending_.clear();
}

void ReloadQueue::discardEpoch(std::uint64_t epoch) {
    std::scoped_lock lock{mutex_};
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.epoch == epoch) it = pending_.erase(it);
        else ++it;
    }
}

std::size_t ReloadQueue::size() const {
    std::scoped_lock lock{mutex_};
    return pending_.size();
}

std::vector<ReloadRequest> ReloadQueue::takeForFrame(std::uint64_t epoch) {
    std::scoped_lock lock{mutex_};
    std::vector<ReloadRequest> result;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.epoch == epoch) {
            result.push_back(std::move(it->second));
            it = pending_.erase(it);
        } else {
            it = pending_.erase(it); // stale work can never cross a gate epoch
        }
    }
    return result;
}

} // namespace Pelican::watch
