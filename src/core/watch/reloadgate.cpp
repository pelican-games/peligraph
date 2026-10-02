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

void ReloadGate::setWatchDeclaration(WatchDeclaration declaration) {
    std::scoped_lock lock{mutex_};
    watch_ = declaration;
    updateLocked();
}

bool ReloadGate::rpcDriven() const {
    std::scoped_lock lock{mutex_};
    return rpc_;
}

void ReloadGate::updateLocked() {
    // AV-1: a PELICAN_WITH_HOT_RELOAD=OFF build (a distribution build) has
    // no file watch at all, whatever else asks.
    constexpr bool build_disabled = !PELICAN_WITH_HOT_RELOAD;
    // HW-1: an RPC driver shuts the watch only when the project declares
    // nothing; a declared list is watched under it as well.
    const bool rpc_shuts = rpc_ && watch_ == WatchDeclaration::undeclared;
    const bool declared_empty = watch_ == WatchDeclaration::empty;
    const bool next =
        !build_disabled && requested_ && !replay_ && !strict_ && !rpc_shuts && !declared_empty;
    if (next != enabled_) ++epoch_;
    enabled_ = next;
    if (build_disabled) reason_ = "hot reload build disabled";
    else if (replay_) reason_ = "replay";
    else if (strict_) reason_ = "strict assets";
    else if (rpc_shuts) reason_ = "rpc driver";
    else if (declared_empty) reason_ = "hot_reload empty";
    else if (!requested_) reason_ = "shader_hot_reload disabled";
    else reason_.clear();
}

ReloadGateSnapshot ReloadGate::snapshot() const {
    std::scoped_lock lock{mutex_};
    return {enabled_, epoch_, reason_};
}

bool ReloadGate::enabled() const { return snapshot().enabled; }
bool ReloadGate::shaderReloadEnabled() const { return enabled(); }

std::string ReloadGate::explicitShaderReloadRefusal() const {
    std::scoped_lock lock{mutex_};
    if (replay_) return "replay";
    if (strict_) return "strict assets";
    return {};
}

} // namespace Pelican::watch
