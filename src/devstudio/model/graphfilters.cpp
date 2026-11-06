#include "graphfilters.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <system_error>

namespace PelicanStudio {
namespace {

constexpr std::size_t kMaxFilterBytes = 1024u * 1024u;

GraphFiltersRead invalid(const std::filesystem::path &path, const std::string &reason) {
    GraphFiltersRead read;
    read.error = path.generic_string() + ": " + reason;
    return read;
}

const Pelican::GraphFilter *findIn(const std::vector<Pelican::GraphFilter> &filters,
                                   std::string_view id) {
    for (const auto &filter : filters) {
        if (filter.id == id) return &filter;
        if (const auto *child = findIn(filter.children, id)) return child;
    }
    return nullptr;
}

} // namespace

GraphFiltersRead readGraphFilters(const std::filesystem::path &project_root) {
    if (project_root.empty()) return {};
    const auto path = project_root / "graph_filters.json";
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory) return {};
        return invalid(path, "could not read the file: " + ec.message());
    }
    if (size > kMaxFilterBytes) return invalid(path, "the file is larger than 1 MiB");
    try {
        std::ifstream file{path, std::ios::binary};
        if (!file) return invalid(path, "could not open the file");
        std::string text(kMaxFilterBytes + 1, '\0');
        file.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (file.bad()) return invalid(path, "could not read the file");
        text.resize(static_cast<std::size_t>(file.gcount()));
        if (text.size() > kMaxFilterBytes) return invalid(path, "the file is larger than 1 MiB");
        nlohmann::json root;
        try {
            root = nlohmann::json::parse(text);
        } catch (const nlohmann::json::parse_error &error) {
            return invalid(path, "invalid JSON at byte " + std::to_string(error.byte) + ": " + error.what());
        }
        auto parsed = Pelican::parseGraphFilters(root);
        if (!parsed.document) return invalid(path, parsed.error);
        GraphFiltersRead read;
        read.document = std::move(parsed.document);
        return read;
    } catch (const std::exception &error) {
        return invalid(path, std::string{"could not be read: "} + error.what());
    } catch (...) {
        return invalid(path, "could not be read");
    }
}

const Pelican::GraphFilter *findGraphFilter(const Pelican::GraphFilters &filters, std::string_view id) {
    return findIn(filters.filters, id);
}

bool graphFilterMatches(const Pelican::GraphFilter &filter, const Pelican::GraphFilters &document,
                        const GraphFilterFacts &facts) {
    const auto matchesOwn = [&] {
        const auto &match = filter.match;
        if (facts.kind == GraphFilterFacts::Kind::node ||
            facts.kind == GraphFilterFacts::Kind::folded_other) {
            bool matches = facts.kind == GraphFilterFacts::Kind::node &&
                           std::ranges::any_of(match.ops, [&](const std::string &op) {
                               return !op.empty() && (op.back() == '.' ? facts.op.starts_with(op)
                                                                       : facts.op == op);
                           });
            return matches || std::ranges::find(match.node_ids, facts.node_id) != match.node_ids.end();
        }
        if (facts.kind != GraphFilterFacts::Kind::folded_feature) return false;
        if (std::ranges::find(match.features, facts.feature_name) != match.features.end()) return true;
        return std::ranges::any_of(match.feature_categories, [&](const std::string &category) {
            return std::ranges::any_of(document.feature_categories, [&](const auto &entry) {
                return entry.first == facts.feature_name &&
                       std::ranges::find(entry.second, category) != entry.second.end();
            });
        });
    };
    return matchesOwn() || std::ranges::any_of(filter.children, [&](const Pelican::GraphFilter &child) {
        return graphFilterMatches(child, document, facts);
    });
}

} // namespace PelicanStudio
