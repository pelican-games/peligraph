#include "logicalrendertype.hpp"

#include <algorithm>
#include <charconv>
#include <deque>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Pelican {

namespace {

bool isIdentifierCharacter(char value, bool first) {
    if (value >= 'a' && value <= 'z') return true;
    if (!first && value >= '0' && value <= '9') return true;
    return !first && value == '_';
}

void requireIdentifier(std::string_view value, std::string_view subject) {
    if (value.empty()) {
        throw std::runtime_error(std::string{subject} + " must not be empty");
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (!isIdentifierCharacter(value[index], index == 0)) {
            throw std::runtime_error(std::string{subject} + " contains invalid identifier: " +
                                     std::string{value});
        }
    }
}

void requireNamespace(std::string_view value) {
    if (value.empty()) {
        throw std::runtime_error("semantic type namespace must not be empty");
    }
    std::size_t begin = 0;
    while (begin < value.size()) {
        const auto end = value.find('.', begin);
        const auto segment_end = end == std::string_view::npos ? value.size() : end;
        requireIdentifier(value.substr(begin, segment_end - begin),
                          "semantic type namespace segment");
        if (end == std::string_view::npos) return;
        begin = end + 1;
        if (begin == value.size()) {
            throw std::runtime_error("semantic type namespace must not end with '.'");
        }
    }
}

void requireSemanticTypeId(const SemanticTypeId &id) {
    requireNamespace(id.name_space);
    requireIdentifier(id.name, "semantic type name");
    if (id.major_version == 0) {
        throw std::runtime_error("semantic type major version must be greater than zero: " +
                                 id.name_space + "." + id.name);
    }
}

void requireEnumValue(const EnumValueId &value, std::string_view subject) {
    requireIdentifier(value.value, subject);
}

void requireSymbol(const SymbolId &value) {
    requireIdentifier(value.value, "logical type symbol");
}

std::uint64_t signedMagnitude(std::int64_t value) {
    if (value >= 0) return static_cast<std::uint64_t>(value);
    return static_cast<std::uint64_t>(-(value + 1)) + 1;
}

Rational canonicalizeRational(Rational value) {
    if (value.denominator == 0) {
        throw std::runtime_error("logical type rational denominator must not be zero");
    }
    if (value.numerator == 0) return Rational{0, 1};
    const auto magnitude = signedMagnitude(value.numerator);
    const auto divisor = std::gcd(magnitude, value.denominator);
    const auto reduced_magnitude = magnitude / divisor;
    if (value.numerator < 0) {
        constexpr auto minimum_magnitude =
            std::uint64_t{std::numeric_limits<std::int64_t>::max()} + 1;
        value.numerator = reduced_magnitude == minimum_magnitude
                              ? std::numeric_limits<std::int64_t>::min()
                              : -static_cast<std::int64_t>(reduced_magnitude);
    } else {
        value.numerator = static_cast<std::int64_t>(reduced_magnitude);
    }
    value.denominator /= divisor;
    return value;
}

int compareUnsignedFractions(std::uint64_t left_numerator,
                             std::uint64_t left_denominator,
                             std::uint64_t right_numerator,
                             std::uint64_t right_denominator) {
    bool inverted = false;
    while (true) {
        const auto left_quotient = left_numerator / left_denominator;
        const auto right_quotient = right_numerator / right_denominator;
        if (left_quotient != right_quotient) {
            const auto result = left_quotient < right_quotient ? -1 : 1;
            return inverted ? -result : result;
        }
        left_numerator %= left_denominator;
        right_numerator %= right_denominator;
        if (left_numerator == 0 || right_numerator == 0) {
            const auto result = left_numerator == right_numerator
                                    ? 0
                                    : left_numerator == 0 ? -1 : 1;
            return inverted ? -result : result;
        }
        std::swap(left_numerator, left_denominator);
        std::swap(right_numerator, right_denominator);
        inverted = !inverted;
    }
}

int compareRationals(Rational left, Rational right) {
    left = canonicalizeRational(left);
    right = canonicalizeRational(right);
    if (left.numerator < 0 && right.numerator >= 0) return -1;
    if (left.numerator >= 0 && right.numerator < 0) return 1;
    const auto magnitude_comparison = compareUnsignedFractions(
        signedMagnitude(left.numerator), left.denominator,
        signedMagnitude(right.numerator), right.denominator);
    return left.numerator < 0 ? -magnitude_comparison : magnitude_comparison;
}

EnumValueSet canonicalizeEnumSet(EnumValueSet value) {
    for (const auto &entry : value.values) {
        requireEnumValue(entry, "logical type enum-set value");
    }
    std::sort(value.values.begin(), value.values.end());
    value.values.erase(std::unique(value.values.begin(), value.values.end()),
                       value.values.end());
    return value;
}

bool containsEnum(const std::vector<EnumValueId> &values,
                  const EnumValueId &candidate) {
    return std::binary_search(values.begin(), values.end(), candidate);
}

TypeArgumentValue canonicalizeValue(const TypeParameterSchema &parameter,
                                    TypeArgumentValue value) {
    if (const auto *symbol = std::get_if<SymbolId>(&value)) {
        requireSymbol(*symbol);
        return value;
    }
    const auto actual_kind = typeArgumentValueKind(value);
    if (actual_kind != parameter.value_kind) {
        throw std::runtime_error("logical type parameter '" + parameter.name +
                                 "' requires " +
                                 std::string{typeArgumentValueKindName(parameter.value_kind)} +
                                 ", got " +
                                 std::string{typeArgumentValueKindName(actual_kind)});
    }
    switch (parameter.value_kind) {
    case TypeArgumentValueKind::boolean:
        break;
    case TypeArgumentValueKind::signed_integer: {
        const auto number = std::get<std::int64_t>(value);
        if (parameter.signed_minimum && number < *parameter.signed_minimum) {
            throw std::runtime_error("logical type parameter '" + parameter.name +
                                     "' is below its minimum");
        }
        if (parameter.signed_maximum && number > *parameter.signed_maximum) {
            throw std::runtime_error("logical type parameter '" + parameter.name +
                                     "' is above its maximum");
        }
        break;
    }
    case TypeArgumentValueKind::unsigned_integer: {
        const auto number = std::get<std::uint64_t>(value);
        if (parameter.unsigned_minimum && number < *parameter.unsigned_minimum) {
            throw std::runtime_error("logical type parameter '" + parameter.name +
                                     "' is below its minimum");
        }
        if (parameter.unsigned_maximum && number > *parameter.unsigned_maximum) {
            throw std::runtime_error("logical type parameter '" + parameter.name +
                                     "' is above its maximum");
        }
        break;
    }
    case TypeArgumentValueKind::rational:
        value = canonicalizeRational(std::get<Rational>(value));
        break;
    case TypeArgumentValueKind::enum_value: {
        const auto &enum_value = std::get<EnumValueId>(value);
        requireEnumValue(enum_value, "logical type enum value");
        if (!parameter.allowed_enum_values.empty() &&
            !containsEnum(parameter.allowed_enum_values, enum_value)) {
            throw std::runtime_error("logical type parameter '" + parameter.name +
                                     "' has unsupported enum value: " + enum_value.value);
        }
        break;
    }
    case TypeArgumentValueKind::semantic_type:
        requireSemanticTypeId(std::get<SemanticTypeId>(value));
        break;
    case TypeArgumentValueKind::integer_interval: {
        const auto interval = std::get<IntegerInterval>(value);
        if (interval.minimum > interval.maximum) {
            throw std::runtime_error("logical type parameter '" + parameter.name +
                                     "' has an inverted interval");
        }
        break;
    }
    case TypeArgumentValueKind::enum_set:
        value = canonicalizeEnumSet(std::get<EnumValueSet>(value));
        if (!parameter.allowed_enum_values.empty()) {
            for (const auto &entry : std::get<EnumValueSet>(value).values) {
                if (!containsEnum(parameter.allowed_enum_values, entry)) {
                    throw std::runtime_error("logical type parameter '" + parameter.name +
                                             "' has unsupported set value: " + entry.value);
                }
            }
        }
        break;
    case TypeArgumentValueKind::symbol:
        requireSymbol(std::get<SymbolId>(value));
        break;
    }
    return value;
}

const TypeParameterSchema &findParameter(const SemanticTypeSchema &schema,
                                         std::string_view name) {
    const auto found = std::lower_bound(
        schema.parameters.begin(), schema.parameters.end(), name,
        [](const TypeParameterSchema &parameter, std::string_view candidate) {
            return parameter.name < candidate;
        });
    if (found == schema.parameters.end() || found->name != name) {
        throw std::runtime_error("unknown logical type parameter '" + std::string{name} +
                                 "' for " + semanticTypeIdName(schema.id));
    }
    return *found;
}

const LogicalTypeArgument *findArgument(const LogicalType &type,
                                        std::string_view name) {
    const auto found = std::lower_bound(
        type.arguments.begin(), type.arguments.end(), name,
        [](const LogicalTypeArgument &argument, std::string_view candidate) {
            return argument.name < candidate;
        });
    if (found == type.arguments.end() || found->name != name) return nullptr;
    return &*found;
}

std::string conversionPathName(const std::vector<std::string> &path) {
    std::ostringstream stream;
    for (std::size_t index = 0; index < path.size(); ++index) {
        if (index != 0) stream << " -> ";
        stream << path[index];
    }
    return stream.str();
}

void requireVersionedIdString(std::string_view value, std::string_view subject) {
    if (value.empty()) {
        throw std::runtime_error(std::string{subject} + " must not be empty");
    }
    try {
        const auto parsed = parseSemanticTypeId(value);
        if (semanticTypeIdName(parsed) != value) {
            throw std::runtime_error(std::string{subject} + " is not canonical: " +
                                     std::string{value});
        }
    } catch (const std::runtime_error &error) {
        throw std::runtime_error(std::string{subject} +
                                 " must use namespace.name@major: " +
                                 std::string{value} + " (" + error.what() + ")");
    }
}

bool addBinding(std::vector<TypeBinding> &bindings, const SymbolId &symbol,
                const TypeArgumentValue &value, std::string &error) {
    const auto found = std::find_if(bindings.begin(), bindings.end(),
                                    [&](const TypeBinding &binding) {
                                        return binding.symbol == symbol;
                                    });
    if (found == bindings.end()) {
        bindings.push_back(TypeBinding{symbol, value});
        return true;
    }
    if (found->value == value) return true;
    error = "symbol '" + symbol.value + "' received conflicting bindings";
    return false;
}

LogicalTypeMatchResult rejected(std::string code, std::string detail) {
    return LogicalTypeMatchResult{LogicalTypeMatchStatus::rejected, {}, {},
                                  std::move(code), std::move(detail)};
}

} // namespace

