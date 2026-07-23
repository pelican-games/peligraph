#include "samplecountplanning.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace Pelican {
namespace {

bool validSampleCount(std::uint32_t samples) {
    return samples != 0 && samples <= 64 &&
           (samples & (samples - 1)) == 0;
}

std::vector<std::uint32_t> canonicalCounts(
    std::vector<std::uint32_t> counts, std::string_view resource) {
    if (counts.empty()) {
        throw std::runtime_error("sample-count resource '" +
                                 std::string{resource} +
                                 "' has no supported sample counts");
    }
    for (const auto count : counts) {
        if (!validSampleCount(count)) {
            throw std::runtime_error("sample-count resource '" +
                                     std::string{resource} +
                                     "' has invalid supported sample count " +
                                     std::to_string(count));
        }
    }
    std::sort(counts.begin(), counts.end());
    counts.erase(std::unique(counts.begin(), counts.end()), counts.end());
    return counts;
}

bool supports(const SampleCountResourceCapability &resource,
              std::uint32_t samples) {
    return std::binary_search(resource.supported_samples.begin(),
                              resource.supported_samples.end(), samples);
}

std::vector<std::string> limitingResources(
    const SampleCountGroupRequest &group, std::uint32_t samples) {
    std::vector<std::string> limiting;
    for (const auto &resource : group.resources) {
        if (!supports(resource, samples)) {
            limiting.push_back(resource.resource + " (" + resource.format + ")");
        }
    }
    return limiting;
}

std::uint32_t selectPreferred(const SampleCountGroupRequest &group,
                              std::uint32_t requested) {
    for (auto candidate = requested; candidate >= 1; candidate >>= 1) {
        const auto supported = std::all_of(
            group.resources.begin(), group.resources.end(),
            [candidate](const auto &resource) {
                return supports(resource, candidate);
            });
        if (supported) return candidate;
        if (candidate == 1) break;
    }
    throw std::runtime_error("sample-count group '" + group.id +
                             "' has no common supported sample count");
}

} // namespace

std::string_view sampleCountRequestModeName(SampleCountRequestMode mode) {
    switch (mode) {
    case SampleCountRequestMode::automatic: return "automatic";
    case SampleCountRequestMode::prefer: return "prefer";
    case SampleCountRequestMode::exact: return "exact";
    }
    return "unknown";
}

std::string_view sampleCountScopeName(SampleCountScope scope) {
    switch (scope) {
    case SampleCountScope::geometry: return "geometry";
    case SampleCountScope::all: return "all";
    case SampleCountScope::none: return "none";
    }
    return "unknown";
}

