#include "xrmultiviewprofile.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace Pelican {
namespace {

char asciiLower(char value) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(value)));
}

bool containsCaseInsensitive(
    std::string_view value,
    std::string_view fragment) {
    return std::search(
               value.begin(), value.end(),
               fragment.begin(), fragment.end(),
               [](char left, char right) {
                   return asciiLower(left) ==
                          asciiLower(right);
               }) != value.end();
}

bool matches(
    const XrMultiviewDeviceProfile &profile,
    const XrMultiviewDeviceIdentity &device,
    std::string_view graph) {
    if (profile.vendor_id != device.vendor_id) {
        return false;
    }
    if (profile.device_id &&
        *profile.device_id != device.device_id) {
        return false;
    }
    if (profile.driver_version &&
        *profile.driver_version != device.driver_version) {
        return false;
    }
    if (profile.device_name_contains &&
        !containsCaseInsensitive(
            device.device_name,
            *profile.device_name_contains)) {
        return false;
    }
    return !profile.graph ||
           *profile.graph == graph;
}

std::uint32_t specificity(
    const XrMultiviewDeviceProfile &profile) {
    return static_cast<std::uint32_t>(
        profile.device_id.has_value()) +
           static_cast<std::uint32_t>(
               profile.driver_version.has_value()) +
           static_cast<std::uint32_t>(
               profile.device_name_contains.has_value()) +
           static_cast<std::uint32_t>(
               profile.graph.has_value());
}

nlohmann::ordered_json measurementToJson(
    const XrMultiviewGpuMeasurement &measurement) {
    return {
        {"sequential_gpu_ms",
         measurement.sequential_gpu_ms},
        {"multiview_gpu_ms",
         measurement.multiview_gpu_ms},
        {"sample_count", measurement.sample_count},
        {"source", measurement.source},
    };
}

void requireOnlyKeys(
    const nlohmann::json &object,
    std::initializer_list<std::string_view> allowed,
    std::string_view subject) {
    for (auto field = object.begin();
         field != object.end(); ++field) {
        const auto known = std::find(
            allowed.begin(), allowed.end(),
            field.key()) != allowed.end();
        if (!known) {
            throw std::runtime_error(
                std::string{subject} +
                " has unknown key '" +
                field.key() + "'");
        }
    }
}

std::string requiredNonEmptyString(
    const nlohmann::json &object,
    std::string_view key,
    std::string_view subject) {
    const auto found =
        object.find(std::string{key});
    if (found == object.end() ||
        !found->is_string() ||
        found->get_ref<
            const std::string &>()
            .empty()) {
        throw std::runtime_error(
            std::string{subject} + " " +
            std::string{key} +
            " must be a non-empty string");
    }
    return found->get<std::string>();
}

std::uint32_t requiredUnsigned(
    const nlohmann::json &object,
    std::string_view key,
    std::string_view subject) {
    const auto found =
        object.find(std::string{key});
    if (found == object.end() ||
        !found->is_number_integer()) {
        throw std::runtime_error(
            std::string{subject} + " " +
            std::string{key} +
            " must be an unsigned 32-bit integer");
    }
    std::uint64_t value = 0;
    if (found->is_number_unsigned()) {
        value = found->get<std::uint64_t>();
    } else {
        const auto signed_value =
            found->get<std::int64_t>();
        if (signed_value < 0) {
            throw std::runtime_error(
                std::string{subject} + " " +
                std::string{key} +
                " must be an unsigned 32-bit integer");
        }
        value = static_cast<std::uint64_t>(
            signed_value);
    }
    if (value > std::numeric_limits<
                    std::uint32_t>::max()) {
        throw std::runtime_error(
            std::string{subject} + " " +
            std::string{key} +
            " must be an unsigned 32-bit integer");
    }
    return static_cast<std::uint32_t>(value);
}

std::optional<std::uint32_t> optionalUnsigned(
    const nlohmann::json &object,
    std::string_view key,
    std::string_view subject) {
    if (!object.contains(std::string{key})) {
        return std::nullopt;
    }
    return requiredUnsigned(object, key, subject);
}

std::optional<std::string> optionalNonEmptyString(
    const nlohmann::json &object,
    std::string_view key,
    std::string_view subject) {
    if (!object.contains(std::string{key})) {
        return std::nullopt;
    }
    return requiredNonEmptyString(
        object, key, subject);
}