std::string_view logicalTypeConstructorName(LogicalTypeConstructor constructor) {
    switch (constructor) {
    case LogicalTypeConstructor::image: return "image";
    case LogicalTypeConstructor::buffer: return "buffer";
    case LogicalTypeConstructor::stream: return "stream";
    case LogicalTypeConstructor::object_set: return "object_set";
    case LogicalTypeConstructor::value: return "value";
    }
    throw std::runtime_error("unknown logical type constructor");
}

bool SemanticTypeId::operator<(const SemanticTypeId &other) const {
    return std::tie(name_space, name, major_version) <
           std::tie(other.name_space, other.name, other.major_version);
}

SemanticTypeId parseSemanticTypeId(std::string_view value) {
    const auto at = value.rfind('@');
    const auto dot = at == std::string_view::npos
                         ? std::string_view::npos
                         : value.rfind('.', at);
    if (at == std::string_view::npos || dot == std::string_view::npos ||
        dot == 0 || dot + 1 == at || at + 1 == value.size()) {
        throw std::runtime_error("semantic type id must use namespace.name@major: " +
                                 std::string{value});
    }
    std::uint32_t major = 0;
    const auto version = value.substr(at + 1);
    const auto parsed = std::from_chars(version.data(), version.data() + version.size(), major);
    if (parsed.ec != std::errc{} || parsed.ptr != version.data() + version.size() ||
        major == 0 || std::to_string(major) != version) {
        throw std::runtime_error("semantic type id has invalid major version: " +
                                 std::string{value});
    }
    SemanticTypeId result{std::string{value.substr(0, dot)},
                          std::string{value.substr(dot + 1, at - dot - 1)}, major};
    requireSemanticTypeId(result);
    return result;
}

