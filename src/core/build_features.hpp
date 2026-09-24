#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace Pelican {

inline std::string buildFeatureDisabledMessage(std::string_view feature, std::string_view detail = {}) {
    std::string message = "This binary was built with ";
    message += feature;
    message += "=OFF";
    if (!detail.empty()) {
        message += ": ";
        message += detail;
    }
    return message;
}

class BuildFeatureDisabledError : public std::runtime_error {
    std::string feature_name;

  public:
    explicit BuildFeatureDisabledError(std::string_view feature, std::string_view detail = {})
        : std::runtime_error(buildFeatureDisabledMessage(feature, detail)), feature_name{feature} {}

    const std::string &feature() const { return feature_name; }
};

inline bool isBuildFeatureDisabledError(const std::exception &error) {
    return dynamic_cast<const BuildFeatureDisabledError *>(&error) != nullptr;
}

[[noreturn]] inline void throwBuildFeatureDisabled(std::string_view feature, std::string_view detail = {}) {
    throw BuildFeatureDisabledError{feature, detail};
}

// PG-2: the one failure a game-code facade (ctx.audio(), ctx.physics(), and
// the scene effect transaction behind them) answers with when the build left
// its module out. BuildFeatureDisabledError keeps its layout and constructor
// -- a game DLL may already catch it by that name -- so the name rides on this
// derived type the way StructFieldReasonError carries its reason: every
// existing catch still catches it, feature() and the prose message are the
// base's, and code() is the name a scene effect records for the same refusal.
class BuildFeatureCallDisabledError final : public BuildFeatureDisabledError {
  public:
    static constexpr std::string_view error_code = "build_feature_disabled@1";

    BuildFeatureCallDisabledError(std::string_view feature, std::string_view detail)
        : BuildFeatureDisabledError(feature, detail) {}

    std::string_view code() const noexcept { return error_code; }
};

// `entry` is the facade function the game called, e.g. "playSound"; the
// message stays "This binary was built with <feature>=OFF: <entry> is
// unavailable".
[[noreturn]] inline void throwBuildFeatureCallDisabled(std::string_view feature,
                                                       std::string_view entry) {
    throw BuildFeatureCallDisabledError{feature, std::string{entry} + " is unavailable"};
}

} // namespace Pelican