XrMultiviewGpuMeasurement parseMeasurement(
    const nlohmann::json &value,
    std::string_view profile_id) {
    const auto subject =
        "rendering config xr multiview_auto profile '" +
        std::string{profile_id} + "' measurement";
    if (!value.is_object()) {
        throw std::runtime_error(
            subject + " must be an object");
    }
    requireOnlyKeys(
        value,
        {"sequential_gpu_ms", "multiview_gpu_ms",
         "sample_count", "source"},
        subject);
    const auto measuredNumber =
        [&](std::string_view key) {
            const auto found =
                value.find(std::string{key});
            if (found == value.end() ||
                !found->is_number()) {
                throw std::runtime_error(
                    subject + " " +
                    std::string{key} +
                    " must be a finite positive number");
            }
            const auto result =
                found->get<double>();
            if (!std::isfinite(result) ||
                result <= 0.0) {
                throw std::runtime_error(
                    subject + " " +
                    std::string{key} +
                    " must be a finite positive number");
            }
            return result;
        };
    const auto sample_count =
        requiredUnsigned(
            value, "sample_count", subject);
    if (sample_count == 0) {
        throw std::runtime_error(
            subject +
            " sample_count must be greater than zero");
    }
    return {
        .sequential_gpu_ms =
            measuredNumber(
                "sequential_gpu_ms"),
        .multiview_gpu_ms =
            measuredNumber(
                "multiview_gpu_ms"),
        .sample_count = sample_count,
        .source =
            requiredNonEmptyString(
                value, "source", subject),
    };
}

} // namespace

std::string_view xrMultiviewAutoSelectionName(
    XrMultiviewAutoSelection selection) {
    switch (selection) {
    case XrMultiviewAutoSelection::multiview:
        return "multiview";
    case XrMultiviewAutoSelection::sequential:
        return "sequential";
    }
    return "unknown";
}

XrMultiviewAutoPolicy compileXrMultiviewAutoPolicy(
    const nlohmann::json &declaration) {
    static constexpr std::string_view subject =
        "rendering config xr multiview_auto";
    if (!declaration.is_object()) {
        throw std::runtime_error(
            std::string{subject} +
            " must be an object");
    }
    requireOnlyKeys(
        declaration,
        {"minimum_gain_percent", "profiles"},
        subject);

    XrMultiviewAutoPolicy result;
    if (const auto threshold =
            declaration.find(
                "minimum_gain_percent");
        threshold != declaration.end()) {
        if (!threshold->is_number()) {
            throw std::runtime_error(
                std::string{subject} +
                " minimum_gain_percent must be a finite "
                "number in [0, 100)");
        }
        result.minimum_gain_percent =
            threshold->get<double>();
        if (!std::isfinite(
                result.minimum_gain_percent) ||
            result.minimum_gain_percent < 0.0 ||
            result.minimum_gain_percent >= 100.0) {
            throw std::runtime_error(
                std::string{subject} +
                " minimum_gain_percent must be a finite "
                "number in [0, 100)");
        }
    }
    const auto profiles =
        declaration.find("profiles");
    if (profiles == declaration.end()) {
        return result;
    }
    if (!profiles->is_array()) {
        throw std::runtime_error(
            std::string{subject} +
            " profiles must be an array");
    }

    std::set<std::string, std::less<>> ids;
    result.profiles.reserve(profiles->size());
    for (const auto &profile : *profiles) {
        if (!profile.is_object()) {
            throw std::runtime_error(
                std::string{subject} +
                " profile must be an object");
        }
        requireOnlyKeys(
            profile,
            {"id", "vendor_id", "device_id",
             "driver_version",
             "device_name_contains", "graph",
             "measurement"},
            std::string{subject} + " profile");
        const auto id =
            requiredNonEmptyString(
                profile, "id",
                std::string{subject} +
                    " profile");
        if (!ids.insert(id).second) {
            throw std::runtime_error(
                std::string{subject} +
                " has duplicate profile id '" +
                id + "'");
        }
        const auto measurement =
            profile.find("measurement");
        if (measurement == profile.end()) {
            throw std::runtime_error(
                std::string{subject} +
                " profile '" + id +
                "' measurement must be an object");
        }
        result.profiles.push_back({
            .id = id,
            .vendor_id =
                requiredUnsigned(
                    profile, "vendor_id",
                    std::string{subject} +
                        " profile '" +
                        id + "'"),
            .device_id =
                optionalUnsigned(
                    profile, "device_id",
                    std::string{subject} +
                        " profile '" +
                        id + "'"),
            .driver_version =
                optionalUnsigned(
                    profile,
                    "driver_version",
                    std::string{subject} +
                        " profile '" +
                        id + "'"),
            .device_name_contains =
                optionalNonEmptyString(
                    profile,
                    "device_name_contains",
                    std::string{subject} +
                        " profile '" +
                        id + "'"),
            .graph =
                optionalNonEmptyString(
                    profile, "graph",
                    std::string{subject} +
                        " profile '" +
                        id + "'"),
            .measurement =
                parseMeasurement(
                    *measurement, id),
        });
    }
    return result;
}

