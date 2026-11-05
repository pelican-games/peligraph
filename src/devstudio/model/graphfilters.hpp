#pragma once

#include "../../project/graphfiltersdocument.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace PelicanStudio {

using Pelican::GraphFilter;
using Pelican::GraphFilterCategory;
using Pelican::GraphFilterMatch;
using Pelican::GraphFilters;

struct GraphFiltersRead {
    std::optional<GraphFilters> document;
    std::string error;
};

// Reads <project_root>/graph_filters.json, then applies the shared strict parser.
// A missing file returns neither a document nor an error.
[[nodiscard]] GraphFiltersRead readGraphFilters(const std::filesystem::path &project_root);
[[nodiscard]] const GraphFilter *findGraphFilter(const GraphFilters &filters, std::string_view id);

struct GraphFilterFacts {
    enum class Kind { node, folded_feature, folded_other };
    Kind kind = Kind::node;
    std::string node_id;
    std::string op;
    std::string feature_name;
};

[[nodiscard]] bool graphFilterMatches(const GraphFilter &filter, const GraphFilters &document,
                                      const GraphFilterFacts &facts);

} // namespace PelicanStudio
