#pragma once

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Pelican {

enum class LogicalTypeConstructor : std::uint8_t {
    image,
    buffer,
    stream,
    object_set,
    value,
};

std::string_view logicalTypeConstructorName(LogicalTypeConstructor constructor);

struct SemanticTypeId {
    std::string name_space;
    std::string name;
    std::uint32_t major_version = 1;

    bool operator==(const SemanticTypeId &) const = default;
    bool operator<(const SemanticTypeId &other) const;
};

SemanticTypeId parseSemanticTypeId(std::string_view name);
std::string semanticTypeIdName(const SemanticTypeId &id);

struct EnumValueId {
    std::string value;

    bool operator==(const EnumValueId &) const = default;
    bool operator<(const EnumValueId &other) const { return value < other.value; }
};

struct SymbolId {
    std::string value;

    bool operator==(const SymbolId &) const = default;
    bool operator<(const SymbolId &other) const { return value < other.value; }
};

struct Rational {
    std::int64_t numerator = 0;
    std::uint64_t denominator = 1;

    bool operator==(const Rational &) const = default;
};

struct IntegerInterval {
    std::int64_t minimum = 0;
    std::int64_t maximum = 0;

    bool operator==(const IntegerInterval &) const = default;
};

struct EnumValueSet {
    std::vector<EnumValueId> values;

    bool operator==(const EnumValueSet &) const = default;
};

using TypeArgumentValue =
    std::variant<bool, std::int64_t, std::uint64_t, Rational, EnumValueId,
                 SemanticTypeId, IntegerInterval, EnumValueSet, SymbolId>;

enum class TypeArgumentValueKind : std::uint8_t {
    boolean,
    signed_integer,
    unsigned_integer,
    rational,
    enum_value,
    semantic_type,
    integer_interval,
    enum_set,
    symbol,
};

std::string_view typeArgumentValueKindName(TypeArgumentValueKind kind);
TypeArgumentValueKind typeArgumentValueKind(const TypeArgumentValue &value);

enum class TypeArgumentRole : std::uint8_t {
    identity,
    refinement,
};

std::string_view typeArgumentRoleName(TypeArgumentRole role);

struct TypeParameterSchema {
    std::string name;
    TypeArgumentRole role = TypeArgumentRole::identity;
    TypeArgumentValueKind value_kind = TypeArgumentValueKind::enum_value;
    std::optional<TypeArgumentValue> default_value;
    std::vector<EnumValueId> allowed_enum_values;
    std::optional<std::int64_t> signed_minimum;
    std::optional<std::int64_t> signed_maximum;
    std::optional<std::uint64_t> unsigned_minimum;
    std::optional<std::uint64_t> unsigned_maximum;
};

struct SemanticTypeSchema {
    SemanticTypeId id;
    LogicalTypeConstructor constructor = LogicalTypeConstructor::value;
    std::vector<TypeParameterSchema> parameters;
    std::vector<std::string> traits;
};

struct LogicalTypeArgumentInput {
    std::string name;
    TypeArgumentValue value;
};

struct LogicalTypeArgument {
    std::string name;
    TypeArgumentRole role = TypeArgumentRole::identity;
    TypeArgumentValue value;

    bool operator==(const LogicalTypeArgument &) const = default;
};

struct LogicalType {
    LogicalTypeConstructor constructor = LogicalTypeConstructor::value;
    SemanticTypeId semantic;
    std::vector<LogicalTypeArgument> arguments;

    bool operator==(const LogicalType &) const = default;
};

class LogicalTypeRegistry {
    std::map<SemanticTypeId, SemanticTypeSchema> schemas_;

  public:
    void registerSchema(SemanticTypeSchema schema);
    bool contains(const SemanticTypeId &id) const;
    const SemanticTypeSchema &schema(const SemanticTypeId &id) const;
    TypeArgumentValue canonicalizeArgumentValue(
        const SemanticTypeId &semantic, std::string_view parameter_name,
        TypeArgumentValue value) const;
    LogicalType canonicalize(
        const SemanticTypeId &semantic,
        std::vector<LogicalTypeArgumentInput> arguments = {}) const;
    void requireCanonical(const LogicalType &type) const;
};

LogicalTypeRegistry makeBuiltinLogicalTypeRegistry();
LogicalType sceneLinearHdrV1(const LogicalTypeRegistry &registry);
LogicalType displayLinearV1(const LogicalTypeRegistry &registry);
LogicalType displayEncodedV1(const LogicalTypeRegistry &registry);
LogicalType deviceDepthV1(const LogicalTypeRegistry &registry);
LogicalType linearViewDepthV1(const LogicalTypeRegistry &registry);
LogicalType legacyOpaqueResourceV1(const LogicalTypeRegistry &registry);

nlohmann::ordered_json typeArgumentValueToJson(const TypeArgumentValue &value);
nlohmann::ordered_json logicalTypeToJson(const LogicalType &type);
std::string canonicalLogicalTypeKey(const LogicalTypeRegistry &registry,
                                    const LogicalType &type);
std::uint64_t canonicalLogicalTypeHash(const LogicalTypeRegistry &registry,
                                       const LogicalType &type);

struct TypeArgumentEquals {
    std::string name;
    TypeArgumentValue expected;
};

struct TypeArgumentOneOf {
    std::string name;
    std::vector<TypeArgumentValue> allowed;
};

struct TypeArgumentSignedRange {
    std::string name;
    std::int64_t minimum = 0;
    std::int64_t maximum = 0;
};

struct TypeArgumentUnsignedRange {
    std::string name;
    std::uint64_t minimum = 0;
    std::uint64_t maximum = 0;
};

struct TypeArgumentRationalRange {
    std::string name;
    Rational minimum;
    Rational maximum;
};

struct TypeArgumentSetContains {
    std::string name;
    EnumValueSet required;
};

using TypeArgumentPredicate =
    std::variant<TypeArgumentEquals, TypeArgumentOneOf,
                 TypeArgumentSignedRange, TypeArgumentUnsignedRange,
                 TypeArgumentRationalRange, TypeArgumentSetContains>;

struct LogicalTypePattern {
    std::optional<LogicalTypeConstructor> constructor;
    std::optional<SemanticTypeId> semantic;
    std::vector<std::string> required_traits;
    std::vector<TypeArgumentPredicate> predicates;
};

LogicalTypePattern exactLogicalTypePattern(const LogicalTypeRegistry &registry,
                                           const LogicalType &type);
nlohmann::ordered_json logicalTypePatternToJson(
    const LogicalTypePattern &pattern);

enum class LogicalTypeMatchStatus : std::uint8_t {
    exact,
    convertible,
    deferred,
    rejected,
};

std::string_view logicalTypeMatchStatusName(LogicalTypeMatchStatus status);

struct TypeBinding {
    SymbolId symbol;
    TypeArgumentValue value;

    bool operator==(const TypeBinding &) const = default;
};

struct LogicalTypeMatchResult {
    LogicalTypeMatchStatus status = LogicalTypeMatchStatus::rejected;
    std::vector<TypeBinding> bindings;
    std::vector<std::string> conversion_path;
    std::string reason_code;
    std::string detail;
};

LogicalTypeMatchResult matchLogicalType(
    const LogicalTypeRegistry &registry, const LogicalType &actual,
    const LogicalTypePattern &pattern);

enum class LogicalConversionMode : std::uint8_t {
    automatic_safe,
    explicit_only,
};

struct LogicalTypeConversion {
    std::string id;
    LogicalType source;
    LogicalType destination;
    LogicalConversionMode mode = LogicalConversionMode::automatic_safe;
    std::uint32_t cost = 1;
};

class LogicalTypeConversionRegistry {
    std::vector<LogicalTypeConversion> conversions_;

  public:
    void registerConversion(const LogicalTypeRegistry &types,
                            LogicalTypeConversion conversion);
    LogicalTypeMatchResult match(
        const LogicalTypeRegistry &types, const LogicalType &actual,
        const LogicalTypePattern &pattern,
        bool allow_explicit_conversions = false) const;
};

} // namespace Pelican