std::string semanticTypeIdName(const SemanticTypeId &id) {
    requireSemanticTypeId(id);
    return id.name_space + "." + id.name + "@" + std::to_string(id.major_version);
}

std::string_view typeArgumentValueKindName(TypeArgumentValueKind kind) {
    switch (kind) {
    case TypeArgumentValueKind::boolean: return "boolean";
    case TypeArgumentValueKind::signed_integer: return "signed_integer";
    case TypeArgumentValueKind::unsigned_integer: return "unsigned_integer";
    case TypeArgumentValueKind::rational: return "rational";
    case TypeArgumentValueKind::enum_value: return "enum_value";
    case TypeArgumentValueKind::semantic_type: return "semantic_type";
    case TypeArgumentValueKind::integer_interval: return "integer_interval";
    case TypeArgumentValueKind::enum_set: return "enum_set";
    case TypeArgumentValueKind::symbol: return "symbol";
    }
    throw std::runtime_error("unknown logical type argument kind");
}

TypeArgumentValueKind typeArgumentValueKind(const TypeArgumentValue &value) {
    return std::visit(
        [](const auto &entry) -> TypeArgumentValueKind {
            using T = std::decay_t<decltype(entry)>;
            if constexpr (std::is_same_v<T, bool>) return TypeArgumentValueKind::boolean;
            if constexpr (std::is_same_v<T, std::int64_t>)
                return TypeArgumentValueKind::signed_integer;
            if constexpr (std::is_same_v<T, std::uint64_t>)
                return TypeArgumentValueKind::unsigned_integer;
            if constexpr (std::is_same_v<T, Rational>) return TypeArgumentValueKind::rational;
            if constexpr (std::is_same_v<T, EnumValueId>)
                return TypeArgumentValueKind::enum_value;
            if constexpr (std::is_same_v<T, SemanticTypeId>)
                return TypeArgumentValueKind::semantic_type;
            if constexpr (std::is_same_v<T, IntegerInterval>)
                return TypeArgumentValueKind::integer_interval;
            if constexpr (std::is_same_v<T, EnumValueSet>)
                return TypeArgumentValueKind::enum_set;
            return TypeArgumentValueKind::symbol;
        },
        value);
}

std::string_view typeArgumentRoleName(TypeArgumentRole role) {
    switch (role) {
    case TypeArgumentRole::identity: return "identity";
    case TypeArgumentRole::refinement: return "refinement";
    }
    throw std::runtime_error("unknown logical type argument role");
}

void LogicalTypeRegistry::registerSchema(SemanticTypeSchema schema_value) {
    requireSemanticTypeId(schema_value.id);
    std::sort(schema_value.parameters.begin(), schema_value.parameters.end(),
              [](const TypeParameterSchema &left, const TypeParameterSchema &right) {
                  return left.name < right.name;
              });
    for (std::size_t index = 0; index < schema_value.parameters.size(); ++index) {
        auto &parameter = schema_value.parameters[index];
        requireIdentifier(parameter.name, "logical type parameter name");
        if (index != 0 && schema_value.parameters[index - 1].name == parameter.name) {
            throw std::runtime_error("duplicate logical type parameter '" + parameter.name +
                                     "' in " + semanticTypeIdName(schema_value.id));
        }
        for (const auto &entry : parameter.allowed_enum_values) {
            requireEnumValue(entry, "logical type allowed enum value");
        }
        std::sort(parameter.allowed_enum_values.begin(),
                  parameter.allowed_enum_values.end());
        if (std::adjacent_find(parameter.allowed_enum_values.begin(),
                               parameter.allowed_enum_values.end()) !=
            parameter.allowed_enum_values.end()) {
            throw std::runtime_error("duplicate allowed enum value for parameter '" +
                                     parameter.name + "'");
        }
        if ((parameter.value_kind == TypeArgumentValueKind::enum_value ||
             parameter.value_kind == TypeArgumentValueKind::enum_set) &&
            parameter.allowed_enum_values.empty()) {
            throw std::runtime_error("enum parameter '" + parameter.name +
                                     "' requires allowed values");
        }
        if (parameter.value_kind != TypeArgumentValueKind::enum_value &&
            parameter.value_kind != TypeArgumentValueKind::enum_set &&
            !parameter.allowed_enum_values.empty()) {
            throw std::runtime_error("non-enum parameter '" + parameter.name +
                                     "' must not declare enum values");
        }
        if (parameter.value_kind != TypeArgumentValueKind::signed_integer &&
            (parameter.signed_minimum || parameter.signed_maximum)) {
            throw std::runtime_error("non-signed-integer parameter '" +
                                     parameter.name +
                                     "' must not declare a signed range");
        }
        if (parameter.value_kind != TypeArgumentValueKind::unsigned_integer &&
            (parameter.unsigned_minimum || parameter.unsigned_maximum)) {
            throw std::runtime_error("non-unsigned-integer parameter '" +
                                     parameter.name +
                                     "' must not declare an unsigned range");
        }
        if (parameter.signed_minimum && parameter.signed_maximum &&
            *parameter.signed_minimum > *parameter.signed_maximum) {
            throw std::runtime_error("parameter '" + parameter.name +
                                     "' has an inverted signed range");
        }
        if (parameter.unsigned_minimum && parameter.unsigned_maximum &&
            *parameter.unsigned_minimum > *parameter.unsigned_maximum) {
            throw std::runtime_error("parameter '" + parameter.name +
                                     "' has an inverted unsigned range");
        }
        if (parameter.default_value) {
            if (std::holds_alternative<SymbolId>(*parameter.default_value)) {
                throw std::runtime_error("parameter '" + parameter.name +
                                         "' default must not be symbolic");
            }
            parameter.default_value =
                canonicalizeValue(parameter, std::move(*parameter.default_value));
        }
    }
    for (const auto &trait : schema_value.traits) {
        requireIdentifier(trait, "logical type trait");
    }
    std::sort(schema_value.traits.begin(), schema_value.traits.end());
    if (std::adjacent_find(schema_value.traits.begin(), schema_value.traits.end()) !=
        schema_value.traits.end()) {
        throw std::runtime_error("duplicate logical type trait in " +
                                 semanticTypeIdName(schema_value.id));
    }
    const auto id_name = semanticTypeIdName(schema_value.id);
    if (!schemas_.emplace(schema_value.id, std::move(schema_value)).second) {
        throw std::runtime_error("logical type schema already registered: " + id_name);
    }
}

