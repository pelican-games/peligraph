#pragma once

#include "./build_features.hpp"
#include "./launchconfig.hpp"

#include <stdexcept>
#include <string>

namespace Pelican {

enum class XrDiscoveryAvailability {
    available,
    runtime_unavailable,
    system_unavailable,
    graphics_binding_unavailable,
    hook_unavailable,
};

struct XrDiscoveryResult {
    XrDiscoveryAvailability availability = XrDiscoveryAvailability::hook_unavailable;
    std::string detail;
};

struct XrDiscoveryHook {
    void *context = nullptr;
    XrDiscoveryResult (*query)(void *context) = nullptr;
};

enum class XrForcedOffDriver { none, headless, rpc, golden, replay };

struct XrActivationDecision {
    XrMode requested_mode = XrMode::off;
    XrMode resolved_mode = XrMode::off;
    bool active = false;
    bool discovery_attempted = false;
    std::string info;
};

inline const char *xrForcedOffDriverName(XrForcedOffDriver driver) {
    switch (driver) {
    case XrForcedOffDriver::headless:
        return "headless";
    case XrForcedOffDriver::rpc:
        return "RPC";
    case XrForcedOffDriver::golden:
        return "golden";
    case XrForcedOffDriver::replay:
        return "replay";
    case XrForcedOffDriver::none:
        break;
    }
    return "window";
}

inline XrForcedOffDriver xrForcedOffDriver(const EngineLaunchConfig &config) {
    // Name the most specific deterministic driver when flags overlap. RPC and
    // replay commonly imply headless, but their public error remains useful.
    if (config.rpc) {
        return XrForcedOffDriver::rpc;
    }
    if (config.golden_mode) {
        return XrForcedOffDriver::golden;
    }
    if (config.input_replay) {
        return XrForcedOffDriver::replay;
    }
    if (config.headless) {
        return XrForcedOffDriver::headless;
    }
    return XrForcedOffDriver::none;
}

inline std::string xrDiscoveryUnavailableName(XrDiscoveryAvailability availability) {
    switch (availability) {
    case XrDiscoveryAvailability::runtime_unavailable:
        return "OpenXR runtime unavailable";
    case XrDiscoveryAvailability::system_unavailable:
        return "OpenXR system unavailable";
    case XrDiscoveryAvailability::graphics_binding_unavailable:
        return "OpenXR graphics binding unavailable";
    case XrDiscoveryAvailability::hook_unavailable:
        return "OpenXR discovery hook unavailable (XR1a)";
    case XrDiscoveryAvailability::available:
        break;
    }
    return "OpenXR unavailable";
}

inline XrActivationDecision resolveXrActivation(const EngineLaunchConfig &config, bool build_has_openxr,
                                                XrDiscoveryHook discovery = {}) {
    XrActivationDecision decision;
    decision.requested_mode = config.xr_mode;
    if (config.xr_mode == XrMode::off) {
        return decision;
    }

    const auto forced_driver = xrForcedOffDriver(config);
    if (forced_driver != XrForcedOffDriver::none) {
        if (config.xr_mode == XrMode::on) {
            const auto detail = std::string{"--xr on is incompatible with "} + xrForcedOffDriverName(forced_driver);
            if (!build_has_openxr) {
                throwBuildFeatureDisabled("PELICAN_WITH_OPENXR", detail);
            }
            throw std::runtime_error(detail);
        }
        if (!build_has_openxr) {
            decision.info = "--xr auto: PELICAN_WITH_OPENXR=OFF; continuing in flat mode";
        }
        return decision;
    }

    if (!build_has_openxr) {
        if (config.xr_mode == XrMode::on) {
            throwBuildFeatureDisabled("PELICAN_WITH_OPENXR", "--xr on is unavailable");
        }
        decision.info = "--xr auto: PELICAN_WITH_OPENXR=OFF; continuing in flat mode";
        return decision;
    }

    decision.discovery_attempted = true;
    const auto discovery_result = discovery.query ? discovery.query(discovery.context) : XrDiscoveryResult{};
    if (discovery_result.availability == XrDiscoveryAvailability::available) {
        decision.resolved_mode = XrMode::on;
        decision.active = true;
        return decision;
    }

    auto unavailable = xrDiscoveryUnavailableName(discovery_result.availability);
    if (!discovery_result.detail.empty()) {
        unavailable += ": " + discovery_result.detail;
    }
    if (config.xr_mode == XrMode::on) {
        throw std::runtime_error("--xr on: " + unavailable);
    }
    decision.info = "--xr auto: " + unavailable + "; continuing in flat mode";
    return decision;
}

inline std::string resolveXrBootstrapFailure(EngineLaunchConfig &config, const std::string &detail) {
    const auto unavailable = std::string{"OpenXR graphics binding unavailable"} +
                             (detail.empty() ? std::string{} : ": " + detail);
    if (!config.xr_active) {
        throw std::logic_error("OpenXR bootstrap failure was reported while XR was inactive");
    }
    if (config.xr_requested_mode == XrMode::on) {
        throw std::runtime_error("--xr on: " + unavailable);
    }
    if (config.xr_requested_mode != XrMode::auto_mode) {
        throw std::logic_error("OpenXR bootstrap fallback requires an auto request");
    }

    config.xr_mode = XrMode::off;
    config.xr_active = false;
    return "--xr auto: " + unavailable + "; continuing in flat mode";
}

} // namespace Pelican
