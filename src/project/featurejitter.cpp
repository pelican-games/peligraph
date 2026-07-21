#include "featurejitter.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace Pelican::FeatureComposeInternal {

std::optional<nlohmann::json> parseProjectionJitterDeclaration(
    const nlohmann::json &feature, const std::string &feature_name) {
    if (!feature.contains("projection_jitter")) {
        return std::nullopt;
    }
    const auto &declaration = feature.at("projection_jitter");
    if (!declaration.is_object()) {
        throw std::runtime_error("projection_jitter provider '" + feature_name +
                                 "' declaration must be an object");
    }
    for (auto field = declaration.begin(); field != declaration.end(); ++field) {
        if (field.key() == "phases_from_scale" || field.key() == "mip_bias" ||
            field.key() == "render_scale") {
            throw std::runtime_error("projection_jitter provider '" + feature_name +
                                     "' uses reserved key: " + field.key());
        }
    }
    if (!declaration.contains("pattern") ||
        !declaration.at("pattern").is_string()) {
        throw std::runtime_error("projection_jitter provider '" + feature_name +
                                 "' requires string pattern");
    }
    const auto pattern = declaration.at("pattern").get<std::string>();
    if (pattern != "halton23" && pattern != "table") {
        throw std::runtime_error("projection_jitter provider '" + feature_name +
                                 "' has unknown pattern: " + pattern);
    }

    for (auto field = declaration.begin(); field != declaration.end(); ++field) {
        const bool allowed = field.key() == "pattern" || field.key() == "phases" ||
                             (pattern == "table" && field.key() == "offsets_px");
        if (!allowed) {
            throw std::runtime_error("projection_jitter provider '" + feature_name +
                                     "' has unknown key: " + field.key());
        }
    }

    const auto parsePhases = [&]() -> std::uint32_t {
        if (!declaration.contains("phases") ||
            !declaration.at("phases").is_number_integer()) {
            throw std::runtime_error("projection_jitter provider '" + feature_name +
                                     "' requires unsigned integer phases");
        }
        const auto &phases = declaration.at("phases");
        const bool in_range = phases.is_number_unsigned()
                                  ? phases.get<std::uint64_t>() >= 1 &&
                                        phases.get<std::uint64_t>() <= 64
                                  : phases.get<std::int64_t>() >= 1 &&
                                        phases.get<std::int64_t>() <= 64;
        if (!in_range) {
            throw std::runtime_error("projection_jitter provider '" + feature_name +
                                     "' phases must be in range 1..64");
        }
        return phases.get<std::uint32_t>();
    };

    if (pattern == "halton23") {
        return nlohmann::json{{"provider", feature_name}, {"pattern", pattern},
                              {"phases", parsePhases()}};
    }

    if (!declaration.contains("offsets_px") ||
        !declaration.at("offsets_px").is_array()) {
        throw std::runtime_error("projection_jitter provider '" + feature_name +
                                 "' table requires offsets_px array");
    }
    const auto &offsets = declaration.at("offsets_px");
    if (offsets.empty() || offsets.size() > 64) {
        throw std::runtime_error("projection_jitter provider '" + feature_name +
                                 "' offsets_px length must be in range 1..64");
    }
    for (std::size_t index = 0; index < offsets.size(); ++index) {
        const auto &offset = offsets.at(index);
        if (!offset.is_array() || offset.size() != 2) {
            throw std::runtime_error("projection_jitter provider '" + feature_name +
                                     "' offsets_px[" + std::to_string(index) +
                                     "] must contain exactly two numbers");
        }
        for (std::size_t component = 0; component < 2; ++component) {
            const auto &value = offset.at(component);
            if (!value.is_number()) {
                throw std::runtime_error(
                    "projection_jitter provider '" + feature_name + "' offsets_px[" +
                    std::to_string(index) + "][" + std::to_string(component) +
                    "] must be a finite number");
            }
            const auto number = value.get<double>();
            if (!std::isfinite(number)) {
                throw std::runtime_error(
                    "projection_jitter provider '" + feature_name + "' offsets_px[" +
                    std::to_string(index) + "][" + std::to_string(component) +
                    "] must be finite");
            }
            if (number < -0.5 || number >= 0.5) {
                throw std::runtime_error(
                    "projection_jitter provider '" + feature_name + "' offsets_px[" +
                    std::to_string(index) + "][" + std::to_string(component) +
                    "] must be in range [-0.5, 0.5)");
            }
        }
    }

    const auto phases = static_cast<std::uint32_t>(offsets.size());
    if (declaration.contains("phases") && parsePhases() != phases) {
        throw std::runtime_error("projection_jitter provider '" + feature_name +
                                 "' phases must match offsets_px length " +
                                 std::to_string(phases));
    }
    return nlohmann::json{{"provider", feature_name}, {"pattern", pattern},
                          {"phases", phases}, {"offsets_px", offsets}};
}

} // namespace Pelican::FeatureComposeInternal