bool LogicalTypeRegistry::contains(const SemanticTypeId &id) const {
    return schemas_.contains(id);
}

const SemanticTypeSchema &LogicalTypeRegistry::schema(const SemanticTypeId &id) const {
    const auto found = schemas_.find(id);
    if (found == schemas_.end()) {
        throw std::runtime_error("unknown logical type schema: " + semanticTypeIdName(id));
    }
    return found->second;
}

TypeArgumentValue LogicalTypeRegistry::canonicalizeArgumentValue(
    const SemanticTypeId &semantic, std::string_view parameter_name,
    TypeArgumentValue value) const {
    return canonicalizeValue(findParameter(schema(semantic), parameter_name),
                             std::move(value));
}

LogicalType LogicalTypeRegistry::canonicalize(
    const SemanticTypeId &semantic,
    std::vector<LogicalTypeArgumentInput> authored_arguments) const {
    const auto &type_schema = schema(semantic);
    std::map<std::string, TypeArgumentValue, std::less<>> authored;
    for (auto &argument : authored_arguments) {
        requireIdentifier(argument.name, "logical type argument name");
        if (!authored.emplace(argument.name, std::move(argument.value)).second) {
            throw std::runtime_error("duplicate logical type argument '" + argument.name +
                                     "' for " + semanticTypeIdName(semantic));
        }
    }

    LogicalType result;
    result.constructor = type_schema.constructor;
    result.semantic = semantic;
    result.arguments.reserve(type_schema.parameters.size());
    for (const auto &parameter : type_schema.parameters) {
        auto found = authored.find(parameter.name);
        TypeArgumentValue value;
        if (found != authored.end()) {
            value = std::move(found->second);
            authored.erase(found);
        } else if (parameter.default_value) {
            value = *parameter.default_value;
        } else {
            throw std::runtime_error("missing required logical type argument '" +
                                     parameter.name + "' for " +
                                     semanticTypeIdName(semantic));
        }
        result.arguments.push_back(LogicalTypeArgument{
            parameter.name, parameter.role,
            canonicalizeValue(parameter, std::move(value))});
    }
    if (!authored.empty()) {
        throw std::runtime_error("unknown logical type argument '" + authored.begin()->first +
                                 "' for " + semanticTypeIdName(semantic));
    }
    return result;
}

void LogicalTypeRegistry::requireCanonical(const LogicalType &type) const {
    const auto &type_schema = schema(type.semantic);
    if (type.constructor != type_schema.constructor) {
        throw std::runtime_error("logical type constructor mismatch for " +
                                 semanticTypeIdName(type.semantic));
    }
    std::vector<LogicalTypeArgumentInput> inputs;
    inputs.reserve(type.arguments.size());
    for (const auto &argument : type.arguments) {
        inputs.push_back(LogicalTypeArgumentInput{argument.name, argument.value});
    }
    if (canonicalize(type.semantic, std::move(inputs)) != type) {
        throw std::runtime_error("logical type is not canonical: " +
                                 semanticTypeIdName(type.semantic));
    }
}

LogicalTypeRegistry makeBuiltinLogicalTypeRegistry() {
    LogicalTypeRegistry registry;
    const auto topology = TypeParameterSchema{
        "topology", TypeArgumentRole::identity,
        TypeArgumentValueKind::enum_value, EnumValueId{"two_d"},
        {EnumValueId{"two_d"}, EnumValueId{"two_d_array"},
         EnumValueId{"cube"}, EnumValueId{"volume"}}};
    registry.registerSchema(SemanticTypeSchema{
        parseSemanticTypeId("pelican.render.color_signal@1"),
        LogicalTypeConstructor::image,
        {TypeParameterSchema{"range", TypeArgumentRole::identity,
                             TypeArgumentValueKind::enum_value, std::nullopt,
                             {EnumValueId{"extended"}, EnumValueId{"normalized"}}},
         TypeParameterSchema{"reference", TypeArgumentRole::identity,
                             TypeArgumentValueKind::enum_value, std::nullopt,
                             {EnumValueId{"display"}, EnumValueId{"scene"}}},
         topology,
         TypeParameterSchema{"transfer", TypeArgumentRole::identity,
                             TypeArgumentValueKind::enum_value, std::nullopt,
                             {EnumValueId{"linear"},
                              EnumValueId{"output_encoded"}}}},
        {"color_like"}});
    registry.registerSchema(SemanticTypeSchema{
        parseSemanticTypeId("pelican.render.depth@1"),
        LogicalTypeConstructor::image,
        {TypeParameterSchema{"representation", TypeArgumentRole::identity,
                             TypeArgumentValueKind::enum_value, std::nullopt,
                             {EnumValueId{"device"},
                              EnumValueId{"linear_distance"}}},
         TypeParameterSchema{"space", TypeArgumentRole::identity,
                             TypeArgumentValueKind::enum_value, std::nullopt,
                             {EnumValueId{"projection"}, EnumValueId{"view"}}},
         topology},
        {"depth_like"}});
    registry.registerSchema(SemanticTypeSchema{
        parseSemanticTypeId("pelican.render.legacy_opaque_resource@1"),
        LogicalTypeConstructor::value,
        {},
        {"legacy_untyped"}});
    return registry;
}

