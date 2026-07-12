#pragma once

#include "../container.hpp"
#include "filewatcher.hpp"
#include "reloadtransaction.hpp"

#include <memory>
#include <nlohmann/json_fwd.hpp>

namespace Pelican { class PathResolver; }

namespace Pelican::watch {

DECLARE_MODULE(ReloadService) {
  public:
    ReloadService() = default;
    ~ReloadService();
    void setup(const PathResolver &resolver);
    void applyFrame();
    nlohmann::json statusJson() const;
    FileWatcher *watcherForTesting() noexcept { return watcher_.get(); }
    ReloadCoordinator &transactions() noexcept { return transactions_; }
    bool applyRequestForTesting(const ReloadRequest &request);

  private:
    ReloadCoordinator::RetireSink retireSink();
    bool applyRequest(const ReloadRequest &request);
    std::unique_ptr<FileWatcher> watcher_;
    ReloadCoordinator transactions_;
};

} // namespace Pelican::watch

