#pragma once

#include "../container.hpp"

#include <cstdint>
#include <mutex>
#include <string>

namespace Pelican { class EngineLaunchConfig; }

namespace Pelican::watch {

enum class ReloadGateReason : std::uint8_t { replay, strict_assets, rpc_driver };

struct ReloadGateSnapshot {
    bool enabled = false;
    std::uint64_t epoch = 1;
    std::string reason;
};

// Single owner for every determinism gate. EngineLaunchConfig's legacy flag is
// consumed once as the adapter's requested-enabled input; dynamic replay uses
// a reason bit and no longer mutates independent handler flags.
DECLARE_MODULE(ReloadGate) {
  public:
    void configureFromLaunch(const EngineLaunchConfig &config);
    void setReason(ReloadGateReason reason, bool active);
    ReloadGateSnapshot snapshot() const;
    bool enabled() const;
    bool shaderReloadEnabled() const;

  private:
    void updateLocked();
    mutable std::mutex mutex_;
    bool requested_ = false;
    bool replay_ = false;
    bool strict_ = false;
    bool rpc_ = false;
    bool enabled_ = false;
    std::uint64_t epoch_ = 1;
    std::string reason_ = "not configured";
};

} // namespace Pelican::watch
