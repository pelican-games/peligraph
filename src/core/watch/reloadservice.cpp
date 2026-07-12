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
    stores.push_back({"project", resolver.projectRoot(), {}});
    for (const auto &store : resolver.stores()) stores.push_back({store.name, store.root, store.mount});
    auto *gate = &GET_MODULE(ReloadGate);
    watcher_ = std::make_unique<FileWatcher>(std::move(stores), [gate] { return gate->snapshot(); });
    watcher_->start();
}

void ReloadService::applyFrame() {
    if (watcher_) {
        // Real handlers are attached by HR1-T/M and HR2. Until then the HR0
        // watcher actor only advances its digest baseline.
        watcher_->applyFrame([](const ReloadRequest &) { return true; });
    }
    transactions_.applyFrame();
}

nlohmann::json ReloadService::statusJson() const {
    const auto gate = GET_MODULE(ReloadGate).snapshot();
    const auto resources = transactions_.status();
    auto resource_error = nlohmann::json(nullptr);
    if (resources.last_reload_error) {
        resource_error = {{"path", resources.last_reload_error->path},
                          {"kind", resources.last_reload_error->kind},
                          {"message", resources.last_reload_error->message}};
    }
    if (!watcher_) {
        return {{"state", "disabled"}, {"epoch", gate.epoch},
                {"error", gate.reason.empty() ? nlohmann::json(nullptr) : nlohmann::json(gate.reason)},
                {"applied", resources.applied}, {"failed", resources.failed},
                {"last_reload_error", std::move(resource_error)}};
    }
    const auto status = watcher_->status();
    return {{"state", watcherStateName(status.state)}, {"epoch", status.epoch},
            {"error", status.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(status.error)},
            {"applied", resources.applied}, {"failed", resources.failed},
            {"last_reload_error", std::move(resource_error)}};
}

} // namespace Pelican::watch