LogicalType sceneLinearHdrV1(const LogicalTypeRegistry &registry) {
    return registry.canonicalize(
        parseSemanticTypeId("pelican.render.color_signal@1"),
        {{"reference", EnumValueId{"scene"}},
         {"transfer", EnumValueId{"linear"}},
         {"range", EnumValueId{"extended"}}});
}

LogicalType displayLinearV1(const LogicalTypeRegistry &registry) {
    return registry.canonicalize(
        parseSemanticTypeId("pelican.render.color_signal@1"),
        {{"reference", EnumValueId{"display"}},
         {"transfer", EnumValueId{"linear"}},
         {"range", EnumValueId{"normalized"}}});
}

LogicalType displayEncodedV1(const LogicalTypeRegistry &registry) {
    return registry.canonicalize(
        parseSemanticTypeId("pelican.render.color_signal@1"),
        {{"reference", EnumValueId{"display"}},
         {"transfer", EnumValueId{"output_encoded"}},
         {"range", EnumValueId{"normalized"}}});
}

LogicalType deviceDepthV1(const LogicalTypeRegistry &registry) {
    return registry.canonicalize(
        parseSemanticTypeId("pelican.render.depth@1"),
        {{"representation", EnumValueId{"device"}},
         {"space", EnumValueId{"projection"}}});
}

LogicalType linearViewDepthV1(const LogicalTypeRegistry &registry) {
    return registry.canonicalize(
        parseSemanticTypeId("pelican.render.depth@1"),
        {{"representation", EnumValueId{"linear_distance"}},
         {"space", EnumValueId{"view"}}});
}

LogicalType legacyOpaqueResourceV1(const LogicalTypeRegistry &registry) {
    return registry.canonicalize(
        parseSemanticTypeId("pelican.render.legacy_opaque_resource@1"));
}

nlohmann::ordered_json typeArgumentValueToJson(const TypeArgumentValue &value) {
    nlohmann::ordered_json result;
    result["kind"] = typeArgumentValueKindName(typeArgumentValueKind(value));
    std::visit(
        [&](const auto &entry) {
            using T = std::decay_t<decltype(entry)>;
            if constexpr (std::is_same_v<T, bool> ||
                          std::is_same_v<T, std::int64_t> ||
                          std::is_same_v<T, std::uint64_t>) {
                result["value"] = entry;
            } else if constexpr (std::is_same_v<T, Rational>) {
                result["numerator"] = entry.numerator;
                result["denominator"] = entry.denominator;
            } else if constexpr (std::is_same_v<T, EnumValueId> ||
                                 std::is_same_v<T, SymbolId>) {
                result["value"] = entry.value;
            } else if constexpr (std::is_same_v<T, SemanticTypeId>) {
                result["value"] = semanticTypeIdName(entry);
            } else if constexpr (std::is_same_v<T, IntegerInterval>) {
                result["minimum"] = entry.minimum;
                result["maximum"] = entry.maximum;
            } else if constexpr (std::is_same_v<T, EnumValueSet>) {
                result["values"] = nlohmann::ordered_json::array();
                for (const auto &item : entry.values) {
                    result["values"].push_back(item.value);
                }
            }
        },
        value);
    return result;
}

nlohmann::ordered_json logicalTypeToJson(const LogicalType &type) {
    nlohmann::ordered_json result;
    result["schema"] = "pelican.logical_type";
    result["version"] = 1;
    result["constructor"] = logicalTypeConstructorName(type.constructor);
    result["semantic"] = semanticTypeIdName(type.semantic);
    result["arguments"] = nlohmann::ordered_json::array();
    for (const auto &argument : type.arguments) {
        result["arguments"].push_back(
            nlohmann::ordered_json{{"name", argument.name},
                                   {"role", typeArgumentRoleName(argument.role)},
                                   {"value", typeArgumentValueToJson(argument.value)}});
    }
    return result;
}

std::string canonicalLogicalTypeKey(const LogicalTypeRegistry &registry,
                                    const LogicalType &type) {
    registry.requireCanonical(type);
    return logicalTypeToJson(type).dump();
}

std::uint64_t canonicalLogicalTypeHash(const LogicalTypeRegistry &registry,
                                       const LogicalType &type) {
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    auto hash = offset;
    for (const auto byte : canonicalLogicalTypeKey(registry, type)) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= prime;
    }
    return hash;
}

LogicalTypePattern exactLogicalTypePattern(const LogicalTypeRegistry &registry,
                                           const LogicalType &type) {
    registry.requireCanonical(type);
    LogicalTypePattern pattern;
    pattern.constructor = type.constructor;
    pattern.semantic = type.semantic;
    pattern.predicates.reserve(type.arguments.size());
    for (const auto &argument : type.arguments) {
        pattern.predicates.push_back(TypeArgumentEquals{argument.name, argument.value});
    }
    return pattern;
}

