#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace Pelican {

enum class SampleCountRequestMode : std::uint8_t {
    automatic,
    prefer,
    exact,
};

std::string_view sampleCountRequestModeName(SampleCountRequestMode mode);

enum class SampleCountScope : std::uint8_t {
    geometry,
    all,
    none,
};

std::string_view sampleCountScopeName(SampleCountScope scope);

struct SampleCountRequest {
    SampleCountRequestMode mode = SampleCountRequestMode::exact;
    std::uint32_t samples = 1;

    bool operator==(const SampleCountRequest &) const = default;
};

// Typed result of the authoring compiler. Runtime layers consume this value
// and never reinterpret the source JSON.
struct SampleCountPolicy {
    SampleCountRequest request;
    SampleCountScope scope = SampleCountScope::geometry;
    std::vector<std::string> targets;
    bool authored = false;

    bool operator==(const SampleCountPolicy &) const = default;
};

SampleCountPolicy compileSampleCountPolicy(
    const nlohmann::json &normalized_rendering_config);
nlohmann::ordered_json sampleCountPolicyToJson(
    const SampleCountPolicy &policy);

struct SampleCountResourceCapability {
    std::string resource;
    std::string format;
    std::vector<std::uint32_t> supported_samples;

    bool operator==(const SampleCountResourceCapability &) const = default;
};

// Resources in one group can be bound by the same rendering scope, directly
// or transitively. They therefore need one common rasterization sample count.
struct SampleCountGroupRequest {
    std::string id;
    bool multisampling_enabled = true;
    std::vector<SampleCountResourceCapability> resources;

    bool operator==(const SampleCountGroupRequest &) const = default;
};

struct ResolvedSampleCountResource {
    std::string resource;
    std::string format;
    std::uint32_t samples = 1;

    bool operator==(const ResolvedSampleCountResource &) const = default;
};

struct ResolvedSampleCountGroup {
    std::string id;
    std::uint32_t requested_samples = 1;
    std::uint32_t selected_samples = 1;
    bool fallback = false;
    std::vector<std::string> limiting_resources;
    std::string reason;

    bool operator==(const ResolvedSampleCountGroup &) const = default;
};

struct ResolvedSampleCountPlan {
    SampleCountRequest request;
    std::vector<ResolvedSampleCountGroup> groups;
    std::vector<ResolvedSampleCountResource> resources;

    bool operator==(const ResolvedSampleCountPlan &) const = default;
};

ResolvedSampleCountPlan resolveSampleCountPlan(
    SampleCountRequest request,
    std::span<const SampleCountGroupRequest> groups);
nlohmann::ordered_json resolvedSampleCountPlanToJson(
    const ResolvedSampleCountPlan &plan);

} // namespace Pelican