SampleCountPolicy compileSampleCountPolicy(
    const nlohmann::json &config) {
    if (!config.is_object()) {
        throw std::runtime_error(
            "rendering config must be an object for sample-count policy");
    }

    SampleCountPolicy result;
    result.request = {SampleCountRequestMode::exact, 1};
    if (!config.contains("multisampling")) return result;

    result.authored = true;
    result.request = {SampleCountRequestMode::exact, 4};
    const auto &value = config.at("multisampling");
    if (!value.is_object()) {
        throw std::runtime_error("multisampling must be an object");
    }
    static const std::set<std::string, std::less<>> allowed{
        "mode", "samples", "fallback", "scope", "targets"};
    for (auto field = value.begin(); field != value.end(); ++field) {
        if (!allowed.contains(field.key())) {
            throw std::runtime_error(
                "multisampling has unknown key '" + field.key() + "'");
        }
    }
    if (value.contains("mode") && value.contains("fallback")) {
        throw std::runtime_error(
            "multisampling cannot specify both mode and fallback");
    }

    if (value.contains("fallback")) {
        if (!value.at("fallback").is_string()) {
            throw std::runtime_error(
                "multisampling fallback must be a string");
        }
        const auto fallback = value.at("fallback").get<std::string>();
        if (fallback == "error") {
            result.request.mode = SampleCountRequestMode::exact;
        } else if (fallback == "lower_supported") {
            result.request.mode = SampleCountRequestMode::prefer;
        } else {
            throw std::runtime_error(
                "multisampling fallback must be error or lower_supported");
        }
    } else if (value.contains("mode")) {
        if (!value.at("mode").is_string()) {
            throw std::runtime_error(
                "multisampling mode must be a string");
        }
        const auto mode = value.at("mode").get<std::string>();
        if (mode == "automatic" || mode == "auto") {
            result.request.mode = SampleCountRequestMode::automatic;
        } else if (mode == "prefer") {
            result.request.mode = SampleCountRequestMode::prefer;
        } else if (mode == "exact") {
            result.request.mode = SampleCountRequestMode::exact;
        } else {
            throw std::runtime_error(
                "multisampling mode must be automatic, prefer, or exact");
        }
    }

    if (value.contains("samples")) {
        const auto &samples = value.at("samples");
        if (samples.is_number_unsigned()) {
            const auto parsed = samples.get<std::uint64_t>();
            if (parsed > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error(
                    "multisampling samples exceeds uint32 range");
            }
            result.request.samples =
                static_cast<std::uint32_t>(parsed);
        } else if (samples.is_number_integer()) {
            const auto parsed = samples.get<std::int64_t>();
            if (parsed <= 0) {
                throw std::runtime_error(
                    "multisampling samples must be positive");
            }
            if (static_cast<std::uint64_t>(parsed) >
                std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error(
                    "multisampling samples exceeds uint32 range");
            }
            result.request.samples =
                static_cast<std::uint32_t>(parsed);
        } else {
            throw std::runtime_error(
                "multisampling samples must be an unsigned integer");
        }
    }
    if (!validSampleCount(result.request.samples)) {
        throw std::runtime_error(
            "multisampling samples must be a power of two between 1 and 64");
    }

    if (value.contains("scope") && !value.at("scope").is_string()) {
        throw std::runtime_error(
            "multisampling scope must be a string");
    }
    const auto scope = value.value("scope", std::string{"geometry"});
    if (scope == "geometry") {
        result.scope = SampleCountScope::geometry;
    } else if (scope == "all") {
        result.scope = SampleCountScope::all;
    } else if (scope == "none") {
        result.scope = SampleCountScope::none;
    } else {
        throw std::runtime_error(
            "multisampling scope must be geometry, all, or none");
    }

    if (value.contains("targets")) {
        if (!value.at("targets").is_array()) {
            throw std::runtime_error(
                "multisampling targets must be an array");
        }
        for (const auto &target : value.at("targets")) {
            if (!target.is_string() ||
                target.get_ref<const std::string &>().empty()) {
                throw std::runtime_error(
                    "multisampling targets must contain non-empty strings");
            }
            result.targets.push_back(target.get<std::string>());
        }
        std::sort(result.targets.begin(), result.targets.end());
        if (std::adjacent_find(result.targets.begin(),
                               result.targets.end()) !=
            result.targets.end()) {
            throw std::runtime_error(
                "multisampling targets must not contain duplicates");
        }
    }
    return result;
}

nlohmann::ordered_json sampleCountPolicyToJson(
    const SampleCountPolicy &policy) {
    return {
        {"mode", sampleCountRequestModeName(policy.request.mode)},
        {"samples", policy.request.samples},
        {"scope", sampleCountScopeName(policy.scope)},
        {"targets", policy.targets},
    };
}