nlohmann::ordered_json logicalTypePatternToJson(
    const LogicalTypePattern &pattern) {
    nlohmann::ordered_json result;
    if (pattern.constructor) {
        result["constructor"] = logicalTypeConstructorName(*pattern.constructor);
    }
    if (pattern.semantic) result["semantic"] = semanticTypeIdName(*pattern.semantic);
    result["required_traits"] = pattern.required_traits;
    result["predicates"] = nlohmann::ordered_json::array();
    for (const auto &predicate : pattern.predicates) {
        std::visit(
            [&](const auto &entry) {
                using T = std::decay_t<decltype(entry)>;
                nlohmann::ordered_json encoded{{"name", entry.name}};
                if constexpr (std::is_same_v<T, TypeArgumentEquals>) {
                    encoded["operation"] = "equals";
                    encoded["expected"] = typeArgumentValueToJson(entry.expected);
                } else if constexpr (std::is_same_v<T, TypeArgumentOneOf>) {
                    encoded["operation"] = "one_of";
                    encoded["allowed"] = nlohmann::ordered_json::array();
                    for (const auto &allowed : entry.allowed) {
                        encoded["allowed"].push_back(typeArgumentValueToJson(allowed));
                    }
                } else if constexpr (std::is_same_v<T, TypeArgumentSignedRange>) {
                    encoded["operation"] = "signed_range";
                    encoded["minimum"] = entry.minimum;
                    encoded["maximum"] = entry.maximum;
                } else if constexpr (std::is_same_v<T, TypeArgumentUnsignedRange>) {
                    encoded["operation"] = "unsigned_range";
                    encoded["minimum"] = entry.minimum;
                    encoded["maximum"] = entry.maximum;
                } else if constexpr (std::is_same_v<T, TypeArgumentRationalRange>) {
                    encoded["operation"] = "rational_range";
                    encoded["minimum"] =
                        typeArgumentValueToJson(TypeArgumentValue{entry.minimum});
                    encoded["maximum"] =
                        typeArgumentValueToJson(TypeArgumentValue{entry.maximum});
                } else {
                    encoded["operation"] = "set_contains";
                    encoded["required"] = nlohmann::ordered_json::array();
                    for (const auto &required : entry.required.values) {
                        encoded["required"].push_back(required.value);
                    }
                }
                result["predicates"].push_back(std::move(encoded));
            },
            predicate);
    }
    return result;
}

std::string_view logicalTypeMatchStatusName(LogicalTypeMatchStatus status) {
    switch (status) {
    case LogicalTypeMatchStatus::exact: return "exact";
    case LogicalTypeMatchStatus::convertible: return "convertible";
    case LogicalTypeMatchStatus::deferred: return "deferred";
    case LogicalTypeMatchStatus::rejected: return "rejected";
    }
    throw std::runtime_error("unknown logical type match status");
}

