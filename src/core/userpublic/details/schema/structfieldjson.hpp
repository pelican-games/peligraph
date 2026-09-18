#pragma once

#include "structfieldschema.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Pelican {

enum class StructFieldErrorCode : std::uint8_t {
    PayloadMustBeObject,
    UnknownField,
    MissingField,
    TypeMismatch,
    OutOfRange,
    InvalidEnum,
    ReservedFieldName,
    ListContainerMissing,
    RowEnvelopeMissing,
    RowEnvelopeUnsupported,
    DuplicateRowId,
    RowIdExhausted,
};

class StructFieldValidationError : public std::runtime_error {
    StructFieldErrorCode error_code;
    std::string field_path;

  public:
    StructFieldValidationError(StructFieldErrorCode code, std::string path, std::string message)
        : std::runtime_error(std::move(message)), error_code(code), field_path(std::move(path)) {}

    StructFieldErrorCode code() const noexcept {
        return error_code;
    }

    std::string_view path() const noexcept {
        return field_path;
    }
};

namespace internal {

using StructJson = nlohmann::json;

[[noreturn]] inline void structFieldError(StructFieldErrorCode code, std::string path,
                                          std::string_view detail) {
    auto message = "field '" + path + "' " + std::string{detail};
    throw StructFieldValidationError(code, std::move(path), std::move(message));
}

inline std::string childFieldPath(std::string_view root, std::string_view field) {
    if (root.empty()) {
        return std::string{field};
    }
    return std::string{root} + "." + std::string{field};
}

inline bool isSignedStructField(StructFieldType type) {
    return type >= StructFieldType::I8 && type <= StructFieldType::I64;
}

inline bool isUnsignedStructField(StructFieldType type) {
    return type >= StructFieldType::U8 && type <= StructFieldType::U64;
}

inline std::pair<std::int64_t, std::int64_t> signedStructFieldLimits(StructFieldType type) {
    switch (type) {
    case StructFieldType::I8:
        return {std::numeric_limits<std::int8_t>::min(), std::numeric_limits<std::int8_t>::max()};
    case StructFieldType::I16:
        return {std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()};
    case StructFieldType::I32:
        return {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()};
    case StructFieldType::I64:
        return {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()};
    default:
        return {0, 0};
    }
}

inline std::uint64_t unsignedStructFieldLimit(StructFieldType type) {
    switch (type) {
    case StructFieldType::U8:
        return std::numeric_limits<std::uint8_t>::max();
    case StructFieldType::U16:
        return std::numeric_limits<std::uint16_t>::max();
    case StructFieldType::U32:
        return std::numeric_limits<std::uint32_t>::max();
    case StructFieldType::U64:
        return std::numeric_limits<std::uint64_t>::max();
    default:
        return 0;
    }
}

inline constexpr std::uint64_t max_exact_struct_json_integer = UINT64_C(9007199254740992);

inline std::int64_t validateStructSigned(const StructJson &value, const StructFieldSchema &field,
                                         const std::string &path) {
    std::int64_t result = 0;
    if (value.is_number_unsigned()) {
        const auto unsigned_value = value.get<std::uint64_t>();
        if (unsigned_value > max_exact_struct_json_integer ||
            unsigned_value > static_cast<std::uint64_t>(INT64_MAX)) {
            structFieldError(StructFieldErrorCode::TypeMismatch, path,
                             "is outside the exact JSON integer range");
        }
        result = static_cast<std::int64_t>(unsigned_value);
    } else if (value.is_number_integer()) {
        result = value.get<std::int64_t>();
        if (result < -static_cast<std::int64_t>(max_exact_struct_json_integer) ||
            result > static_cast<std::int64_t>(max_exact_struct_json_integer)) {
            structFieldError(StructFieldErrorCode::TypeMismatch, path,
                             "is outside the exact JSON integer range");
        }
    } else {
        structFieldError(StructFieldErrorCode::TypeMismatch, path, "must be an integer token");
    }

    const auto [min_value, max_value] = signedStructFieldLimits(field.type);
    if (result < min_value || result > max_value) {
        structFieldError(StructFieldErrorCode::TypeMismatch, path,
                         "is not representable by its integer type");
    }
    if (const auto *range = std::get_if<std::pair<std::int64_t, std::int64_t>>(&field.range);
        range != nullptr && (result < range->first || result > range->second)) {
        structFieldError(StructFieldErrorCode::OutOfRange, path, "is out of range");
    }
    return result;
}

inline std::uint64_t validateStructUnsigned(const StructJson &value, const StructFieldSchema &field,
                                            const std::string &path) {
    std::uint64_t result = 0;
    if (value.is_number_unsigned()) {
        result = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value < 0) {
            structFieldError(StructFieldErrorCode::TypeMismatch, path, "must not be negative");
        }
        result = static_cast<std::uint64_t>(signed_value);
    } else {
        structFieldError(StructFieldErrorCode::TypeMismatch, path, "must be an integer token");
    }

