#include "graphfiltersdocument.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <string_view>

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;
constexpr int kMaxFilterDepth = 8;

bool validId(std::string_view id) {
    if (id.empty() || id.front() < 'a' || id.front() > 'z') return false;
    return std::ranges::all_of(id, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

bool onlyKeys(const Json &object, std::initializer_list<std::string_view> allowed,
              std::string &error) {
    if (!object.is_object()) {
        error = "expected an object";
        return false;
    }
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
            !nonEmptyString(*id_entry, category.id, error, "category id")) return false;
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
                            "category \"" + category.id + "\" label")) return false;
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
                             "feature_categories.\"" + entry.key() + "\"")) return false;
        for (const auto &category : named)
            if (!categories.contains(category)) {
                error = "feature \"" + entry.key() + "\" names undeclared category \"" + category + "\"";
                return false;
            }
        out.emplace_back(entry.key(), std::move(named));
    }
    return true;
}

OrderedJson matchJson(const GraphFilterMatch &match) {
    OrderedJson out = OrderedJson::object();
    if (!match.ops.empty()) out["ops"] = match.ops;
    if (!match.node_ids.empty()) out["node_ids"] = match.node_ids;
    if (!match.features.empty()) out["features"] = match.features;
    if (!match.feature_categories.empty()) out["feature_categories"] = match.feature_categories;
    return out;
}

OrderedJson filterJson(const GraphFilter &filter) {
    OrderedJson out{{"id", filter.id}, {"label", filter.label}, {"match", matchJson(filter.match)}};
    if (!filter.children.empty()) {
        out["children"] = OrderedJson::array();
        for (const auto &child : filter.children) out["children"].push_back(filterJson(child));
    }
    return out;
}

} // namespace

GraphFiltersParse parseGraphFilters(const nlohmann::json &root) {
    GraphFiltersParse result;
    try {
        std::string detail;
        if (!root.is_object()) {
            result.error = "the top level is not an object";
            return result;
        }
        if (!onlyKeys(root, {"schema", "version", "categories", "feature_categories", "filters"}, detail)) {
            result.error = std::move(detail);
            return result;
        }
        const auto schema = root.find("schema");
        if (schema == root.end() || !schema->is_string() ||
            schema->get<std::string>() != "pelican.graph_filters") {
            result.error = "schema is not \"pelican.graph_filters\"";
            return result;
        }
        const auto version = root.find("version");
        if (version == root.end() || !version->is_number_integer()) {
            result.error = "version is not the integer 1";
            return result;
        }
        const bool version_one = version->is_number_unsigned()
                                     ? version->get<std::uint64_t>() == 1
                                     : version->get<std::int64_t>() == 1;
        if (!version_one) {
            result.error = "version is not the integer 1";
            return result;
        }
        GraphFilters document;
        std::set<std::string, std::less<>> category_ids;
        const auto categories = root.find("categories");
        if (categories != root.end() &&
            !readCategories(*categories, document.categories, category_ids, detail)) {
            result.error = std::move(detail);
            return result;
        }
        const auto feature_categories = root.find("feature_categories");
        if (feature_categories != root.end() &&
            !readFeatureCategories(*feature_categories, category_ids, document.feature_categories, detail)) {
            result.error = std::move(detail);
            return result;
        }
        const auto filters = root.find("filters");
        if (filters == root.end() || !filters->is_array()) {
            result.error = "filters is not an array";
            return result;
        }
        std::set<std::string, std::less<>> filter_ids;
        for (const auto &entry : *filters) {
            GraphFilter filter;
            if (!readFilter(entry, 1, category_ids, filter_ids, filter, detail)) {
                result.error = std::move(detail);
                return result;
            }
            document.filters.push_back(std::move(filter));
        }
        result.document = std::move(document);
    } catch (const std::exception &error) {
        result.document.reset();
        result.error = std::string{"could not be read: "} + error.what();
    } catch (...) {
        result.document.reset();
        result.error = "could not be read";
    }
    return result;
}

GraphFiltersMerge mergeGraphFilters(const GraphFilters &base, const GraphFilters &incoming) {
    GraphFiltersMerge result{base, {}, {}};
    std::set<std::string, std::less<>> filter_ids;
    const auto collect = [&](const auto &self, const GraphFilter &filter) -> void {
        filter_ids.insert(filter.id);
        for (const auto &child : filter.children) self(self, child);
    };
    for (const auto &filter : base.filters) collect(collect, filter);
    const auto collides = [&](const auto &self, const GraphFilter &filter) -> bool {
        if (filter_ids.contains(filter.id)) return true;
        return std::ranges::any_of(filter.children, [&](const auto &child) { return self(self, child); });
    };
    for (const auto &filter : incoming.filters) {
        if (collides(collides, filter)) result.skipped_filters.push_back(filter.id);
        else {
            result.merged.filters.push_back(filter);
            collect(collect, filter);
        }
    }
    for (const auto &category : incoming.categories) {
        const auto found = std::ranges::find(base.categories, category.id, &GraphFilterCategory::id);
        if (found == base.categories.end()) result.merged.categories.push_back(category);
        else if (found->label != category.label) result.skipped_categories.push_back(category.id);
    }
    for (const auto &[feature, incoming_ids] : incoming.feature_categories) {
        auto found = std::ranges::find_if(result.merged.feature_categories, [&](const auto &entry) {
            return entry.first == feature;
        });
        if (found == result.merged.feature_categories.end())
            found = result.merged.feature_categories.insert(result.merged.feature_categories.end(), {feature, {}});
        for (const auto &id : incoming_ids)
            if (std::ranges::find(found->second, id) == found->second.end()) found->second.push_back(id);
    }
    return result;
}

std::string renderGraphFilters(const GraphFilters &document) {
    OrderedJson root{{"schema", "pelican.graph_filters"}, {"version", 1}, {"categories", OrderedJson::array()},
                     {"feature_categories", OrderedJson::object()}, {"filters", OrderedJson::array()}};
    for (const auto &category : document.categories)
        root["categories"].push_back({{"id", category.id}, {"label", category.label}});
    for (const auto &[feature, categories] : document.feature_categories)
        root["feature_categories"][feature] = categories;
    for (const auto &filter : document.filters) root["filters"].push_back(filterJson(filter));
    return root.dump(2) + '\n';
}

} // namespace Pelican
