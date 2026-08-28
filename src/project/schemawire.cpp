#include "schemawire.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace Pelican {
namespace {

[[noreturn]] void wireError(Schema::ErrorCode code, std::string path,
                            std::string detail) {
    throw Schema::Error{code, std::move(path), std::move(detail)};
}

const nlohmann::json &required(const nlohmann::json &object,
                               std::string_view name,
                               std::string_view path) {
    if (!object.is_object()) {
        wireError(Schema::ErrorCode::TypeMismatch, std::string{path},
                  "schema field must be an object");
    }
    const auto found = object.find(name);
    if (found == object.end()) {
        wireError(Schema::ErrorCode::TypeMismatch,
                  std::string{path} + "." + std::string{name},
                  "required field is missing");
    }
    return *found;
}

std::int64_t signedInteger(const nlohmann::json &value,
                           std::string path) {
    if (value.is_number_unsigned()) {
        const auto source = value.get<std::uint64_t>();
        if (source <=
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(source);
        }
    } else if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    wireError(Schema::ErrorCode::TypeMismatch, std::move(path),
              "range bound must be a signed integer");
}

std::uint64_t unsignedInteger(const nlohmann::json &value,
                              std::string path) {
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    if (value.is_number_integer()) {
        const auto source = value.get<std::int64_t>();
        if (source >= 0) return static_cast<std::uint64_t>(source);
    }
    wireError(Schema::ErrorCode::TypeMismatch, std::move(path),
              "range bound must be a non-negative integer");
}

double floating(const nlohmann::json &value, std::string path) {
    if (!value.is_number()) {
        wireError(Schema::ErrorCode::TypeMismatch, std::move(path),
                  "range bound must be a number");
    }
    const double result = value.get<double>();
    if (!std::isfinite(result)) {
        wireError(Schema::ErrorCode::RangeBoundNotFinite, std::move(path),
                  "range bound must be finite");
    }
    return result;
}

} // namespace

Schema::FieldDeclaration parseSchemaFieldDeclaration(
    const nlohmann::json &wire, std::string_view path) {
    const auto &name_json = required(wire, "name", path);
    const auto &type_json = required(wire, "type", path);
    if (!name_json.is_string()) {
        wireError(Schema::ErrorCode::TypeMismatch,
                  std::string{path} + ".name", "field name must be a string");
    }
    if (!type_json.is_string()) {
        wireError(Schema::ErrorCode::TypeMismatch,
                  std::string{path} + ".type", "field type must be a string");
    }

    Schema::FieldDeclaration result{
        .name = name_json.get<std::string>(),
        .type = Schema::fieldTypeFromName(
            type_json.get_ref<const std::string &>(),
            std::string{path} + ".type"),
    };
    const auto &rule = Schema::typeRule(result.type);

    if (const auto range = wire.find("range"); range != wire.end()) {
        const std::string range_path = std::string{path} + ".range";
        if (!range->is_array() || range->size() != 2) {
            wireError(Schema::ErrorCode::TypeMismatch, range_path,
                      "range must contain exactly two bounds");
        }
        switch (rule.range_kind) {
        case Schema::RangeKind::Signed:
            result.range = Schema::SignedRange{
                signedInteger((*range)[0], range_path + "[0]"),
                signedInteger((*range)[1], range_path + "[1]")};
            break;
        case Schema::RangeKind::Unsigned:
            result.range = Schema::UnsignedRange{
                unsignedInteger((*range)[0], range_path + "[0]"),
                unsignedInteger((*range)[1], range_path + "[1]")};
            break;
        case Schema::RangeKind::Floating:
            result.range = Schema::FloatingRange{
                floating((*range)[0], range_path + "[0]"),
                floating((*range)[1], range_path + "[1]")};
            break;
        case Schema::RangeKind::None:
            wireError(Schema::ErrorCode::RangeNotApplicable, range_path,
                      "range does not apply to this field type");
        }
    }

    if (const auto unit = wire.find("unit"); unit != wire.end()) {
        if (!unit->is_string()) {
            wireError(Schema::ErrorCode::TypeMismatch,
                      std::string{path} + ".unit", "unit must be a string");
        }
        result.unit = unit->get<std::string>();
    }
    if (const auto values = wire.find("enum"); values != wire.end()) {
        if (!values->is_array()) {
            wireError(Schema::ErrorCode::TypeMismatch,
                      std::string{path} + ".enum",
                      "enum values must be an array");
        }
        for (std::size_t index = 0; index < values->size(); ++index) {
            if (!(*values)[index].is_string()) {
                wireError(Schema::ErrorCode::TypeMismatch,
                          std::string{path} + ".enum[" +
                              std::to_string(index) + "]",
                          "enum value must be a string");
            }
            result.enum_values.push_back((*values)[index].get<std::string>());
        }
    }
    if (const auto default_value = wire.find("default");
        default_value != wire.end()) {
        result.default_value = *default_value;
    }

    Schema::validateDeclaration(result, path);
    return result;
}

} // namespace Pelican
