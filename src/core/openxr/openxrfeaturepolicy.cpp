#include "openxrfeaturepolicy.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>
#include <string>

namespace Pelican::OpenXr {
namespace {

bool declaresHistory(const nlohmann::json &feature) {
    if (!feature.contains("render_targets") ||
        !feature.at("render_targets").is_array()) {
        return false;
    }
    for (const auto &target : feature.at("render_targets")) {
        if (target.is_object() && target.value("history", false)) return true;
    }
    return false;
}

bool declaresPassType(const nlohmann::json &feature, std::string_view type) {
    if (!feature.contains("passes") || !feature.at("passes").is_array()) {
        return false;
    }
    for (const auto &entry : feature.at("passes")) {
        if (!entry.is_object() || !entry.contains("pass") ||
            !entry.at("pass").is_object()) {
            continue;
        }
        if (entry.at("pass").value("type", std::string{}) == type) return true;
    }
    return false;
}

bool looksLikeVelocityName(std::string_view name) {
    std::string lower{name};
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower.find("velocity") != std::string::npos ||
           lower.find("motion_vector") != std::string::npos;
}

bool declaresVelocityTarget(const nlohmann::json &feature) {
    if (!feature.contains("render_targets") ||
        !feature.at("render_targets").is_array()) {
        return false;
    }
    return std::any_of(
        feature.at("render_targets").begin(), feature.at("render_targets").end(),
        [](const auto &target) {
            return target.is_object() &&
                   looksLikeVelocityName(target.value("name", std::string{}));
        });
}

bool declaresUiAnchor(const nlohmann::json &feature) {
    if (!feature.contains("passes") || !feature.at("passes").is_array()) {
        return false;
    }
    return std::any_of(feature.at("passes").begin(), feature.at("passes").end(),
                       [](const auto &entry) {
                           return entry.is_object() &&
                                  entry.value("insert", std::string{}).find("pelican_ui") !=
                                      std::string::npos;
                       });
}

bool knownExcludedFeature(std::string_view name) {
    constexpr std::array<std::string_view, 3> names{"taa", "velocity", "ui"};
    for (const auto candidate : names) {
        if (candidate == name) return true;
    }
    return false;
}

[[noreturn]] void rejectHistory(std::string_view feature_name) {
    throw std::runtime_error("OpenXR activation rejected history feature '" +
                             std::string{feature_name} + "'");
}

} // namespace

bool includeFeatureInXrGraph(std::string_view feature_name,
                             const nlohmann::json &feature) {
    if (knownExcludedFeature(feature_name)) return false;
    if (declaresHistory(feature)) rejectHistory(feature_name);
    // A custom jitter/velocity/UI provider is safe only when removed as one
    // complete feature unit.  Its name remains in excluded_feature_names for
    // activation diagnostics and the startup plan trace.
    if (feature.contains("projection_jitter") ||
        declaresPassType(feature, "velocity") ||
        declaresVelocityTarget(feature) || declaresPassType(feature, "ui") ||
        declaresUiAnchor(feature)) {
        return false;
    }
    return true;
}

void validateXrGraphConfig(const nlohmann::json &config) {
    if (config.contains("projection_jitter")) {
        throw std::runtime_error(
            "OpenXR activation cannot include authored projection jitter");
    }
    if (config.contains("render_targets") && config.at("render_targets").is_array()) {
        for (const auto &target : config.at("render_targets")) {
            if (target.is_object() && target.value("history", false)) {
                rejectHistory("<base rendering config>:" +
                              target.value("name", std::string{"unnamed"}));
            }
            if (target.is_object() &&
                looksLikeVelocityName(target.value("name", std::string{}))) {
                throw std::runtime_error(
                    "OpenXR activation cannot include velocity target '" +
                    target.value("name", std::string{"unnamed"}) + "'");
            }
        }
    }
    if (!config.contains("rendering_passes") ||
        !config.at("rendering_passes").is_array()) {
        return;
    }
    for (const auto &graph : config.at("rendering_passes")) {
        if (!graph.is_object() || !graph.contains("passes") ||
            !graph.at("passes").is_array()) {
            continue;
        }
        for (const auto &pass : graph.at("passes")) {
            if (!pass.is_object()) continue;
            const auto type = pass.value("type", std::string{});
            if (type == "ui" || type == "velocity") {
                throw std::runtime_error(
                    "OpenXR activation cannot exclude direct " + type + " pass '" +
                    pass.value("name", std::string{"unnamed"}) + "'");
            }
            if (!pass.contains("input")) continue;
            const auto &inputs = pass.at("input");
            const auto reject_input = [](const nlohmann::json &input) {
                return input.is_string() &&
                       input.get_ref<const std::string &>().ends_with("@history");
            };
            if ((inputs.is_string() && reject_input(inputs)) ||
                (inputs.is_array() &&
                 std::any_of(inputs.begin(), inputs.end(), reject_input))) {
                rejectHistory("<base rendering config>:" +
                              pass.value("name", std::string{"unnamed"}));
            }
        }
    }
}

} // namespace Pelican::OpenXr