    if (result > max_exact_struct_json_integer) {
        structFieldError(StructFieldErrorCode::TypeMismatch, path,
                         "is outside the exact JSON integer range");
    }
    if (result > unsignedStructFieldLimit(field.type)) {
        structFieldError(StructFieldErrorCode::TypeMismatch, path,
                         "is not representable by its integer type");
    }
    if (const auto *range = std::get_if<std::pair<std::uint64_t, std::uint64_t>>(&field.range);
        range != nullptr && (result < range->first || result > range->second)) {
        structFieldError(StructFieldErrorCode::OutOfRange, path, "is out of range");
    }
    return result;
}

inline double validateStructFloating(const StructJson &value, const StructFieldSchema &field,
                                     const std::string &path, bool requires_f32) {
    if (!value.is_number()) {
        structFieldError(StructFieldErrorCode::TypeMismatch, path, "must be a number");
    }
    const double result = value.get<double>();
    if (!std::isfinite(result)) {
        structFieldError(StructFieldErrorCode::TypeMismatch, path, "must be finite");
    }
    if (requires_f32) {
        constexpr double max_f32 = static_cast<double>(std::numeric_limits<float>::max());
        if (result < -max_f32 || result > max_f32) {
            structFieldError(StructFieldErrorCode::TypeMismatch, path,
                             "is not representable as F32");
        }
    }
    if (const auto *range = std::get_if<std::pair<double, double>>(&field.range);
        range != nullptr && (result < range->first || result > range->second)) {
        structFieldError(StructFieldErrorCode::OutOfRange, path, "is out of range");
    }
    return result;
}

inline std::size_t structVectorLength(StructFieldType type) {
    switch (type) {
    case StructFieldType::Vec2:
        return 2;
    case StructFieldType::Vec3:
        return 3;
    case StructFieldType::Vec4:
    case StructFieldType::Quat:
        return 4;
    default:
        return 0;
    }
}

template <class PolicyField, class Owner>
void setStructDefault(const PolicyField &policy_field, Owner &temporary) {
    using Traits = PolicyFieldTraits<PolicyField>;
    static_assert(std::same_as<Owner, typename Traits::owner_type>, "schema owner does not match destination");
    if constexpr (Traits::presence == StructFieldPresence::Defaulted) {
        using Declaration = typename Traits::declaration_type;
        using Member = typename Traits::member_type;
        if constexpr (is_list_v<Member>) {
            temporary.*Declaration::member = Member{};
        } else if constexpr (std::same_as<Member, std::string>) {
            temporary.*Declaration::member = std::string{policy_field.default_value};
        } else {
            temporary.*Declaration::member = policy_field.default_value;
        }
    }
}

template <class Owner, class Policy, class... PolicyFields>
void decodeStructObject(
    const StructJson &payload, Owner &destination,
    const StructSchemaDescriptor<Policy, PolicyFields...> &schema,
    std::string_view root_path);

template <class Owner, class Policy, class... PolicyFields>
nlohmann::ordered_json encodeStructObject(
    const Owner &value,
    const StructSchemaDescriptor<Policy, PolicyFields...> &schema,
    std::string_view root_path);

// The semantic List invariants are shared by both directions.  Decode first
// constructs the typed value after checking JSON shape; encode calls the same
// validator before emitting a byte of wire data.
template <class Row>
void validateListValue(const List<Row> &list, const std::string &path) {
    std::set<std::uint32_t> ids;
    std::uint32_t maximum_id = 0;
    for (std::size_t index = 0; index < list.rows.size(); ++index) {
        const auto &envelope = list.rows[index].envelope;
        const auto row_path = path + ".rows[" + std::to_string(index) + "]";
        if (envelope.id == 0)
            structFieldError(StructFieldErrorCode::OutOfRange, row_path,
                             "row id must be non-zero");
        if (envelope.version != 1)
            structFieldError(StructFieldErrorCode::RowEnvelopeUnsupported,
                             row_path,
                             "RowEnvelopeUnsupported: unknown $row version");
        if (!ids.emplace(envelope.id).second)
            structFieldError(StructFieldErrorCode::DuplicateRowId, row_path,
                             "DuplicateRowId: row id is duplicated");
        maximum_id = std::max(maximum_id, envelope.id);
        if (envelope.hint_json) {
            try {
                if (!nlohmann::json::parse(*envelope.hint_json).is_object())
                    structFieldError(StructFieldErrorCode::TypeMismatch,
                                     row_path + ".$row.hint",
                                     "must be an object");
            } catch (const StructFieldValidationError &) {
                throw;
            } catch (const nlohmann::json::exception &) {
                structFieldError(StructFieldErrorCode::TypeMismatch,
                                 row_path + ".$row.hint",
                                 "must be valid object JSON");
            }
        }
    }
    if (maximum_id == std::numeric_limits<std::uint32_t>::max())
        structFieldError(StructFieldErrorCode::RowIdExhausted, path,
                         "RowIdExhausted: row id high-water reached UINT32_MAX");
    if (list.next_id <= maximum_id)
        structFieldError(StructFieldErrorCode::OutOfRange, path,
                         "$list.next_id must exceed every row id");
}