LogicalTypeMatchResult matchLogicalType(
    const LogicalTypeRegistry &registry, const LogicalType &actual,
    const LogicalTypePattern &pattern) {
    registry.requireCanonical(actual);
    if (pattern.constructor && *pattern.constructor != actual.constructor) {
        return rejected("constructor_mismatch", "actual constructor is " +
                                                    std::string{logicalTypeConstructorName(
                                                        actual.constructor)});
    }
    if (pattern.semantic && *pattern.semantic != actual.semantic) {
        return rejected("semantic_mismatch", "actual semantic is " +
                                                 semanticTypeIdName(actual.semantic));
    }
    const auto &schema = registry.schema(actual.semantic);
    std::set<std::string, std::less<>> required_traits;
    for (const auto &trait : pattern.required_traits) {
        requireIdentifier(trait, "logical type required trait");
        if (!required_traits.insert(trait).second) {
            return rejected("duplicate_trait", "duplicate required trait: " + trait);
        }
        if (!std::binary_search(schema.traits.begin(), schema.traits.end(), trait)) {
            return rejected("trait_missing", "logical type " +
                                                 semanticTypeIdName(actual.semantic) +
                                                 " does not provide trait " + trait);
        }
    }

    LogicalTypeMatchResult result;
    result.status = LogicalTypeMatchStatus::exact;
    result.reason_code = "exact_match";
    result.detail = "logical type satisfies pattern";
    bool deferred_match = false;
    for (const auto &predicate : pattern.predicates) {
        const auto predicate_result = std::visit(
            [&](const auto &entry) -> LogicalTypeMatchResult {
                using T = std::decay_t<decltype(entry)>;
                const auto *argument = findArgument(actual, entry.name);
                if (!argument) {
                    return rejected("argument_missing", "logical type argument is missing: " +
                                                            entry.name);
                }
                if constexpr (std::is_same_v<T, TypeArgumentEquals>) {
                    const auto expected = registry.canonicalizeArgumentValue(
                        actual.semantic, entry.name, entry.expected);
                    const auto *actual_symbol = std::get_if<SymbolId>(&argument->value);
                    const auto *expected_symbol = std::get_if<SymbolId>(&expected);
                    if (actual_symbol || expected_symbol) {
                        std::string binding_error;
                        if (actual_symbol && expected_symbol) {
                            if (*actual_symbol != *expected_symbol &&
                                !addBinding(result.bindings, *actual_symbol, expected,
                                            binding_error)) {
                                return rejected("binding_conflict", binding_error);
                            }
                            deferred_match = true;
                            return result;
                        }
                        if (actual_symbol) {
                            if (!addBinding(result.bindings, *actual_symbol, expected,
                                            binding_error)) {
                                return rejected("binding_conflict", binding_error);
                            }
                            deferred_match = true;
                            return result;
                        }
                        if (!addBinding(result.bindings, *expected_symbol,
                                        argument->value, binding_error)) {
                            return rejected("binding_conflict", binding_error);
                        }
                        return result;
                    }
                    if (argument->value != expected) {
                        return rejected("argument_mismatch", "logical type argument '" +
                                                                 entry.name +
                                                                 "' does not match");
                    }
                    return result;
                } else if constexpr (std::is_same_v<T, TypeArgumentOneOf>) {
                    if (entry.allowed.empty()) {
                        return rejected("empty_one_of", "one_of predicate is empty for " +
                                                            entry.name);
                    }
                    if (std::holds_alternative<SymbolId>(argument->value)) {
                        deferred_match = true;
                        return result;
                    }
                    for (const auto &candidate : entry.allowed) {
                        const auto canonical = registry.canonicalizeArgumentValue(
                            actual.semantic, entry.name, candidate);
                        if (std::holds_alternative<SymbolId>(canonical)) {
                            return rejected("symbolic_one_of",
                                            "one_of candidates must be concrete");
                        }
                        if (argument->value == canonical) return result;
                    }
                    return rejected("argument_not_allowed", "logical type argument '" +
                                                               entry.name +
                                                               "' is outside one_of");
                } else if constexpr (std::is_same_v<T, TypeArgumentSignedRange>) {
                    if (entry.minimum > entry.maximum) {
                        return rejected("inverted_range", "predicate range is inverted for " +
                                                              entry.name);
                    }
                    if (std::holds_alternative<SymbolId>(argument->value)) {
                        deferred_match = true;
                        return result;
                    }
                    const auto *number = std::get_if<std::int64_t>(&argument->value);
                    if (!number) {
                        return rejected("range_kind_mismatch",
                                        "signed range requires signed integer: " + entry.name);
                    }
                    if (*number < entry.minimum || *number > entry.maximum) {
                        return rejected("argument_out_of_range", "logical type argument '" +
                                                                  entry.name +
                                                                  "' is outside range");
                    }
                    return result;
                } else if constexpr (std::is_same_v<T, TypeArgumentUnsignedRange>) {
                    if (entry.minimum > entry.maximum) {
                        return rejected("inverted_range", "predicate range is inverted for " +
                                                              entry.name);
                    }
                    if (std::holds_alternative<SymbolId>(argument->value)) {
                        deferred_match = true;
                        return result;
                    }
                    const auto *number = std::get_if<std::uint64_t>(&argument->value);
                    if (!number) {
                        return rejected(
                            "range_kind_mismatch",
                            "unsigned range requires unsigned integer: " + entry.name);
                    }
                    if (*number < entry.minimum || *number > entry.maximum) {
                        return rejected("argument_out_of_range", "logical type argument '" +
                                                                  entry.name +
                                                                  "' is outside range");
                    }
                    return result;
                } else if constexpr (std::is_same_v<T, TypeArgumentRationalRange>) {
                    const auto minimum = canonicalizeRational(entry.minimum);
                    const auto maximum = canonicalizeRational(entry.maximum);
                    if (compareRationals(minimum, maximum) > 0) {
                        return rejected("inverted_range", "predicate range is inverted for " +
                                                              entry.name);
                    }
                    if (std::holds_alternative<SymbolId>(argument->value)) {
                        deferred_match = true;
                        return result;
                    }
                    const auto *number = std::get_if<Rational>(&argument->value);
                    if (!number) {
                        return rejected("range_kind_mismatch",
                                        "rational range requires rational: " + entry.name);
                    }
                    if (compareRationals(*number, minimum) < 0 ||
                        compareRationals(*number, maximum) > 0) {
                        return rejected("argument_out_of_range", "logical type argument '" +
                                                                  entry.name +
                                                                  "' is outside range");
                    }
                    return result;
                } else {
                    if (std::holds_alternative<SymbolId>(argument->value)) {
                        deferred_match = true;
                        return result;
                    }
                    const auto *values = std::get_if<EnumValueSet>(&argument->value);
                    if (!values) {
                        return rejected("set_kind_mismatch",
                                        "set_contains requires enum set: " + entry.name);
                    }
                    const auto required = canonicalizeEnumSet(entry.required);
                    for (const auto &required_value : required.values) {
                        if (!containsEnum(values->values, required_value)) {
                            return rejected("set_value_missing", "logical type argument '" +
                                                                     entry.name +
                                                                     "' lacks " +
                                                                     required_value.value);
                        }
                    }
                    return result;
                }
            },
            predicate);
        if (predicate_result.status == LogicalTypeMatchStatus::rejected) {
            return predicate_result;
        }
    }
    std::sort(result.bindings.begin(), result.bindings.end(),
              [](const TypeBinding &left, const TypeBinding &right) {
                  return left.symbol < right.symbol;
              });
    if (deferred_match) {
        result.status = LogicalTypeMatchStatus::deferred;
        result.reason_code = "symbolic_constraint_deferred";
        result.detail = "logical type match requires target-time binding";
    }
    return result;
}

void LogicalTypeConversionRegistry::registerConversion(
    const LogicalTypeRegistry &types, LogicalTypeConversion conversion) {
    requireVersionedIdString(conversion.id, "logical type conversion id");
    requireVersionedIdString(conversion.implementation.operation,
                             "logical conversion operation id");
    requireVersionedIdString(conversion.implementation.provider,
                             "logical conversion provider id");
    const auto has_provider_identity =
        conversion.implementation.provider_identity != 0;
    const auto has_provider_generation =
        conversion.implementation.provider_generation != 0;
    if (has_provider_identity != has_provider_generation) {
        throw std::runtime_error(
            "logical conversion provider identity and generation must both be zero "
            "for static providers or both be non-zero for reloadable providers: " +
            conversion.id);
    }
    types.requireCanonical(conversion.source);
    types.requireCanonical(conversion.destination);
    if (conversion.source == conversion.destination) {
        throw std::runtime_error("logical type conversion must change its type: " +
                                 conversion.id);
    }
    if (conversion.cost == 0) {
        throw std::runtime_error("logical type conversion cost must be positive: " +
                                 conversion.id);
    }
    if (std::any_of(conversions_.begin(), conversions_.end(), [&](const auto &entry) {
            return entry.id == conversion.id;
        })) {
        throw std::runtime_error("logical type conversion already registered: " +
                                 conversion.id);
    }
    conversions_.push_back(std::move(conversion));
    std::sort(conversions_.begin(), conversions_.end(),
              [](const LogicalTypeConversion &left,
                 const LogicalTypeConversion &right) { return left.id < right.id; });
}

