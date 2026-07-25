#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace Pelican {

enum class XrMultiviewAutoSelection : std::uint8_t {
    multiview,
    sequential,
};

std::string_view xrMultiviewAutoSelectionName(
    XrMultiviewAutoSelection selection);

struct XrMultiviewDeviceIdentity {
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint32_t driver_version = 0;
    std::string device_name;

    bool operator==(
        const XrMultiviewDeviceIdentity &) const = default;
};

struct XrMultiviewGpuMeasurement {
    double sequential_gpu_ms = 0.0;
    double multiview_gpu_ms = 0.0;
    std::uint32_t sample_count = 0;
    std::string source;

    bool operator==(
        const XrMultiviewGpuMeasurement &) const = default;
};

// Profiles are deliberately data-only. The first profile among equally
// specific matches wins; a profile with more optional match fields wins over
// a broader vendor profile regardless of declaration order.
struct XrMultiviewDeviceProfile {
    std::string id;
    std::uint32_t vendor_id = 0;
    std::optional<std::uint32_t> device_id;
    std::optional<std::uint32_t> driver_version;
    std::optional<std::string> device_name_contains;
    std::optional<std::string> graph;
    XrMultiviewGpuMeasurement measurement;

    bool operator==(
        const XrMultiviewDeviceProfile &) const = default;
};

struct XrMultiviewAutoPolicy {
    // A measured improvement must meet this threshold. With the default zero
    // threshold, equal or slower measurements select sequential execution.
    double minimum_gain_percent = 0.0;
    std::vector<XrMultiviewDeviceProfile> profiles;

    bool operator==(
        const XrMultiviewAutoPolicy &) const = default;
};

struct ResolvedXrMultiviewAutoPolicy {
    XrMultiviewAutoSelection selection =
        XrMultiviewAutoSelection::multiview;
    std::string profile_id = "optimize_by_default";
    bool matched_profile = false;
    XrMultiviewDeviceIdentity device;
    std::string graph;
    double measured_gain_percent = 0.0;
    std::optional<XrMultiviewGpuMeasurement> measurement;
    std::string reason =
        "no measured device profile matched; optimize-by-default "
        "selects multiview";

    bool operator==(
        const ResolvedXrMultiviewAutoPolicy &) const = default;
};

XrMultiviewAutoPolicy compileXrMultiviewAutoPolicy(
    const nlohmann::json &declaration);

ResolvedXrMultiviewAutoPolicy resolveXrMultiviewAutoPolicy(
    const XrMultiviewAutoPolicy &policy,
    const XrMultiviewDeviceIdentity &device,
    std::string_view graph);

nlohmann::ordered_json xrMultiviewAutoPolicyToJson(
    const XrMultiviewAutoPolicy &policy);
nlohmann::ordered_json resolvedXrMultiviewAutoPolicyToJson(
    const ResolvedXrMultiviewAutoPolicy &policy);

} // namespace Pelican