// Presence is the only thing that separates an OptionalOverride field from a
// Required or Defaulted one.  The per-value rules -- shape checks, error
// text, and the order in which the checks run -- live here once, so the two
// presence forms cannot drift apart again.
template <class Value, class PolicyField>
Value decodeStructScalarValue(const StructJson &value, const PolicyField &policy_field,
                              const StructFieldSchema &schema, const std::string &path) {
    Value result{};
    if constexpr (is_flags_v<Value>) {
        if (!value.is_array()) {
            structFieldError(StructFieldErrorCode::TypeMismatch, path,
                             "must be a flag string array");
        }
        using Storage = typename Value::storage_type;
        Storage bits = 0;
        std::set<std::string> seen;
        for (const auto &item : value) {
            if (!item.is_string()) {
                structFieldError(StructFieldErrorCode::TypeMismatch, path,
                                 "flag entries must be strings");
            }
            const auto &name = item.template get_ref<const std::string &>();
            if (!seen.emplace(name).second) {
                structFieldError(StructFieldErrorCode::InvalidEnum, path,
                                 "contains a duplicate flag");
            }
            bool found = false;
            for (const auto &entry : policy_field.enum_values.values) {
                if (entry.name == name) {
                    bits |= static_cast<Storage>(entry.value);
                    found = true;
                    break;
                }
            }
            if (!found) {
                structFieldError(StructFieldErrorCode::InvalidEnum, path,
                                 "contains an undeclared flag");
            }
        }
        result = Value{bits};
    } else if constexpr (std::same_as<Value, bool>) {
        if (!value.is_boolean()) {
            structFieldError(StructFieldErrorCode::TypeMismatch, path, "must be a boolean");
        }
        result = value.get<bool>();
    } else if constexpr (std::is_enum_v<Value>) {
        if (!value.is_string()) {
            structFieldError(StructFieldErrorCode::TypeMismatch, path, "must be an enum string");
        }
        const auto &name = value.template get_ref<const std::string &>();
        bool found = false;
        for (const auto &entry : policy_field.enum_values.values) {
            if (entry.name == name) {
                result = entry.value;
                found = true;
                break;
            }
        }
        if (!found) {
            structFieldError(StructFieldErrorCode::InvalidEnum, path,
                             "is not a declared enum value");
        }
    } else if constexpr (std::is_signed_v<Value> && std::is_integral_v<Value>) {
        result = static_cast<Value>(validateStructSigned(value, schema, path));
    } else if constexpr (std::is_unsigned_v<Value> && std::is_integral_v<Value>) {
        result = static_cast<Value>(validateStructUnsigned(value, schema, path));
    } else if constexpr (std::same_as<Value, float> || std::same_as<Value, double>) {
        result = static_cast<Value>(validateStructFloating(value, schema, path,
                                                           std::same_as<Value, float>));
    } else if constexpr (std::same_as<Value, std::string>) {
        if (!value.is_string()) {
            structFieldError(StructFieldErrorCode::TypeMismatch, path, "must be a string");
        }
        result = value.template get_ref<const std::string &>();
    } else if constexpr (std::same_as<Value, vec2> || std::same_as<Value, vec3> ||
                         std::same_as<Value, vec4> || std::same_as<Value, quat>) {
        if (!value.is_array() || value.size() != structVectorLength(schema.type)) {
            structFieldError(StructFieldErrorCode::TypeMismatch, path, "has an invalid array shape");
        }
        std::array<float, 4> components{};
        for (std::size_t i = 0; i < value.size(); ++i) {
            components[i] = static_cast<float>(validateStructFloating(value[i], schema, path, true));
        }
        if constexpr (std::same_as<Value, vec2>) {
            result = {components[0], components[1]};
        } else if constexpr (std::same_as<Value, vec3>) {
            result = {components[0], components[1], components[2]};
        } else {
            result = {components[0], components[1], components[2], components[3]};
        }
    } else {
        static_assert(always_false_v<Value>, "unsupported struct JSON field type");
    }
    return result;
}

