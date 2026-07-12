#include "reloadgate.hpp"

#include "../launchconfig.hpp"

namespace Pelican::watch {

void ReloadGate::configureFromLaunch(const EngineLaunchConfig &config) {
    std::scoped_lock lock{mutex_};
    requested_ = config.shader_hot_reload;
    replay_ = config.input_replay || config.input_replay_path.has_value();
    strict_ = config.strict_assets;
    rpc_ = config.rpc;
    updateLocked();
}

void ReloadGate::setReason(ReloadGateReason reason, bool active) {
    std::scoped_lock lock{mutex_};
    switch (reason) {
    case ReloadGateReason::replay: replay_ = active; break;
    case ReloadGateReason::strict_assets: strict_ = active; break;
    case ReloadGateReason::rpc_driver: rpc_ = active; break;
    }
    updateLocked();
}

void ReloadGate::updateLocked() {
    const bool next = requested_ && !replay_ && !strict_ && !rpc_;
    if (next != enabled_) ++epoch_;
    enabled_ = next;
    if (replay_) reason_ = "replay";
    else if (strict_) reason_ = "strict assets";
    else if (rpc_) reason_ = "rpc driver";
    else if (!requested_) reason_ = "shader_hot_reload disabled";
    else reason_.clear();
}

ReloadGateSnapshot ReloadGate::snapshot() const {
    std::scoped_lock lock{mutex_};
    return {enabled_, epoch_, reason_};
}

bool ReloadGate::enabled() const { return snapshot().enabled; }
bool ReloadGate::shaderPollEnabled() const { return enabled(); }

} // namespace Pelican::watch