ResolvedXrMultiviewAutoPolicy resolveXrMultiviewAutoPolicy(
    const XrMultiviewAutoPolicy &policy,
    const XrMultiviewDeviceIdentity &device,
    std::string_view graph) {
    if (!std::isfinite(policy.minimum_gain_percent) ||
        policy.minimum_gain_percent < 0.0 ||
        policy.minimum_gain_percent >= 100.0) {
        throw std::runtime_error(
            "XR multiview auto minimum gain percent must be "
            "finite and in [0, 100)");
    }

    const XrMultiviewDeviceProfile *selected = nullptr;
    std::uint32_t selected_specificity = 0;
    for (const auto &profile : policy.profiles) {
        if (!matches(profile, device, graph)) {
            continue;
        }
        const auto candidate_specificity =
            specificity(profile);
        if (selected == nullptr ||
            candidate_specificity >
                selected_specificity) {
            selected = &profile;
            selected_specificity =
                candidate_specificity;
        }
    }
    if (selected == nullptr) {
        ResolvedXrMultiviewAutoPolicy result;
        result.device = device;
        result.graph = graph;
        return result;
    }

    const auto &measurement = selected->measurement;
    if (!std::isfinite(
            measurement.sequential_gpu_ms) ||
        !std::isfinite(
            measurement.multiview_gpu_ms) ||
        measurement.sequential_gpu_ms <= 0.0 ||
        measurement.multiview_gpu_ms <= 0.0 ||
        measurement.sample_count == 0 ||
        measurement.source.empty()) {
        throw std::runtime_error(
            "XR multiview device profile '" +
            selected->id +
            "' has invalid GPU measurement evidence");
    }

    const auto gain =
        (measurement.sequential_gpu_ms -
         measurement.multiview_gpu_ms) /
        measurement.sequential_gpu_ms * 100.0;
    const auto improved =
        measurement.multiview_gpu_ms <
            measurement.sequential_gpu_ms &&
        gain >= policy.minimum_gain_percent;

    ResolvedXrMultiviewAutoPolicy result;
    result.selection =
        improved
            ? XrMultiviewAutoSelection::multiview
            : XrMultiviewAutoSelection::sequential;
    result.profile_id = selected->id;
    result.matched_profile = true;
    result.device = device;
    result.graph = graph;
    result.measured_gain_percent = gain;
    result.measurement = measurement;
    result.reason =
        "measured profile '" + selected->id +
        "' selects " +
        std::string{xrMultiviewAutoSelectionName(
            result.selection)} +
        " (gain=" + std::to_string(gain) +
        "%, required=" +
        std::to_string(
            policy.minimum_gain_percent) +
        "%, samples=" +
        std::to_string(measurement.sample_count) +
        ", source=" + measurement.source + ")";
    return result;
}

nlohmann::ordered_json xrMultiviewAutoPolicyToJson(
    const XrMultiviewAutoPolicy &policy) {
    nlohmann::ordered_json profiles =
        nlohmann::ordered_json::array();
    for (const auto &profile : policy.profiles) {
        nlohmann::ordered_json encoded{
            {"id", profile.id},
            {"vendor_id", profile.vendor_id},
            {"measurement",
             measurementToJson(
                 profile.measurement)},
        };
        if (profile.device_id) {
            encoded["device_id"] =
                *profile.device_id;
        }
        if (profile.driver_version) {
            encoded["driver_version"] =
                *profile.driver_version;
        }
        if (profile.device_name_contains) {
            encoded["device_name_contains"] =
                *profile.device_name_contains;
        }
        if (profile.graph) {
            encoded["graph"] = *profile.graph;
        }
        profiles.push_back(std::move(encoded));
    }
    return {
        {"minimum_gain_percent",
         policy.minimum_gain_percent},
        {"profiles", std::move(profiles)},
    };
}

nlohmann::ordered_json resolvedXrMultiviewAutoPolicyToJson(
    const ResolvedXrMultiviewAutoPolicy &policy) {
    nlohmann::ordered_json result{
        {"selection",
         xrMultiviewAutoSelectionName(
             policy.selection)},
        {"profile_id", policy.profile_id},
        {"matched_profile",
         policy.matched_profile},
        {"device",
         {
             {"vendor_id",
              policy.device.vendor_id},
             {"device_id",
              policy.device.device_id},
             {"driver_version",
              policy.device.driver_version},
             {"device_name",
              policy.device.device_name},
         }},
        {"graph", policy.graph},
        {"measured_gain_percent",
         policy.measured_gain_percent},
        {"reason", policy.reason},
    };
    if (policy.measurement) {
        result["measurement"] =
            measurementToJson(*policy.measurement);
    }
    return result;
}

} // namespace Pelican