// Encode runs the same range and finiteness checks the decoder would run on
// the wire form it is about to emit, so a typed value that could not survive
// a round trip is reported before any byte leaves.
template <class Value, class PolicyField>
nlohmann::ordered_json encodeStructScalarValue(const Value &value, const PolicyField &policy_field,
                                               const StructFieldSchema &schema,
                                               const std::string &path) {
    if constexpr (is_flags_v<Value>) {
        nlohmann::ordered_json result = nlohmann::ordered_json::array();
        typename Value::storage_type declared_bits = 0;
        for (const auto &entry : policy_field.enum_values.values) {
            declared_bits |= static_cast<typename Value::storage_type>(entry.value);
            if (value.contains(entry.value)) result.push_back(entry.name);
        }
        if ((value.bits() & ~declared_bits) != 0) {
            structFieldError(StructFieldErrorCode::InvalidEnum, path,
                             "contains bits outside the declared vocabulary");
        }
        return result;
    } else if constexpr (std::same_as<Value, bool>) {
        return value;
    } else if constexpr (std::is_enum_v<Value>) {
        for (const auto &entry : policy_field.enum_values.values) {
            if (entry.value == value) {
                return entry.name;
            }
        }
        structFieldError(StructFieldErrorCode::InvalidEnum, path, "is not a declared enum value");
    } else if constexpr (std::is_signed_v<Value> && std::is_integral_v<Value>) {
        const StructJson encoded = value;
        (void)validateStructSigned(encoded, schema, path);
        return value;
    } else if constexpr (std::is_unsigned_v<Value> && std::is_integral_v<Value>) {
        const StructJson encoded = value;
        (void)validateStructUnsigned(encoded, schema, path);
        return value;
    } else if constexpr (std::same_as<Value, float> || std::same_as<Value, double>) {
        const StructJson encoded = value;
        (void)validateStructFloating(encoded, schema, path, std::same_as<Value, float>);
        return value;
    } else if constexpr (std::same_as<Value, vec2>) {
        for (const float component : {value.x, value.y}) {
            (void)validateStructFloating(StructJson(component), schema, path, true);
        }
        return nlohmann::ordered_json::array({value.x, value.y});
    } else if constexpr (std::same_as<Value, vec3>) {
        for (const float component : {value.x, value.y, value.z}) {
            (void)validateStructFloating(StructJson(component), schema, path, true);
        }
        return nlohmann::ordered_json::array({value.x, value.y, value.z});
    } else if constexpr (std::same_as<Value, vec4> || std::same_as<Value, quat>) {
        for (const float component : {value.x, value.y, value.z, value.w}) {
            (void)validateStructFloating(StructJson(component), schema, path, true);
        }
        return nlohmann::ordered_json::array({value.x, value.y, value.z, value.w});
    } else {
        return value;
    }
}

