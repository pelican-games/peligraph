#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Pelican::Schema {

enum class FieldType : std::uint8_t {
    I8,
    I16,
    I32,
    I64,
    U8,
    U16,
    U32,
    U64,
    F32,
    F64,
    Vec2,
    Vec3,
    Vec4,
    Quat,
    String,
    Bool,
    Enum,
};

inline constexpr std::array allFieldTypes{
    FieldType::I8,   FieldType::I16,  FieldType::I32,  FieldType::I64,
    FieldType::U8,   FieldType::U16,  FieldType::U32,  FieldType::U64,
    FieldType::F32,  FieldType::F64,  FieldType::Vec2, FieldType::Vec3,
    FieldType::Vec4, FieldType::Quat, FieldType::String,
    FieldType::Bool, FieldType::Enum,
};

inline constexpr std::size_t maxEnumValues = 8;
inline constexpr std::uint64_t maxExactJsonInteger = UINT64_C(9007199254740992);

enum class JsonShape : std::uint8_t {
    SignedInteger,
    UnsignedInteger,
    FloatingPoint,
    Vector,
    String,
    Boolean,
    Enumeration,
};

enum class RangeKind : std::uint8_t { None, Signed, Unsigned, Floating };

struct TypeRule {
    FieldType type;
    std::string_view wire_name;
    JsonShape json_shape;
    RangeKind range_kind;
    std::size_t vector_arity;
    bool components_require_f32;
    std::int64_t signed_min;
    std::int64_t signed_max;
    std::uint64_t unsigned_max;
};

struct SignedRange {
    std::int64_t min;
    std::int64_t max;
};

struct UnsignedRange {
    std::uint64_t min;
    std::uint64_t max;
};

struct FloatingRange {
    double min;
    double max;
};

using FieldRange =
    std::variant<std::monostate, SignedRange, UnsignedRange, FloatingRange>;

struct FieldDeclaration {
    std::string name;
    FieldType type = FieldType::String;
    FieldRange range;
    std::string unit;
    std::vector<std::string> enum_values;
    std::optional<nlohmann::json> default_value;
};

enum class ErrorCode : std::uint8_t {
    UnknownTypeName,
    InvalidTypeOrdinal,
    EmptyFieldName,
    RangeNotApplicable,
    RangeKindMismatch,
    RangeMinimumExceedsMaximum,
    RangeBoundNotRepresentable,
    RangeBoundNotFinite,
    EnumValuesRequired,
    EnumValuesNotApplicable,
    EnumTooManyValues,
    EnumValueEmpty,
    EnumValueDuplicate,
    DuplicateFieldName,
    PayloadMustBeObject,
    UnknownField,
    MissingField,
    TypeMismatch,
    OutOfRange,
    InvalidEnum,
};

std::string_view errorCodeName(ErrorCode code) noexcept;

class Error : public std::runtime_error {
    ErrorCode code_;
    std::string path_;

  public:
    Error(ErrorCode code, std::string path, std::string detail);

    ErrorCode code() const noexcept { return code_; }
    std::string_view path() const noexcept { return path_; }
};

const std::array<TypeRule, allFieldTypes.size()> &typeRules() noexcept;
const TypeRule &typeRule(FieldType type);
FieldType fieldTypeFromName(std::string_view name,
                            std::string_view path = "type");
std::string_view fieldTypeName(FieldType type,
                               std::string_view path = "type");

// Declaration validation and value resolution are deliberately distinct APIs.
// Callers that ingest schemas validate the declaration once, then resolve any
// number of values against it.
void validateDeclaration(const FieldDeclaration &field,
                         std::string_view path = "schema");
void validateDeclarations(std::span<const FieldDeclaration> fields,
                          std::string_view path = "schema");
nlohmann::ordered_json resolveValue(const FieldDeclaration &field,
                                    const nlohmann::json &value,
                                    std::string_view path);
nlohmann::ordered_json resolveObject(
    std::span<const FieldDeclaration> fields, const nlohmann::json &value,
    std::string_view path = "params");

} // namespace Pelican::Schema