const LogicalTypeConversion &LogicalTypeConversionRegistry::conversion(
    std::string_view id) const {
    const auto found = std::lower_bound(
        conversions_.begin(), conversions_.end(), id,
        [](const LogicalTypeConversion &entry, std::string_view candidate) {
            return entry.id < candidate;
        });
    if (found == conversions_.end() || found->id != id) {
        throw std::runtime_error("logical type conversion is not registered: " +
                                 std::string{id});
    }
    return *found;
}

LogicalTypeMatchResult LogicalTypeConversionRegistry::match(
    const LogicalTypeRegistry &types, const LogicalType &actual,
    const LogicalTypePattern &pattern, bool allow_explicit_conversions) const {
    const auto direct = matchLogicalType(types, actual, pattern);
    if (direct.status != LogicalTypeMatchStatus::rejected) return direct;

    struct SearchState {
        LogicalType type;
        std::uint64_t cost = 0;
        std::vector<std::string> path;
        std::vector<std::string> visited_types;
    };
    struct Candidate {
        std::uint64_t cost = 0;
        std::vector<std::string> path;
        LogicalTypeMatchResult match;
    };

    std::deque<SearchState> pending;
    pending.push_back(
        SearchState{actual, 0, {}, {canonicalLogicalTypeKey(types, actual)}});
    std::vector<Candidate> candidates;
    std::optional<std::uint64_t> best_cost;
    std::size_t expanded_states = 0;
    constexpr std::size_t max_states = 4096;

    while (!pending.empty()) {
        auto state = std::move(pending.front());
        pending.pop_front();
        if (++expanded_states > max_states) {
            return rejected("conversion_search_limit",
                            "logical type conversion search exceeded 4096 states");
        }
        for (const auto &conversion : conversions_) {
            if (conversion.source != state.type) continue;
            if (conversion.mode == LogicalConversionMode::explicit_only &&
                !allow_explicit_conversions) {
                continue;
            }
            const auto destination_key =
                canonicalLogicalTypeKey(types, conversion.destination);
            if (std::find(state.visited_types.begin(), state.visited_types.end(),
                          destination_key) != state.visited_types.end()) {
                continue;
            }
            if (state.cost > std::numeric_limits<std::uint64_t>::max() -
                                 conversion.cost) {
                return rejected("conversion_cost_overflow",
                                "logical type conversion cost overflow");
            }
            const auto next_cost = state.cost + conversion.cost;
            if (best_cost && next_cost > *best_cost) continue;
            auto next_path = state.path;
            next_path.push_back(conversion.id);
            auto candidate_match =
                matchLogicalType(types, conversion.destination, pattern);
            if (candidate_match.status == LogicalTypeMatchStatus::exact) {
                if (!best_cost || next_cost < *best_cost) {
                    best_cost = next_cost;
                    candidates.clear();
                }
                if (next_cost == *best_cost) {
                    candidates.push_back(Candidate{next_cost, next_path,
                                                   std::move(candidate_match)});
                }
            }
            if ((!best_cost || next_cost < *best_cost) &&
                next_path.size() < conversions_.size()) {
                auto visited = state.visited_types;
                visited.push_back(std::move(destination_key));
                pending.push_back(SearchState{conversion.destination, next_cost,
                                              std::move(next_path),
                                              std::move(visited)});
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &left, const Candidate &right) {
                  return left.path < right.path;
              });
    candidates.erase(
        std::unique(candidates.begin(), candidates.end(),
                    [](const Candidate &left, const Candidate &right) {
                        return left.path == right.path;
                    }),
        candidates.end());
    if (candidates.empty()) {
        return rejected("no_conversion", direct.detail +
                                             "; no permitted conversion path exists");
    }
    if (candidates.size() != 1) {
        std::ostringstream detail;
        detail << "ambiguous logical type conversion paths at cost " << *best_cost << ": ";
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            if (index != 0) detail << "; ";
            detail << conversionPathName(candidates[index].path);
        }
        return rejected("ambiguous_conversion", detail.str());
    }
    auto result = std::move(candidates.front().match);
    result.status = LogicalTypeMatchStatus::convertible;
    result.conversion_path = std::move(candidates.front().path);
    result.reason_code = "conversion_selected";
    result.detail = "selected logical type conversion path: " +
                    conversionPathName(result.conversion_path);
    return result;
}

LogicalTypeConversionRegistry makeBuiltinLogicalTypeConversionRegistry(
    const LogicalTypeRegistry &types) {
    const LogicalConversionImplementation depth_implementation{
        "pelican.render.depth_linearize@1",
        "pelican.render.builtin_conversion_provider@1", 0, 0};
    const LogicalConversionImplementation tone_implementation{
        "pelican.render.tone_map@1",
        "pelican.render.builtin_conversion_provider@1", 0, 0};
    const LogicalConversionImplementation encode_implementation{
        "pelican.render.display_encode@1",
        "pelican.render.builtin_conversion_provider@1", 0, 0};

    LogicalTypeConversionRegistry result;
    result.registerConversion(
        types,
        {"pelican.render.depth_linearize@1", deviceDepthV1(types),
         linearViewDepthV1(types), LogicalConversionMode::automatic_safe, 1,
         depth_implementation});
    result.registerConversion(
        types,
        {"pelican.render.tone_map@1", sceneLinearHdrV1(types),
         displayLinearV1(types), LogicalConversionMode::explicit_only, 1,
         tone_implementation});
    result.registerConversion(
        types,
        {"pelican.render.display_encode@1", displayLinearV1(types),
         displayEncodedV1(types), LogicalConversionMode::explicit_only, 1,
         encode_implementation});
    return result;
}

} // namespace Pelican
