#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {

// GF-1a: a project's graph_filters.json. A filter narrows the root canvas to
// what it names; it is a session choice and never edits the document.
struct GraphFilterCategory {
    std::string id;
    std::string label;

    bool operator==(const GraphFilterCategory &) const = default;
};

struct GraphFilterMatch {
    // An entry that ends in '.' is a prefix: "canvas." takes canvas.stack.
    // Otherwise the operation is named exactly.
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
    // Display hierarchy only. A child's match is not combined with its
    // parent's: picking either chooses exactly the match it carries.
    std::vector<GraphFilter> children;

    bool operator==(const GraphFilter &) const = default;
};

struct GraphFilters {
    std::vector<GraphFilterCategory> categories;
    // Project-local feature name -> the declared categories it belongs to.
    std::vector<std::pair<std::string, std::vector<std::string>>> feature_categories;
    std::vector<GraphFilter> filters;

    bool operator==(const GraphFilters &) const = default;
};

// A read either yields a document or an error. A missing file is neither:
// the project simply has no filter menu. `error` is a sentence naming the
// path and the reason, and a malformed document is never partially accepted.
struct GraphFiltersRead {
    std::optional<GraphFilters> document;
    std::string error;
};

// The strict reader. A missing file returns neither a document nor an error.
// Every other failure -- unreadable, too large, invalid JSON, unknown key,
// bad id, undeclared category, too deep -- returns an error and no document.
// Parsing and validation never throw.
[[nodiscard]] GraphFiltersRead readGraphFilters(const std::filesystem::path &project_root);

// The filter named `id` at any depth, or nullptr.
[[nodiscard]] const GraphFilter *findGraphFilter(const GraphFilters &filters, std::string_view id);

// What the canvas is asking about. A folded node that is not a feature (a
// project pass, the rendering section) matches no filter: only a feature fold
// has a feature name to name.
struct GraphFilterFacts {
    enum class Kind { node, folded_feature, folded_other };
    Kind kind = Kind::node;
    std::string node_id;
    std::string op;
    std::string feature_name;
};

// True when the filter names the subject. The keys are ORed; the filter is
// then ANDed with the Show boxes by the model. An empty match names nothing.
[[nodiscard]] bool graphFilterMatches(const GraphFilter &filter, const GraphFilters &document,
                                      const GraphFilterFacts &facts);

} // namespace PelicanStudio
