#pragma once

#include "../container.hpp"

#include <cstdint>
#include <mutex>
#include <string>

namespace Pelican { class EngineLaunchConfig; }

namespace Pelican::watch {

enum class ReloadGateReason : std::uint8_t { replay, strict_assets, rpc_driver };

// HW-1: what project.json's `hot_reload` declares. `undeclared` keeps the
// launch default (the whole project watched, nothing under an RPC driver);
// `listed` watches the listed paths, under an RPC driver too; `empty`
// watches nothing.
enum class WatchDeclaration : std::uint8_t { undeclared, empty, listed };

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
    // HW-1. Set when the project is known (ReloadService::setup); a launch
    // reconfiguration leaves it as it is.
    void setWatchDeclaration(WatchDeclaration declaration);
    // True while an RPC client drives the frames (whatever the watch).
    bool rpcDriven() const;
    ReloadGateSnapshot snapshot() const;
    bool enabled() const;
    bool shaderReloadEnabled() const;
    // F-5. An explicit request (the reload_shaders RPC) is not the file
    // watch: it passes a gate that is shut only because an RPC client drives
    // the frames, or because nobody asked for the watch. The determinism
    // reasons still refuse it; the reason is returned, empty when allowed.
    std::string explicitShaderReloadRefusal() const;

  private:
    void updateLocked();
    mutable std::mutex mutex_;
    bool requested_ = false;
    bool replay_ = false;
    bool strict_ = false;
    bool rpc_ = false;
    WatchDeclaration watch_ = WatchDeclaration::undeclared;
    bool enabled_ = false;
    std::uint64_t epoch_ = 1;
    std::string reason_ = "not configured";
};

} // namespace Pelican::watch