template <class PolicyField, class Owner>
void decodeStructPolicyField(const StructJson &payload, const PolicyField &policy_field, Owner &temporary,
                             std::string_view root_path) {
    using Traits = PolicyFieldTraits<PolicyField>;
    using Declaration = typename Traits::declaration_type;
    using Member = typename Traits::member_type;
    static_assert(std::same_as<Owner, typename Traits::owner_type>, "schema owner does not match destination");

    const auto &descriptor = policy_field.field.descriptor;
    const auto value_it = payload.find(descriptor.name);
    if (value_it == payload.end()) {
        if constexpr (Traits::presence == StructFieldPresence::Required) {
            const auto path = childFieldPath(root_path, descriptor.name);
            structFieldError(StructFieldErrorCode::MissingField, path, "is required");
        }
        return;
    }

    const auto schema = materializeStructField(descriptor);
    const auto path = childFieldPath(root_path, descriptor.name);
    auto &member = temporary.*Declaration::member;
    if constexpr (is_optional_v<Member>) {
        using Value = unwrap_optional_t<Member>;
        member = decodeStructScalarValue<Value>(*value_it, policy_field, schema, path);
    } else if constexpr (is_list_v<Member>) {
        using Row = typename ListTraits<Member>::row_type;
        if (!value_it->is_object()) {
            structFieldError(StructFieldErrorCode::ListContainerMissing, path,
                             "ListContainerMissing: list must use an object container");
        }
        for (auto item = value_it->begin(); item != value_it->end(); ++item) {
            if (item.key() != "$list" && item.key() != "rows") {
                structFieldError(StructFieldErrorCode::UnknownField,
                                 childFieldPath(path, item.key()),
                                 "is not part of a List container");
            }
        }
        const auto list_meta = value_it->find("$list");
        if (list_meta == value_it->end()) {
            structFieldError(StructFieldErrorCode::RowEnvelopeMissing, path,
                             "RowEnvelopeMissing: list container has no $list envelope");
        }
        if (!list_meta->is_object() || !list_meta->contains("v") ||
            !list_meta->contains("next_id")) {
            structFieldError(StructFieldErrorCode::RowEnvelopeMissing, path,
                             "RowEnvelopeMissing: invalid $list envelope");
        }
        for (auto item = list_meta->begin(); item != list_meta->end(); ++item) {
            if (item.key() != "v" && item.key() != "next_id") {
                structFieldError(StructFieldErrorCode::UnknownField,
                                 childFieldPath(path, "$list." + item.key()),
                                 "is not part of the $list envelope");
            }
        }
        const auto read_u32 = [&](const StructJson &json,
                                  std::string child) -> std::uint32_t {
            if ((!json.is_number_unsigned() && !json.is_number_integer()) ||
                (json.is_number_integer() && json.get<std::int64_t>() < 0)) {
                structFieldError(StructFieldErrorCode::TypeMismatch,
                                 childFieldPath(path, child),
                                 "must be an unsigned 32-bit integer");
            }
            const auto value = json.get<std::uint64_t>();
            if (value > std::numeric_limits<std::uint32_t>::max()) {
                structFieldError(StructFieldErrorCode::OutOfRange,
                                 childFieldPath(path, child),
                                 "is outside U32");
            }
            return static_cast<std::uint32_t>(value);
        };
        const auto list_version = read_u32(list_meta->at("v"), "$list.v");
        if (list_version != 1) {
            structFieldError(StructFieldErrorCode::RowEnvelopeUnsupported, path,
                             "RowEnvelopeUnsupported: unknown $list version");
        }
        Member decoded;
        decoded.next_id = read_u32(list_meta->at("next_id"), "$list.next_id");
        const auto rows = value_it->find("rows");
        if (rows == value_it->end() || !rows->is_array()) {
            structFieldError(StructFieldErrorCode::TypeMismatch, path,
                             "list rows must be an array");
        }
        for (std::size_t index = 0; index < rows->size(); ++index) {
            const auto &encoded_row = rows->at(index);
            const auto row_path = path + ".rows[" + std::to_string(index) + "]";
            if (!encoded_row.is_object() || !encoded_row.contains("$row")) {
                structFieldError(StructFieldErrorCode::RowEnvelopeMissing,
                                 row_path,
                                 "RowEnvelopeMissing: row has no $row envelope");
            }
            const auto &row_meta = encoded_row.at("$row");
            if (!row_meta.is_object() || !row_meta.contains("id") ||
                !row_meta.contains("v")) {
                structFieldError(StructFieldErrorCode::RowEnvelopeMissing,
                                 row_path,
                                 "RowEnvelopeMissing: invalid $row envelope");
            }
            for (auto item = row_meta.begin(); item != row_meta.end(); ++item) {
                if (item.key() != "id" && item.key() != "v" &&
                    item.key() != "hint") {
                    structFieldError(
                        StructFieldErrorCode::UnknownField,
                        row_path + ".$row." + item.key(),
                        "is not part of the $row envelope");
                }
            }
            ListRow<Row> row;
            row.envelope.id = read_u32(row_meta.at("id"), "$row.id");
            row.envelope.version = read_u32(row_meta.at("v"), "$row.v");
            if (const auto hint = row_meta.find("hint"); hint != row_meta.end()) {
                if (!hint->is_object()) {
                    structFieldError(StructFieldErrorCode::TypeMismatch,
                                     row_path + ".$row.hint",
                                     "must be an object");
                }
                row.envelope.hint_json = hint->dump();
            }
            auto authored_row = encoded_row;
            authored_row.erase("$row");
            decodeStructObject(authored_row, row.value, Row::schema, row_path);
            decoded.rows.push_back(std::move(row));
        }
        validateListValue(decoded, path);
        member = std::move(decoded);
    } else {
        member = decodeStructScalarValue<Member>(*value_it, policy_field, schema, path);
    }
}

