#include "schema.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace Pelican::Schema {
namespace {

constexpr auto i8Min = static_cast<std::int64_t>(std::numeric_limits<std::int8_t>::min());
constexpr auto i8Max = static_cast<std::int64_t>(std::numeric_limits<std::int8_t>::max());
constexpr auto i16Min = static_cast<std::int64_t>(std::numeric_limits<std::int16_t>::min());
constexpr auto i16Max = static_cast<std::int64_t>(std::numeric_limits<std::int16_t>::max());
constexpr auto i32Min = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min());
constexpr auto i32Max = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());

constexpr std::array<TypeRule, allFieldTypes.size()> Rules{
    TypeRule{FieldType::I8, "i8", JsonShape::SignedInteger, RangeKind::Signed, 0, false,
             i8Min, i8Max, 0},
    TypeRule{FieldType::I16, "i16", JsonShape::SignedInteger, RangeKind::Signed, 0, false,
             i16Min, i16Max, 0},
    TypeRule{FieldType::I32, "i32", JsonShape::SignedInteger, RangeKind::Signed, 0, false,
             i32Min, i32Max, 0},
    TypeRule{FieldType::I64, "i64", JsonShape::SignedInteger, RangeKind::Signed, 0, false,
             std::numeric_limits<std::int64_t>::min(),
             std::numeric_limits<std::int64_t>::max(), 0},
    TypeRule{FieldType::U8, "u8", JsonShape::UnsignedInteger, RangeKind::Unsigned, 0, false,
             0, 0, std::numeric_limits<std::uint8_t>::max()},
    TypeRule{FieldType::U16, "u16", JsonShape::UnsignedInteger, RangeKind::Unsigned, 0, false,
             0, 0, std::numeric_limits<std::uint16_t>::max()},
    TypeRule{FieldType::U32, "u32", JsonShape::UnsignedInteger, RangeKind::Unsigned, 0, false,
             0, 0, std::numeric_limits<std::uint32_t>::max()},
    TypeRule{FieldType::U64, "u64", JsonShape::UnsignedInteger, RangeKind::Unsigned, 0, false,
             0, 0, std::numeric_limits<std::uint64_t>::max()},
    TypeRule{FieldType::F32, "f32", JsonShape::FloatingPoint, RangeKind::Floating, 0, true,
             0, 0, 0},
    TypeRule{FieldType::F64, "f64", JsonShape::FloatingPoint, RangeKind::Floating, 0, false,
             0, 0, 0},
    TypeRule{FieldType::Vec2, "vec2", JsonShape::Vector, RangeKind::Floating, 2, true,
             0, 0, 0},
    TypeRule{FieldType::Vec3, "vec3", JsonShape::Vector, RangeKind::Floating, 3, true,
             0, 0, 0},
    TypeRule{FieldType::Vec4, "vec4", JsonShape::Vector, RangeKind::Floating, 4, true,
             0, 0, 0},
    TypeRule{FieldType::Quat, "quat", JsonShape::Vector, RangeKind::None, 4, true,
             0, 0, 0},
    TypeRule{FieldType::String, "string", JsonShape::String, RangeKind::None, 0, false,
             0, 0, 0},
    TypeRule{FieldType::Bool, "bool", JsonShape::Boolean, RangeKind::None, 0, false,
             0, 0, 0},
    TypeRule{FieldType::Enum, "enum", JsonShape::Enumeration, RangeKind::None, 0, false,
             0, 0, 0},
};

[[noreturn]] void fail(ErrorCode code, std::string_view path,
                       std::string detail) {
    throw Error{code, std::string{path}, std::move(detail)};
}

bool f32Representable(double value) {
    constexpr auto maximum =
        static_cast<double>(std::numeric_limits<float>::max());
    return std::isfinite(value) && value >= -maximum && value <= maximum;
}

