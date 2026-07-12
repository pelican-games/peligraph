#include "reloadservice.hpp"

#include "../loader/pathresolver.hpp"
#include "reloadgate.hpp"

#include <nlohmann/json.hpp>

namespace Pelican::watch {

ReloadService::~ReloadService() {
    if (watcher_) watcher_->stop();
}

void ReloadService::setup(const PathResolver &resolver) {
    if (watcher_) watcher_->stop();
    std::vector<WatchStore> stores;
    stores.push_back({"project", resolver.projectRoot()});
    for (const auto &store : resolver.stores()) stores.push_back({store.name, store.root});
    auto *gate = &GET_MODULE(ReloadGate);
    watcher_ = std::make_unique<FileWatcher>(std::move(stores), [gate] { return gate->snapshot(); });
    watcher_->start();
}

void ReloadService::applyFrame() {
    if (!watcher_) return;
    // HR0 deliberately has no real resource handler. This actor only proves
    // frame-boundary queue publication and advances the fake live baseline.
    watcher_->applyFrame([](const ReloadRequest &) { return true; });
}

nlohmann::json ReloadService::statusJson() const {
    const auto gate = GET_MODULE(ReloadGate).snapshot();
    if (!watcher_) {
        return {{"state", "disabled"}, {"epoch", gate.epoch},
                {"error", gate.reason.empty() ? nlohmann::json(nullptr) : nlohmann::json(gate.reason)}};
    }
    const auto status = watcher_->status();
    return {{"state", watcherStateName(status.state)}, {"epoch", status.epoch},
            {"error", status.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(status.error)}};
}

} // namespace Pelican::watch