template <class Owner, class PolicyField>
nlohmann::ordered_json encodeStructPolicyField(const Owner &value, const PolicyField &policy_field,
                                                std::string_view root_path) {
    using Traits = PolicyFieldTraits<PolicyField>;
    using Declaration = typename Traits::declaration_type;
    using Member = typename Traits::member_type;
    static_assert(std::same_as<Owner, typename Traits::owner_type>, "schema owner does not match source");

    const auto &member = value.*Declaration::member;
    const auto path = childFieldPath(root_path, policy_field.field.descriptor.name);
    const auto schema = materializeStructField(policy_field.field.descriptor);
    if constexpr (is_optional_v<Member>) {
        if (!member) return nullptr;
        using Value = unwrap_optional_t<Member>;
        return encodeStructScalarValue<Value>(*member, policy_field, schema, path);
    } else if constexpr (is_list_v<Member>) {
        using Row = typename ListTraits<Member>::row_type;
        validateListValue(member, path);
        nlohmann::ordered_json result = nlohmann::ordered_json::object();
        result["$list"] = nlohmann::ordered_json{{"v", 1}, {"next_id", member.next_id}};
        result["rows"] = nlohmann::ordered_json::array();
        for (const auto &row : member.rows) {
            nlohmann::ordered_json envelope{{"id", row.envelope.id},
                                            {"v", row.envelope.version}};
            if (row.envelope.hint_json) {
                envelope["hint"] = nlohmann::ordered_json::parse(*row.envelope.hint_json);
            }
            nlohmann::ordered_json encoded_row = nlohmann::ordered_json::object();
            encoded_row["$row"] = std::move(envelope);
            auto authored = encodeStructObject(row.value, Row::schema,
                                               path);
            for (auto &[key, item] : authored.items()) {
                encoded_row[key] = std::move(item);
            }
            result["rows"].push_back(std::move(encoded_row));
        }
        return result;
    } else {
        return encodeStructScalarValue<Member>(member, policy_field, schema, path);
    }
}

} // namespace internal

namespace internal {

template <class Owner, class Policy, class... PolicyFields>
void decodeStructObject(
    const StructJson &payload, Owner &destination,
    const StructSchemaDescriptor<Policy, PolicyFields...> &schema,
    std::string_view root_path) {
    static_assert(std::default_initializable<Owner>,
                  "schema owner must be default constructible");
    static_assert(std::is_nothrow_swappable_v<Owner>,
                  "schema owner atomic apply requires a noexcept swap");
    if (!payload.is_object()) {
        structFieldError(StructFieldErrorCode::PayloadMustBeObject,
                         std::string{root_path}, "must be an object");
    }
    for (auto it = payload.begin(); it != payload.end(); ++it) {
        bool known = false;
        for (const auto &field_schema : schema.fields) {
            known = known || field_schema.name == it.key();
        }
        if (!known) {
            const auto path = childFieldPath(root_path, it.key());
            if (it.key().starts_with("$")) {
                structFieldError(StructFieldErrorCode::ReservedFieldName,
                                 path,
                                 "ReservedFieldName: '$' names are reserved");
            }
            structFieldError(StructFieldErrorCode::UnknownField, path,
                             "is unknown");
        }
    }

    Owner temporary{};
    std::apply(
        [&](const auto &...policy_field) {
            (setStructDefault(policy_field, temporary), ...);
            (decodeStructPolicyField(payload, policy_field, temporary,
                                     root_path), ...);
            ((void)encodeStructPolicyField(temporary, policy_field,
                                           root_path), ...);
        },
        schema.declarations);
    using std::swap;
    swap(destination, temporary);
}

template <class Owner, class Policy, class... PolicyFields>
nlohmann::ordered_json encodeStructObject(
    const Owner &value,
    const StructSchemaDescriptor<Policy, PolicyFields...> &schema,
    std::string_view root_path) {
    nlohmann::ordered_json result = nlohmann::ordered_json::object();
    std::apply(
        [&](const auto &...policy_field) {
            ([&] {
                using Field = std::remove_cvref_t<decltype(policy_field)>;
                using Member = typename PolicyFieldTraits<Field>::member_type;
                using Declaration =
                    typename PolicyFieldTraits<Field>::declaration_type;
                const auto &member = value.*Declaration::member;
                if constexpr (is_optional_v<Member>) {
                    if (!member) return;
                }
                result[std::string{policy_field.field.descriptor.name}] =
                    encodeStructPolicyField(value, policy_field, root_path);
            }(), ...);
        },
        schema.declarations);
    return result;
}

} // namespace internal