std::int64_t resolveSigned(const TypeRule &rule, const FieldDeclaration &field,
                           const nlohmann::json &value,
                           std::string_view path) {
    std::int64_t result = 0;
    if (value.is_number_unsigned()) {
        const auto source = value.get<std::uint64_t>();
        if (source > maxExactJsonInteger ||
            source > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            fail(ErrorCode::TypeMismatch, path,
                 "is outside the exact JSON integer range");
        }
        result = static_cast<std::int64_t>(source);
    } else if (value.is_number_integer()) {
        result = value.get<std::int64_t>();
        if (result < -static_cast<std::int64_t>(maxExactJsonInteger) ||
            result > static_cast<std::int64_t>(maxExactJsonInteger)) {
            fail(ErrorCode::TypeMismatch, path,
                 "is outside the exact JSON integer range");
        }
    } else {
        fail(ErrorCode::TypeMismatch, path, "must be an integer token");
    }
    if (result < rule.signed_min || result > rule.signed_max) {
        fail(ErrorCode::TypeMismatch, path,
             "is not representable by its integer type");
    }
    if (const auto *range = std::get_if<SignedRange>(&field.range);
        range != nullptr && (result < range->min || result > range->max)) {
        fail(ErrorCode::OutOfRange, path, "is out of range");
    }
    return result;
}

std::uint64_t resolveUnsigned(const TypeRule &rule,
                              const FieldDeclaration &field,
                              const nlohmann::json &value,
                              std::string_view path) {
    std::uint64_t result = 0;
    if (value.is_number_unsigned()) {
        result = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto source = value.get<std::int64_t>();
        if (source < 0) {
            fail(ErrorCode::TypeMismatch, path, "must not be negative");
        }
        result = static_cast<std::uint64_t>(source);
    } else {
        fail(ErrorCode::TypeMismatch, path, "must be an integer token");
    }
    if (result > maxExactJsonInteger) {
        fail(ErrorCode::TypeMismatch, path,
             "is outside the exact JSON integer range");
    }
    if (result > rule.unsigned_max) {
        fail(ErrorCode::TypeMismatch, path,
             "is not representable by its integer type");
    }
    if (const auto *range = std::get_if<UnsignedRange>(&field.range);
        range != nullptr && (result < range->min || result > range->max)) {
        fail(ErrorCode::OutOfRange, path, "is out of range");
    }
    return result;
}

double resolveFloating(const TypeRule &rule, const FieldDeclaration &field,
                       const nlohmann::json &value, std::string_view path) {
    if (!value.is_number()) {
        fail(ErrorCode::TypeMismatch, path, "must be a number");
    }
    const double source = value.get<double>();
    if (!std::isfinite(source)) {
        fail(ErrorCode::TypeMismatch, path, "must be finite");
    }
    if (rule.components_require_f32 && !f32Representable(source)) {
        fail(ErrorCode::TypeMismatch, path, "is not representable as F32");
    }
    if (const auto *range = std::get_if<FloatingRange>(&field.range);
        range != nullptr && (source < range->min || source > range->max)) {
        fail(ErrorCode::OutOfRange, path, "is out of range");
    }
    return rule.components_require_f32
               ? static_cast<double>(static_cast<float>(source))
               : source;
}

} // namespace

std::string_view errorCodeName(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::UnknownTypeName: return "unknown_type_name";
    case ErrorCode::InvalidTypeOrdinal: return "invalid_type_ordinal";
    case ErrorCode::EmptyFieldName: return "empty_field_name";
    case ErrorCode::RangeNotApplicable: return "range_not_applicable";
    case ErrorCode::RangeKindMismatch: return "range_kind_mismatch";
    case ErrorCode::RangeMinimumExceedsMaximum: return "range_minimum_exceeds_maximum";
    case ErrorCode::RangeBoundNotRepresentable: return "range_bound_not_representable";
    case ErrorCode::RangeBoundNotFinite: return "range_bound_not_finite";
    case ErrorCode::EnumValuesRequired: return "enum_values_required";
    case ErrorCode::EnumValuesNotApplicable: return "enum_values_not_applicable";
    case ErrorCode::EnumTooManyValues: return "enum_too_many_values";
    case ErrorCode::EnumValueEmpty: return "enum_value_empty";
    case ErrorCode::EnumValueDuplicate: return "enum_value_duplicate";
    case ErrorCode::DuplicateFieldName: return "duplicate_field_name";
    case ErrorCode::PayloadMustBeObject: return "payload_must_be_object";
    case ErrorCode::UnknownField: return "unknown_field";
    case ErrorCode::MissingField: return "missing_field";
    case ErrorCode::TypeMismatch: return "type_mismatch";
    case ErrorCode::OutOfRange: return "out_of_range";
    case ErrorCode::InvalidEnum: return "invalid_enum";
    }
    return "invalid_error_code";
}

Error::Error(ErrorCode code, std::string path, std::string detail)
    : std::runtime_error{"schema_error[" + std::string{errorCodeName(code)} +
                         "] at '" + path + "': " + detail},
      code_{code}, path_{std::move(path)} {}

