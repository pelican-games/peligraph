#include "graphfilters.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <set>
#include <system_error>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

// 1 MiB. Larger files are refused before they are parsed, and are not read
// into memory beyond this.
constexpr std::size_t kMaxFilterBytes = 1024u * 1024u;
constexpr int kMaxFilterDepth = 8;

std::string pathText(const std::filesystem::path &path) {
    return path.generic_string();
}

bool validId(std::string_view id) {
    if (id.empty() || id.front() < 'a' || id.front() > 'z') return false;
    return std::ranges::all_of(id, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

bool onlyKeys(const Json &object, std::initializer_list<std::string_view> allowed,
              std::string &error) {
    for (auto entry = object.begin(); entry != object.end(); ++entry) {
        const bool known = std::ranges::any_of(allowed, [&](std::string_view key) {
            return std::string_view{entry.key()} == key;
        });
        if (!known) {
            error = "unknown key \"" + entry.key() + "\"";
            return false;
        }
    }
    return true;
}

bool nonEmptyString(const Json &value, std::string &out, std::string &error,
                    const std::string &where) {
    if (!value.is_string()) {
        error = where + " is not a string";
        return false;
    }
    out = value.get<std::string>();
    if (out.empty()) {
        error = where + " is empty";
        return false;
    }
    return true;
}

bool readStringArray(const Json &value, std::vector<std::string> &out, std::string &error,
                     const std::string &where) {
    if (!value.is_array()) {
        error = where + " is not an array";
        return false;
    }
    for (const auto &entry : value) {
        if (!entry.is_string()) {
            error = where + " holds a value that is not a string";
            return false;
        }
        auto text = entry.get<std::string>();
        if (text.empty()) {
            error = where + " holds an empty string";
            return false;
        }
        out.push_back(std::move(text));
    }
    return true;
}

bool readMatch(const Json &object, GraphFilterMatch &match,
               const std::set<std::string, std::less<>> &categories, std::string &error) {
    if (!onlyKeys(object, {"ops", "node_ids", "features", "feature_categories"}, error))
        return false;
    const auto read = [&](const char *key, std::vector<std::string> &out) {
        const auto found = object.find(key);
        if (found == object.end()) return true;
        return readStringArray(*found, out, error, std::string{"match."} + key);
    };
    if (!read("ops", match.ops) || !read("node_ids", match.node_ids) ||
        !read("features", match.features) || !read("feature_categories", match.feature_categories))
        return false;
    for (const auto &category : match.feature_categories)
        if (!categories.contains(category)) {
            error = "match names undeclared category \"" + category + "\"";
            return false;
        }
    return true;
}

bool readFilter(const Json &object, int depth,
                const std::set<std::string, std::less<>> &categories,
                std::set<std::string, std::less<>> &ids, GraphFilter &filter,
                std::string &error) {
    if (!object.is_object()) {
        error = "a filter is not an object";
        return false;
    }
    if (depth > kMaxFilterDepth) {
        error = "filters nest deeper than " + std::to_string(kMaxFilterDepth);
        return false;
    }

    const auto id_entry = object.find("id");
    if (id_entry == object.end() || !nonEmptyString(*id_entry, filter.id, error, "filter id"))
        return false;
    if (!validId(filter.id)) {
        error = "filter id \"" + filter.id + "\" is not [a-z][a-z0-9_]*";
        return false;
    }
    if (!ids.insert(filter.id).second) {
        error = "filter id \"" + filter.id + "\" is repeated";
        return false;
    }

    const auto label_entry = object.find("label");
    if (label_entry == object.end() ||
        !nonEmptyString(*label_entry, filter.label, error, "filter \"" + filter.id + "\" label"))
        return false;

    if (!onlyKeys(object, {"id", "label", "match", "children"}, error)) {
        error = "filter \"" + filter.id + "\": " + error;
        return false;
    }

    const auto match_entry = object.find("match");
    if (match_entry == object.end() || !match_entry->is_object()) {
        error = "filter \"" + filter.id + "\" has no match object";
        return false;
    }
    if (!readMatch(*match_entry, filter.match, categories, error)) {
        error = "filter \"" + filter.id + "\": " + error;
        return false;
    }

    const auto children = object.find("children");
    if (children != object.end()) {
        if (!children->is_array()) {
            error = "filter \"" + filter.id + "\" children is not an array";
            return false;
        }
        for (const auto &child : *children) {
            GraphFilter nested;
            if (!readFilter(child, depth + 1, categories, ids, nested, error)) return false;
            filter.children.push_back(std::move(nested));
        }
    }
    return true;
}

bool readCategories(const Json &array, std::vector<GraphFilterCategory> &categories,
                    std::set<std::string, std::less<>> &ids, std::string &error) {
    if (!array.is_array()) {
        error = "categories is not an array";
        return false;
    }
    for (const auto &entry : array) {
        if (!entry.is_object()) {
            error = "a category is not an object";
            return false;
        }
        GraphFilterCategory category;
        const auto id_entry = entry.find("id");
        if (id_entry == entry.end() ||
            !nonEmptyString(*id_entry, category.id, error, "category id"))
            return false;
        if (!validId(category.id)) {
            error = "category id \"" + category.id + "\" is not [a-z][a-z0-9_]*";
            return false;
        }
        if (!ids.insert(category.id).second) {
            error = "category id \"" + category.id + "\" is repeated";
            return false;
        }
        const auto label_entry = entry.find("label");
        if (label_entry == entry.end() ||
            !nonEmptyString(*label_entry, category.label, error,
                            "category \"" + category.id + "\" label"))
            return false;
        if (!onlyKeys(entry, {"id", "label"}, error)) {
            error = "category \"" + category.id + "\": " + error;
            return false;
        }
        categories.push_back(std::move(category));
    }
    return true;
}

bool readFeatureCategories(const Json &object,
                           const std::set<std::string, std::less<>> &categories,
                           std::vector<std::pair<std::string, std::vector<std::string>>> &out,
                           std::string &error) {
    if (!object.is_object()) {
        error = "feature_categories is not an object";
        return false;
    }
    for (auto entry = object.begin(); entry != object.end(); ++entry) {
        if (entry.key().empty()) {
            error = "feature_categories has an empty feature name";
            return false;
        }
        std::vector<std::string> named;
        if (!readStringArray(entry.value(), named, error,
                             "feature_categories.\"" + entry.key() + "\""))
            return false;
        for (const auto &category : named)
            if (!categories.contains(category)) {
                error = "feature \"" + entry.key() + "\" names undeclared category \"" +
                        category + "\"";
                return false;
            }
        out.emplace_back(entry.key(), std::move(named));
    }
    return true;
}

GraphFiltersRead invalid(const std::filesystem::path &path, const std::string &reason) {
    GraphFiltersRead read;
    read.error = pathText(path) + ": " + reason;
    return read;
}

const GraphFilter *findIn(const std::vector<GraphFilter> &filters, std::string_view id) {
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
        if (text.size() > kMaxFilterBytes)
            return invalid(path, "the file is larger than 1 MiB");

        Json root;
        try {
            root = Json::parse(text);
        } catch (const Json::parse_error &error) {
            return invalid(path, "invalid JSON at byte " + std::to_string(error.byte) + ": " +
                                     error.what());
        }
        if (!root.is_object()) return invalid(path, "the top level is not an object");

        std::string detail;
        if (!onlyKeys(root, {"schema", "version", "categories", "feature_categories", "filters"},
                      detail))
            return invalid(path, detail);

        const auto schema = root.find("schema");
        if (schema == root.end() || !schema->is_string() ||
            schema->get<std::string>() != "pelican.graph_filters")
            return invalid(path, "schema is not \"pelican.graph_filters\"");
        const auto version = root.find("version");
        if (version == root.end() || !version->is_number_integer())
            return invalid(path, "version is not the integer 1");
        const bool version_one =
            version->is_number_unsigned() ? version->get<std::uint64_t>() == 1
                                          : version->get<std::int64_t>() == 1;
        if (!version_one) return invalid(path, "version is not the integer 1");

        GraphFilters document;
        std::set<std::string, std::less<>> category_ids;
        const auto categories = root.find("categories");
        if (categories != root.end() &&
            !readCategories(*categories, document.categories, category_ids, detail))
            return invalid(path, detail);

        const auto feature_categories = root.find("feature_categories");
        if (feature_categories != root.end() &&
            !readFeatureCategories(*feature_categories, category_ids,
                                   document.feature_categories, detail))
            return invalid(path, detail);

        const auto filters = root.find("filters");
        if (filters == root.end() || !filters->is_array())
            return invalid(path, "filters is not an array");
        std::set<std::string, std::less<>> filter_ids;
        for (const auto &entry : *filters) {
            GraphFilter filter;
            if (!readFilter(entry, 1, category_ids, filter_ids, filter, detail))
                return invalid(path, detail);
            document.filters.push_back(std::move(filter));
        }
        GraphFiltersRead read;
        read.document = std::move(document);
        return read;
    } catch (const std::exception &error) {
        return invalid(path, std::string{"could not be read: "} + error.what());
    } catch (...) {
        return invalid(path, "could not be read");
    }
}

const GraphFilter *findGraphFilter(const GraphFilters &filters, std::string_view id) {
    return findIn(filters.filters, id);
}

bool graphFilterMatches(const GraphFilter &filter, const GraphFilters &document,
                        const GraphFilterFacts &facts) {
    const auto &match = filter.match;
    if (facts.kind == GraphFilterFacts::Kind::node) {
        for (const auto &op : match.ops) {
            if (op.empty()) continue;
            if (op.back() == '.') {
                if (facts.op.starts_with(op)) return true;
            } else if (facts.op == op) {
                return true;
            }
        }
        return std::ranges::find(match.node_ids, facts.node_id) != match.node_ids.end();
    }
    if (facts.kind != GraphFilterFacts::Kind::folded_feature) return false;
    if (std::ranges::find(match.features, facts.feature_name) != match.features.end())
        return true;
    return std::ranges::any_of(match.feature_categories, [&](const std::string &category) {
        return std::ranges::any_of(document.feature_categories, [&](const auto &entry) {
            return entry.first == facts.feature_name &&
                   std::ranges::find(entry.second, category) != entry.second.end();
        });
    });
}

} // namespace PelicanStudio