namespace internal {

inline nlohmann::ordered_json schemaRangeJson(
    const StructFieldRangeDescriptor &range) {
    switch (range.kind) {
    case StructFieldRangeKind::None: return nullptr;
    case StructFieldRangeKind::Signed:
        return nlohmann::ordered_json::array(
            {range.signed_min, range.signed_max});
    case StructFieldRangeKind::Unsigned:
        return nlohmann::ordered_json::array(
            {range.unsigned_min, range.unsigned_max});
    case StructFieldRangeKind::Floating: {
        const auto canonical = [](double endpoint) {
            return endpoint == 0.0 ? 0.0 : endpoint;
        };
        return nlohmann::ordered_json::array(
            {canonical(range.floating_min), canonical(range.floating_max)});
    }
    }
    return nullptr;
}

template <class T>
constexpr SchemaDefaultValue schemaDefaultValue(const T &value) {
    SchemaDefaultValue result;
    if constexpr (std::same_as<T, EmptyListDefault>) {
        result.kind = SchemaDefaultKind::EmptyList;
    } else if constexpr (std::same_as<T, bool>) {
        result.kind = SchemaDefaultKind::Bool;
        result.unsigned_value = value ? 1 : 0;
    } else if constexpr (std::same_as<T, std::string_view>) {
        result.kind = SchemaDefaultKind::String;
        result.string_value = value;
    } else if constexpr (is_flags_v<T>) {
        result.kind = SchemaDefaultKind::Flags;
        result.unsigned_value = value.bits();
    } else if constexpr (std::is_enum_v<T>) {
        result.kind = SchemaDefaultKind::Enum;
        using Raw = std::underlying_type_t<T>;
        if constexpr (std::is_signed_v<Raw>)
            result.signed_value = static_cast<std::int64_t>(value);
        else
            result.unsigned_value = static_cast<std::uint64_t>(value);
    } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
        result.kind = SchemaDefaultKind::Signed;
        result.signed_value = static_cast<std::int64_t>(value);
    } else if constexpr (std::is_integral_v<T>) {
        result.kind = SchemaDefaultKind::Unsigned;
        result.unsigned_value = static_cast<std::uint64_t>(value);
    } else if constexpr (std::is_floating_point_v<T>) {
        result.kind = SchemaDefaultKind::Floating;
        result.components[0] = value == 0 ? 0.0 : static_cast<double>(value);
        result.component_count = 1;
    } else if constexpr (std::same_as<T, vec2>) {
        result.kind = SchemaDefaultKind::Vector;
        result.components = {value.x, value.y, 0, 0};
        result.component_count = 2;
    } else if constexpr (std::same_as<T, vec3>) {
        result.kind = SchemaDefaultKind::Vector;
        result.components = {value.x, value.y, value.z, 0};
        result.component_count = 3;
    } else if constexpr (std::same_as<T, vec4> || std::same_as<T, quat>) {
        result.kind = SchemaDefaultKind::Vector;
        result.components = {value.x, value.y, value.z, value.w};
        result.component_count = 4;
    } else {
        static_assert(always_false_v<T>, "unsupported SchemaNode default type");
    }
    return result;
}

template <class PolicyField>
constexpr void populateSchemaNodeEntry(SchemaNodeEntry &node,
                                       const PolicyField &policy_field) {
    using Field = std::remove_cvref_t<PolicyField>;
    using Traits = PolicyFieldTraits<Field>;
    using Member = unwrap_optional_t<typename Traits::member_type>;
    node.name = policy_field.field.descriptor.name;
    node.presence = Traits::presence;
    node.leaf_type = policy_field.field.descriptor.type;
    node.range = policy_field.field.descriptor.range;
    node.unit = policy_field.field.descriptor.unit;
    if constexpr (is_list_v<Member>) node.tag = SchemaNodeTag::List;
    else if constexpr (is_flags_v<Member>) node.tag = SchemaNodeTag::Flags;
    else node.tag = SchemaNodeTag::Leaf;

    if constexpr (Traits::has_enum_values) {
        for (const auto &entry : policy_field.enum_values.values) {
            using Enum = std::remove_cvref_t<decltype(entry.value)>;
            using Raw = std::underlying_type_t<Enum>;
            SchemaVocabularyValue value{entry.name, 0, std::is_signed_v<Raw>};
            if constexpr (std::is_signed_v<Raw>)
                value.value = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(entry.value));
            else
                value.value = static_cast<std::uint64_t>(entry.value);
            node.vocabulary[node.vocabulary_count++] = value;
        }
    }
    if constexpr (Traits::presence == StructFieldPresence::Defaulted) {
        node.default_value = schemaDefaultValue(policy_field.default_value);
        if constexpr (std::is_enum_v<Member>) {
            for (const auto &entry : policy_field.enum_values.values)
                if (entry.value == policy_field.default_value)
                    node.default_value.string_value = entry.name;
        }
    }
}

