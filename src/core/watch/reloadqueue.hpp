#pragma once

#include "assetkey.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Pelican::watch {

enum class ReloadKind { modified, removed };

struct ReloadRequest {
    AssetKey key;
    ReloadKind kind = ReloadKind::modified;
    std::optional<std::string> digest;
    std::uint64_t epoch = 0;
};

using ReloadHandler = std::function<bool(const ReloadRequest &)>;

class ReloadQueue {
  public:
    void push(ReloadRequest request);
    void discardAll();
    void discardEpoch(std::uint64_t epoch);
    std::size_t size() const;
    std::vector<ReloadRequest> takeForFrame(std::uint64_t epoch);

  private:
    mutable std::mutex mutex_;
    std::map<AssetKey, ReloadRequest> pending_;
};

} // namespace Pelican::watch

