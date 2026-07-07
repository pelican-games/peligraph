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

} // namespace Pelican