ResolvedSampleCountPlan resolveSampleCountPlan(
    SampleCountRequest request,
    std::span<const SampleCountGroupRequest> input_groups) {
    if (!validSampleCount(request.samples)) {
        throw std::runtime_error("requested sample count must be a power of two "
                                 "between 1 and 64");
    }

    auto groups = std::vector<SampleCountGroupRequest>{
        input_groups.begin(), input_groups.end()};
    std::sort(groups.begin(), groups.end(), [](const auto &left,
                                               const auto &right) {
        return left.id < right.id;
    });

    std::set<std::string, std::less<>> group_ids;
    std::set<std::string, std::less<>> resource_names;
    for (auto &group : groups) {
        if (group.id.empty() || !group_ids.insert(group.id).second) {
            throw std::runtime_error(
                "sample-count group ids must be non-empty and unique: " +
                group.id);
        }
        if (group.resources.empty()) {
            throw std::runtime_error("sample-count group '" + group.id +
                                     "' has no resources");
        }
        std::sort(group.resources.begin(), group.resources.end(),
                  [](const auto &left, const auto &right) {
                      return left.resource < right.resource;
                  });
        for (auto &resource : group.resources) {
            if (resource.resource.empty() ||
                !resource_names.insert(resource.resource).second) {
                throw std::runtime_error(
                    "sample-count resources must be non-empty and belong to "
                    "exactly one group: " +
                    resource.resource);
            }
            if (resource.format.empty()) {
                throw std::runtime_error("sample-count resource '" +
                                         resource.resource +
                                         "' has no format");
            }
            resource.supported_samples = canonicalCounts(
                std::move(resource.supported_samples), resource.resource);
            if (!supports(resource, 1)) {
                throw std::runtime_error("sample-count resource '" +
                                         resource.resource +
                                         "' must support 1 sample");
            }
        }
    }

    ResolvedSampleCountPlan result;
    result.request = request;
    for (const auto &group : groups) {
        ResolvedSampleCountGroup resolved;
        resolved.id = group.id;
        resolved.requested_samples =
            group.multisampling_enabled ? request.samples : 1;

        if (!group.multisampling_enabled || request.samples == 1) {
            resolved.selected_samples = 1;
            resolved.reason = group.multisampling_enabled
                                  ? "single_sample_requested"
                                  : "group_not_selected";
        } else if (request.mode == SampleCountRequestMode::exact) {
            resolved.limiting_resources =
                limitingResources(group, request.samples);
            if (!resolved.limiting_resources.empty()) {
                std::string detail;
                for (const auto &resource : resolved.limiting_resources) {
                    if (!detail.empty()) detail += ", ";
                    detail += resource;
                }
                throw std::runtime_error(
                    "sample-count group '" + group.id + "' cannot satisfy exact " +
                    std::to_string(request.samples) + "x; limiting resources: " +
                    detail);
            }
            resolved.selected_samples = request.samples;
            resolved.reason = "exact_supported";
        } else {
            resolved.selected_samples =
                selectPreferred(group, request.samples);
            resolved.fallback =
                resolved.selected_samples != request.samples;
            if (resolved.fallback) {
                resolved.limiting_resources =
                    limitingResources(group, request.samples);
                resolved.reason =
                    request.mode == SampleCountRequestMode::automatic
                        ? "automatic_common_fallback"
                        : "preferred_common_fallback";
            } else {
                resolved.reason =
                    request.mode == SampleCountRequestMode::automatic
                        ? "automatic_supported"
                        : "preferred_supported";
            }
        }

        for (const auto &resource : group.resources) {
            result.resources.push_back(ResolvedSampleCountResource{
                resource.resource, resource.format,
                resolved.selected_samples});
        }
        result.groups.push_back(std::move(resolved));
    }
    std::sort(result.resources.begin(), result.resources.end(),
              [](const auto &left, const auto &right) {
                  return left.resource < right.resource;
              });
    return result;
}

nlohmann::ordered_json resolvedSampleCountPlanToJson(
    const ResolvedSampleCountPlan &plan) {
    nlohmann::ordered_json groups = nlohmann::ordered_json::array();
    for (const auto &group : plan.groups) {
        groups.push_back({
            {"id", group.id},
            {"requested_samples", group.requested_samples},
            {"selected_samples", group.selected_samples},
            {"fallback", group.fallback},
            {"limiting_resources", group.limiting_resources},
            {"reason", group.reason},
        });
    }
    nlohmann::ordered_json resources = nlohmann::ordered_json::array();
    for (const auto &resource : plan.resources) {
        resources.push_back({
            {"resource", resource.resource},
            {"format", resource.format},
            {"samples", resource.samples},
        });
    }
    return {
        {"request",
         {{"mode", sampleCountRequestModeName(plan.request.mode)},
          {"samples", plan.request.samples}}},
        {"groups", std::move(groups)},
        {"resources", std::move(resources)},
    };
}

} // namespace Pelican
