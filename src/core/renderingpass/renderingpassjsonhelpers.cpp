#include "renderingpassjsonhelpers.hpp"
#include "../../project/logicalrendergraph.hpp"
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace Pelican {

std::string parseStringField(const nlohmann::json &json, std::string_view field_name,
                             std::string_view context) {
    if (!json.contains(field_name) || !json.at(field_name).is_string()) {
        throw std::runtime_error(std::string{context} + " requires string field: " +
                                 std::string{field_name});
    }
    return json.at(field_name).get<std::string>();
}

std::vector<std::string> parseStringList(const nlohmann::json &json,
                                         std::string_view context) {
    if (json.is_null()) {
        return {};
    }
    if (json.is_string()) {
        return {json.get<std::string>()};
    }
    if (!json.is_array()) {
        throw std::runtime_error(std::string{context} + " must be a string or string array");
    }

    std::vector<std::string> values;
    values.reserve(json.size());
    for (const auto &entry : json) {
        if (!entry.is_string()) {
            throw std::runtime_error(std::string{context} + " entries must be strings");
        }
        values.push_back(entry.get<std::string>());
    }
    return values;
}

std::vector<std::string> parseOptionalStringList(const nlohmann::json &json,
                                                 std::string_view field,
                                                 std::string_view context) {
    if (!json.contains(field)) {
        return {};
    }
    return parseStringList(json.at(field), std::string{context} + "." + std::string{field});
}

void appendUnique(std::vector<std::string> &values, std::string value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(std::move(value));
    }
}

void appendUnique(std::vector<std::string> &values,
                  const std::vector<std::string> &more_values) {
    for (const auto &value : more_values) {
        appendUnique(values, value);
    }
}

void validateName(const std::string &name, const std::string &context) {
    if (name.empty()) {
        throw std::runtime_error(context + " name must not be empty");
    }
}

float parseFloatField(const nlohmann::json &json, const std::string &field_name,
                      const std::string &context) {
    if (!json.contains(field_name) || !json.at(field_name).is_number()) {
        throw std::runtime_error(context + " requires numeric field: " + field_name);
    }
    return json.at(field_name).get<float>();
}

std::vector<std::string> parseStringArrayField(const nlohmann::json &json, const std::string &field_name,
                                               const std::string &context) {
    if (!json.contains(field_name) || !json.at(field_name).is_array()) {
        throw std::runtime_error(context + " requires string array field: " + field_name);
    }

    std::vector<std::string> values;
    for (const auto &value_json : json.at(field_name)) {
        if (!value_json.is_string()) {
            throw std::runtime_error(context + " requires string array field: " + field_name);
        }
        values.push_back(value_json.get<std::string>());
    }
    return values;
}

std::vector<std::string> parseOptionalRegionTags(
    const nlohmann::json &json, const std::string &context) {
    if (!json.contains("regions")) {
        return {};
    }
    if (!json.at("regions").is_array()) {
        throw std::runtime_error(
            context + " regions must be a string array");
    }
    std::vector<std::string> result;
    result.reserve(json.at("regions").size());
    std::set<std::string, std::less<>> unique;
    for (const auto &encoded : json.at("regions")) {
        if (!encoded.is_string()) {
            throw std::runtime_error(
                context + " regions must be a string array");
        }
        auto region = encoded.get<std::string>();
        if (region.empty() ||
            region.size() > maximumLogicalRegionTagBytes) {
            throw std::runtime_error(
                context + " region tag is empty or too long");
        }
        if (!unique.insert(region).second) {
            throw std::runtime_error(
                context + " has duplicate region tag: " + region);
        }
        result.push_back(std::move(region));
    }
    return result;
}

RenderViewSelect parseRenderViewSelect(
    const nlohmann::json &json,
    const std::string &context) {
    RenderViewSelect result;
    const auto found = json.find("view_select");
    if (found == json.end()) {
        return result;
    }
    if (!found->is_object() || found->empty()) {
        throw std::runtime_error(
            context +
            " view_select must be a non-empty object of axis names "
            "to non-negative integers");
    }
    for (auto entry = found->begin(); entry != found->end();
         ++entry) {
        if (entry.key().empty()) {
            throw std::runtime_error(
                context +
                " view_select has an empty axis name");
        }
        const auto &value = entry.value();
        if (!value.is_number_integer() ||
            (value.is_number_unsigned()
                 ? value.get<std::uint64_t>() >
                       std::numeric_limits<std::uint32_t>::max()
                 : value.get<std::int64_t>() < 0 ||
                       value.get<std::int64_t>() >
                           static_cast<std::int64_t>(
                               std::numeric_limits<
                                   std::uint32_t>::max()))) {
            throw std::runtime_error(
                context + " view_select axis '" + entry.key() +
                "' must be a non-negative 32-bit integer");
        }
        result.emplace(
            entry.key(),
            static_cast<std::uint32_t>(
                value.is_number_unsigned()
                    ? value.get<std::uint64_t>()
                    : static_cast<std::uint64_t>(
                          value.get<std::int64_t>())));
    }
    return result;
}

uint32_t parseUint32Field(const nlohmann::json &json, const std::string &field_name,
                          const std::string &context) {
    if (!json.contains(field_name) || !json.at(field_name).is_number_integer()) {
        throw std::runtime_error(context + " requires non-negative integer field: " + field_name);
    }

    uint64_t value = 0;
    const auto &field = json.at(field_name);
    if (field.is_number_unsigned()) {
        value = field.get<uint64_t>();
    } else {
        const int64_t signed_value = field.get<int64_t>();
        if (signed_value < 0) {
            throw std::runtime_error(context + " requires non-negative integer field: " + field_name);
        }
        value = static_cast<uint64_t>(signed_value);
    }

    if (value > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(context + " field is too large: " + field_name);
    }
    return static_cast<uint32_t>(value);
}
} // namespace Pelican
