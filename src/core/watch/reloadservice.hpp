#pragma once

#include "../container.hpp"
#include "filewatcher.hpp"

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

  private:
    std::unique_ptr<FileWatcher> watcher_;
};

} // namespace Pelican::watch

