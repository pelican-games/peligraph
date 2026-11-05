#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Pelican {

struct GraphFilterCategory {
    std::string id;
    std::string label;
    bool operator==(const GraphFilterCategory &) const = default;
};

struct GraphFilterMatch {
    std::vector<std::string> ops;
    std::vector<std::string> node_ids;
    std::vector<std::string> features;
    std::vector<std::string> feature_categories;
    bool operator==(const GraphFilterMatch &) const = default;
};

struct GraphFilter {
    std::string id;
    std::string label;
    GraphFilterMatch match;
    std::vector<GraphFilter> children;
    bool operator==(const GraphFilter &) const = default;
};

struct GraphFilters {
    std::vector<GraphFilterCategory> categories;
    std::vector<std::pair<std::string, std::vector<std::string>>> feature_categories;
    std::vector<GraphFilter> filters;
    bool operator==(const GraphFilters &) const = default;
};

struct GraphFiltersParse {
    std::optional<GraphFilters> document;
    std::string error;
};

struct GraphFiltersMerge {
    GraphFilters merged;
    std::vector<std::string> skipped_filters;
    std::vector<std::string> skipped_categories;
};

// Strict, whole-document parsing. Failure is reported rather than thrown.
[[nodiscard]] GraphFiltersMerge mergeGraphFilters(const GraphFilters &base, const GraphFilters &incoming);
[[nodiscard]] GraphFiltersParse parseGraphFilters(const nlohmann::json &root);
// Canonical two-space JSON, with a trailing newline.
[[nodiscard]] std::string renderGraphFilters(const GraphFilters &document);

} // namespace Pelican