const std::array<TypeRule, allFieldTypes.size()> &typeRules() noexcept {
    return Rules;
}

const TypeRule &typeRule(FieldType type) {
    const auto found = std::find_if(
        Rules.begin(), Rules.end(),
        [type](const TypeRule &rule) { return rule.type == type; });
    if (found == Rules.end()) {
        fail(ErrorCode::InvalidTypeOrdinal, "type",
             "field type ordinal " +
                 std::to_string(static_cast<unsigned>(type)) + " is invalid");
    }
    return *found;
}

FieldType fieldTypeFromName(std::string_view name, std::string_view path) {
    const auto found = std::find_if(
        Rules.begin(), Rules.end(),
        [name](const TypeRule &rule) { return rule.wire_name == name; });
    if (found == Rules.end()) {
        fail(ErrorCode::UnknownTypeName, path,
             "unknown field type name '" + std::string{name} + "'");
    }
    return found->type;
}

std::string_view fieldTypeName(FieldType type, std::string_view path) {
    try {
        return typeRule(type).wire_name;
    } catch (const Error &error) {
        if (error.code() != ErrorCode::InvalidTypeOrdinal || path == "type") {
            throw;
        }
        fail(ErrorCode::InvalidTypeOrdinal, path,
             "field type ordinal " +
                 std::to_string(static_cast<unsigned>(type)) + " is invalid");
    }
}

void validateDeclaration(const FieldDeclaration &field,
                         std::string_view path) {
    const TypeRule &rule = typeRule(field.type);
    if (field.name.empty()) {
        fail(ErrorCode::EmptyFieldName, path, "field name must not be empty");
    }

    const bool has_range = !std::holds_alternative<std::monostate>(field.range);
    if (has_range && rule.range_kind == RangeKind::None) {
        fail(ErrorCode::RangeNotApplicable, path,
             "range does not apply to type '" + std::string{rule.wire_name} + "'");
    }
    if (const auto *range = std::get_if<SignedRange>(&field.range)) {
        if (rule.range_kind != RangeKind::Signed) {
            fail(ErrorCode::RangeKindMismatch, path,
                 "signed range does not match field type");
        }
        if (range->min > range->max) {
            fail(ErrorCode::RangeMinimumExceedsMaximum, path,
                 "range minimum exceeds maximum");
        }
        if (range->min < rule.signed_min || range->max > rule.signed_max) {
            fail(ErrorCode::RangeBoundNotRepresentable, path,
                 "range bound is not representable by the field type");
        }
    } else if (const auto *range = std::get_if<UnsignedRange>(&field.range)) {
        if (rule.range_kind != RangeKind::Unsigned) {
            fail(ErrorCode::RangeKindMismatch, path,
                 "unsigned range does not match field type");
        }
        if (range->min > range->max) {
            fail(ErrorCode::RangeMinimumExceedsMaximum, path,
                 "range minimum exceeds maximum");
        }
        if (range->max > rule.unsigned_max) {
            fail(ErrorCode::RangeBoundNotRepresentable, path,
                 "range bound is not representable by the field type");
        }
    } else if (const auto *range = std::get_if<FloatingRange>(&field.range)) {
        if (rule.range_kind != RangeKind::Floating) {
            fail(ErrorCode::RangeKindMismatch, path,
                 "floating range does not match field type");
        }
        if (!std::isfinite(range->min) || !std::isfinite(range->max)) {
            fail(ErrorCode::RangeBoundNotFinite, path,
                 "range bounds must be finite");
        }
        if (range->min > range->max) {
            fail(ErrorCode::RangeMinimumExceedsMaximum, path,
                 "range minimum exceeds maximum");
        }
        if (rule.components_require_f32 &&
            (!f32Representable(range->min) || !f32Representable(range->max))) {
            fail(ErrorCode::RangeBoundNotRepresentable, path,
                 "range bound is not representable as F32");
        }
    }

    if (field.type == FieldType::Enum) {
        if (field.enum_values.empty()) {
            fail(ErrorCode::EnumValuesRequired, path,
                 "enum requires at least one value");
        }
        if (field.enum_values.size() > maxEnumValues) {
            fail(ErrorCode::EnumTooManyValues, path,
                 "enum exceeds the maximum of " + std::to_string(maxEnumValues));
        }
        for (std::size_t index = 0; index < field.enum_values.size(); ++index) {
            if (field.enum_values[index].empty()) {
                fail(ErrorCode::EnumValueEmpty, path,
                     "enum value names must not be empty");
            }
            if (std::find(field.enum_values.begin(),
                          field.enum_values.begin() +
                              static_cast<std::ptrdiff_t>(index),
                          field.enum_values[index]) !=
                field.enum_values.begin() +
                    static_cast<std::ptrdiff_t>(index)) {
                fail(ErrorCode::EnumValueDuplicate, path,
                     "enum value names must be unique");
            }
        }
    } else if (!field.enum_values.empty()) {
        fail(ErrorCode::EnumValuesNotApplicable, path,
             "enum values apply only to enum fields");
    }

    if (field.default_value) {
        (void)resolveValue(field, *field.default_value,
                           std::string{path} + ".default");
    }
    // unit is intentionally opaque: preserving the exact string is its only
    // contract. No semantic unit vocabulary exists in this leaf.
}

