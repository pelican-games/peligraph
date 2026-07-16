#include <catch2/catch_test_macros.hpp>

#include "../src/core/xractivation.hpp"

#include <array>
#include <string>

namespace {

struct DiscoveryFixture {
    Pelican::XrDiscoveryAvailability availability = Pelican::XrDiscoveryAvailability::hook_unavailable;
    int calls = 0;
};

Pelican::XrDiscoveryResult queryDiscovery(void *context) {
    auto &fixture = *static_cast<DiscoveryFixture *>(context);
    ++fixture.calls;
    return {fixture.availability};
}

void setDriver(Pelican::EngineLaunchConfig &config, Pelican::XrForcedOffDriver driver) {
    switch (driver) {
    case Pelican::XrForcedOffDriver::headless:
        config.headless = true;
        break;
    case Pelican::XrForcedOffDriver::rpc:
        config.rpc = true;
        config.headless = true;
        break;
    case Pelican::XrForcedOffDriver::golden:
        config.golden_mode = true;
        break;
    case Pelican::XrForcedOffDriver::replay:
        config.input_replay = true;
        config.headless = true;
        break;
    case Pelican::XrForcedOffDriver::none:
        break;
    }
}

const char *modeName(Pelican::XrMode mode) {
    switch (mode) {
    case Pelican::XrMode::off:
        return "off";
    case Pelican::XrMode::auto_mode:
        return "auto";
    case Pelican::XrMode::on:
        return "on";
    }
    return "unknown";
}

} // namespace

TEST_CASE("OpenXR activation compile-mode-driver matrix", "[openxr][activation]") {
    constexpr std::array build_states{false, true};
    constexpr std::array modes{Pelican::XrMode::off, Pelican::XrMode::auto_mode, Pelican::XrMode::on};
    constexpr std::array drivers{
        Pelican::XrForcedOffDriver::none,
        Pelican::XrForcedOffDriver::headless,
        Pelican::XrForcedOffDriver::rpc,
        Pelican::XrForcedOffDriver::golden,
        Pelican::XrForcedOffDriver::replay,
    };

    for (const bool compiled : build_states) {
        for (const auto mode : modes) {
            for (const auto driver : drivers) {
                DYNAMIC_SECTION("compile " << (compiled ? "ON" : "OFF") << " / --xr " << modeName(mode)
                                             << " / " << Pelican::xrForcedOffDriverName(driver)) {
                    Pelican::EngineLaunchConfig config;
                    config.xr_mode = mode;
                    setDriver(config, driver);
                    DiscoveryFixture discovery;

                    bool threw = false;
                    std::string error;
                    Pelican::XrActivationDecision decision;
                    try {
                        decision = Pelican::resolveXrActivation(
                            config, compiled,
                            Pelican::XrDiscoveryHook{.context = &discovery, .query = queryDiscovery});
                    } catch (const std::exception &exception) {
                        threw = true;
                        error = exception.what();
                    }

                    const bool forced = driver != Pelican::XrForcedOffDriver::none;
                    const bool expected_error = mode == Pelican::XrMode::on;
                    CHECK(threw == expected_error);
                    const bool expected_probe = compiled && !forced && mode != Pelican::XrMode::off;
                    CHECK(discovery.calls == (expected_probe ? 1 : 0));

                    if (threw) {
                        CHECK(error.find("--xr on") != std::string::npos);
                        if (forced) {
                            const auto incompatibility = std::string{"--xr on is incompatible with "} +
                                                         Pelican::xrForcedOffDriverName(driver);
                            CHECK(error.find(incompatibility) != std::string::npos);
                        }
                        if (!compiled) {
                            CHECK(error.find("PELICAN_WITH_OPENXR=OFF") != std::string::npos);
                        }
                    } else {
                        CHECK_FALSE(decision.active);
                        CHECK(decision.resolved_mode == Pelican::XrMode::off);
                        CHECK(decision.discovery_attempted == expected_probe);
                        const bool expected_info = mode == Pelican::XrMode::auto_mode && (!compiled || !forced);
                        CHECK((!decision.info.empty()) == expected_info);
                        if (!compiled && mode == Pelican::XrMode::auto_mode) {
                            CHECK(decision.info.find("PELICAN_WITH_OPENXR=OFF") != std::string::npos);
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("OpenXR window activation uses the injected discovery result", "[openxr][activation]") {
    constexpr std::array request_modes{Pelican::XrMode::auto_mode, Pelican::XrMode::on};
    for (const auto mode : request_modes) {
        DYNAMIC_SECTION("available / " << modeName(mode)) {
            Pelican::EngineLaunchConfig config;
            config.xr_mode = mode;
            DiscoveryFixture discovery{.availability = Pelican::XrDiscoveryAvailability::available};
            const auto decision = Pelican::resolveXrActivation(
                config, true, Pelican::XrDiscoveryHook{.context = &discovery, .query = queryDiscovery});
            CHECK(decision.active);
            CHECK(decision.resolved_mode == Pelican::XrMode::on);
            CHECK(decision.discovery_attempted);
            CHECK(decision.info.empty());
            CHECK(discovery.calls == 1);
        }
    }

    constexpr std::array unavailable{
        Pelican::XrDiscoveryAvailability::runtime_unavailable,
        Pelican::XrDiscoveryAvailability::system_unavailable,
        Pelican::XrDiscoveryAvailability::graphics_binding_unavailable,
    };
    for (const auto availability : unavailable) {
        Pelican::EngineLaunchConfig config;
        config.xr_mode = Pelican::XrMode::auto_mode;
        DiscoveryFixture discovery{.availability = availability};
        const auto decision = Pelican::resolveXrActivation(
            config, true, Pelican::XrDiscoveryHook{.context = &discovery, .query = queryDiscovery});
        CAPTURE(static_cast<int>(availability));
        CHECK_FALSE(decision.active);
        CHECK(decision.info.find("OpenXR") != std::string::npos);
        CHECK(discovery.calls == 1);
    }
}