template <class Policy, class... PolicyFields>
constexpr void appendSchemaNodeFields(
    SchemaNode &result,
    const StructSchemaDescriptor<Policy, PolicyFields...> &schema,
    std::uint16_t parent, std::uint8_t recursive_depth = 0) {
    std::apply(
        [&](const auto &...policy_field) {
            ([&] {
                if (result.node_count >= maxSchemaNodeCount)
                    throw "SchemaNodeCapacityExceeded";
                const auto index = result.node_count++;
                auto &node = result.nodes[index];
                node.parent = parent;
                populateSchemaNodeEntry(node, policy_field);
                using Field = std::remove_cvref_t<decltype(policy_field)>;
                using Member = unwrap_optional_t<
                    typename PolicyFieldTraits<Field>::member_type>;
                if constexpr (is_list_v<Member>) {
                    if (recursive_depth >= 1)
                        throw "SchemaNodeDepthExceeded";
                    if (result.node_count >= maxSchemaNodeCount)
                        throw "SchemaNodeCapacityExceeded";
                    const auto row_index = result.node_count++;
                    result.nodes[row_index].tag = SchemaNodeTag::Struct;
                    result.nodes[row_index].parent = index;
                    using Row = typename ListTraits<Member>::row_type;
                    appendSchemaNodeFields(result, Row::schema, row_index,
                                           recursive_depth + 1);
                }
            }(), ...);
        },
        schema.declarations);
}

template <class Policy, class... PolicyFields>
consteval SchemaNode materializeSchemaNode(
    const StructSchemaDescriptor<Policy, PolicyFields...> &schema) {
    SchemaNode result;
    appendSchemaNodeFields(result, schema, 0);
    return result;
}

inline nlohmann::ordered_json schemaDefaultJson(const SchemaNodeEntry &node) {
    const auto &value = node.default_value;
    switch (value.kind) {
    case SchemaDefaultKind::None: return nullptr;
    case SchemaDefaultKind::EmptyList:
        return nlohmann::ordered_json{{"$list", {{"v", 1}, {"next_id", 1}}},
                                      {"rows", nlohmann::ordered_json::array()}};
    case SchemaDefaultKind::Bool: return value.unsigned_value != 0;
    case SchemaDefaultKind::Signed: return value.signed_value;
    case SchemaDefaultKind::Unsigned: return value.unsigned_value;
    case SchemaDefaultKind::Floating: return value.components[0];
    case SchemaDefaultKind::String:
    case SchemaDefaultKind::Enum: return value.string_value;
    case SchemaDefaultKind::Flags: {
        auto result = nlohmann::ordered_json::array();
        for (std::size_t index = 0; index < node.vocabulary_count; ++index) {
            const auto &entry = node.vocabulary[index];
            if ((value.unsigned_value & entry.value) == entry.value)
                result.push_back(entry.name);
        }
        return result;
    }
    case SchemaDefaultKind::Vector: {
        auto result = nlohmann::ordered_json::array();
        for (std::size_t index = 0; index < value.component_count; ++index)
            result.push_back(value.components[index]);
        return result;
    }
    }
    return nullptr;
}

inline nlohmann::ordered_json schemaNodeJson(const SchemaNode &tree,
                                             std::size_t node_index = 0) {
    const auto &node = tree.at(node_index);
    nlohmann::ordered_json result{
        {"tag", static_cast<std::uint8_t>(node.tag)},
        {"name", node.name},
        {"presence", static_cast<std::uint8_t>(node.presence)},
        {"type", static_cast<std::uint8_t>(node.leaf_type)},
        {"range", schemaRangeJson(node.range)},
        {"unit", node.unit},
        {"default", schemaDefaultJson(node)},
    };
    auto vocabulary = nlohmann::ordered_json::array();
    for (std::size_t index = 0; index < node.vocabulary_count; ++index) {
        const auto &entry = node.vocabulary[index];
        vocabulary.push_back({{"name", entry.name},
                              {"value", entry.value},
                              {"signed", entry.signed_value}});
    }
    result["vocabulary"] = std::move(vocabulary);
    auto children = nlohmann::ordered_json::array();
    for (std::size_t ordinal = 0; ordinal < tree.childCount(node_index);
         ++ordinal)
        children.push_back(schemaNodeJson(
            tree, tree.childIndex(node_index, ordinal)));
    result["children"] = std::move(children);
    return result;
}

inline std::string schemaNodeFingerprint(const SchemaNode &node) {
    return schemaNodeJson(node).dump();
}

} // namespace internal

template <class Params, class... PolicyFields>
void decodeBehaviorParams(const nlohmann::json &payload, Params &destination,
                          const StructSchemaDescriptor<BehaviorParamsPolicy, PolicyFields...> &schema,
                          std::string_view root_path = "params") {
    static_assert(std::default_initializable<Params>, "BehaviorParams must be default constructible");
    static_assert(std::is_nothrow_swappable_v<Params>,
                  "BehaviorParams atomic apply requires a noexcept swap");

    internal::decodeStructObject(payload, destination, schema, root_path);
}

template <class Params, class... PolicyFields>
nlohmann::ordered_json
encodeBehaviorParams(const Params &value,
                     const StructSchemaDescriptor<BehaviorParamsPolicy, PolicyFields...> &schema,
                     std::string_view root_path = "params") {
    return internal::encodeStructObject(value, schema, root_path);
}

} // namespace Pelican