void validateDeclarations(std::span<const FieldDeclaration> fields,
                          std::string_view path) {
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const auto field_path = std::string{path} + "[" +
                                std::to_string(index) + "]";
        validateDeclaration(fields[index], field_path);
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (fields[previous].name == fields[index].name) {
                fail(ErrorCode::DuplicateFieldName, field_path + ".name",
                     "field name '" + fields[index].name +
                         "' is declared more than once");
            }
        }
    }
}

nlohmann::ordered_json resolveValue(const FieldDeclaration &field,
                                    const nlohmann::json &value,
                                    std::string_view path) {
    const TypeRule &rule = typeRule(field.type);
    switch (rule.json_shape) {
    case JsonShape::SignedInteger:
        return resolveSigned(rule, field, value, path);
    case JsonShape::UnsignedInteger:
        return resolveUnsigned(rule, field, value, path);
    case JsonShape::FloatingPoint:
        return resolveFloating(rule, field, value, path);
    case JsonShape::Vector: {
        if (!value.is_array() || value.size() != rule.vector_arity) {
            fail(ErrorCode::TypeMismatch, path, "has an invalid array shape");
        }
        auto result = nlohmann::ordered_json::array();
        for (std::size_t index = 0; index < value.size(); ++index) {
            result.push_back(resolveFloating(rule, field, value[index], path));
        }
        return result;
    }
    case JsonShape::String:
        if (!value.is_string()) {
            fail(ErrorCode::TypeMismatch, path, "must be a string");
        }
        return value.get<std::string>();
    case JsonShape::Boolean:
        if (!value.is_boolean()) {
            fail(ErrorCode::TypeMismatch, path, "must be a boolean");
        }
        return value.get<bool>();
    case JsonShape::Enumeration:
        if (!value.is_string()) {
            fail(ErrorCode::TypeMismatch, path, "must be an enum string");
        }
        if (std::find(field.enum_values.begin(), field.enum_values.end(),
                      value.get_ref<const std::string &>()) ==
            field.enum_values.end()) {
            fail(ErrorCode::InvalidEnum, path,
                 "is not a declared enum value");
        }
        return value.get<std::string>();
    }
    fail(ErrorCode::InvalidTypeOrdinal, path, "field type is invalid");
}

nlohmann::ordered_json resolveObject(
    std::span<const FieldDeclaration> fields, const nlohmann::json &value,
    std::string_view path) {
    validateDeclarations(fields);
    if (!value.is_object()) {
        fail(ErrorCode::PayloadMustBeObject, path, "must be an object");
    }
    for (auto item = value.begin(); item != value.end(); ++item) {
        const auto known = std::find_if(
            fields.begin(), fields.end(), [&](const FieldDeclaration &field) {
                return field.name == item.key();
            });
        if (known == fields.end()) {
            fail(ErrorCode::UnknownField,
                 std::string{path} + "." + item.key(), "is unknown");
        }
    }

    auto result = nlohmann::ordered_json::object();
    for (const auto &field : fields) {
        const auto field_path = std::string{path} + "." + field.name;
        const auto found = value.find(field.name);
        if (found != value.end()) {
            result[field.name] = resolveValue(field, *found, field_path);
        } else if (field.default_value) {
            result[field.name] =
                resolveValue(field, *field.default_value, field_path);
        } else {
            fail(ErrorCode::MissingField, field_path, "is required");
        }
    }
    return result;
}

} // namespace Pelican::Schema
