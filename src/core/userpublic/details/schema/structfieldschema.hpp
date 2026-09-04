#pragma once

#include "../../geom/quat.hpp"
#include "../../geom/vec.hpp"

#include <array>
#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Pelican {

// StructFieldSchema is deliberately use-site neutral. Whether a kind is legal,
// and whether a field is required or defaulted, is decided by the policy layer.
enum class StructFieldType : std::uint8_t {
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
    Flags,
    List,
};

using StructFieldRange = std::variant<std::monostate, std::pair<std::int64_t, std::int64_t>,
                                      std::pair<std::uint64_t, std::uint64_t>, std::pair<double, double>>;

inline constexpr std::size_t maxStructFieldEnumValues = 8;

struct StructFieldSchema {
    std::string_view name;
    StructFieldType type;
    StructFieldRange range;
    std::string_view unit;
    // Flat owned storage avoids self-referential spans and preserves the MSVC
    // constexpr-in-incomplete-owner invariant used by WP71 fixtures.
    std::string_view enum_values[maxStructFieldEnumValues]{};
    std::size_t enum_value_count = 0;

    constexpr StructFieldSchema(std::string_view field_name, StructFieldType field_type,
                                 StructFieldRange field_range, std::string_view field_unit = {})
        : name(field_name), type(field_type), range(std::move(field_range)), unit(field_unit) {}
};

enum class StructFieldPresence : std::uint8_t {
    Required,
    Defaulted,
    OptionalOverride,
};

// V3 descriptors are fixed-capacity constexpr values.  They are deliberately
// non-owning: every string_view refers to a declaration literal and all tree
// storage lives in this value.  A host converts the descriptor to its owning
// Schema::Node model at a v3 entrance.  Keeping this type allocation-free is
// important because game DLLs construct their descriptors during static
// initialization.
enum class SchemaNodeTag : std::uint8_t { Struct, Leaf, Flags, List };

struct SchemaVocabularyValue {
    std::string_view name;
    std::uint64_t value = 0;
    bool signed_value = false;

    friend bool operator==(const SchemaVocabularyValue &,
                           const SchemaVocabularyValue &) = default;
};

enum class SchemaDefaultKind : std::uint8_t {
    None,
    EmptyList,
    Bool,
    Signed,
    Unsigned,
    Floating,
    String,
    Enum,
    Flags,
    Vector,
};

namespace internal {

enum class StructFieldRangeKind : std::uint8_t { None, Signed, Unsigned, Floating };

struct StructFieldRangeDescriptor {
    StructFieldRangeKind kind = StructFieldRangeKind::None;
    std::int64_t signed_min = 0;
    std::int64_t signed_max = 0;
    std::uint64_t unsigned_min = 0;
    std::uint64_t unsigned_max = 0;
    double floating_min = 0;
    double floating_max = 0;

    friend bool operator==(const StructFieldRangeDescriptor &,
                           const StructFieldRangeDescriptor &) = default;
};

} // namespace internal

struct SchemaDefaultValue {
    SchemaDefaultKind kind = SchemaDefaultKind::None;
    std::int64_t signed_value = 0;
    std::uint64_t unsigned_value = 0;
    std::array<double, 4> components{};
    std::uint8_t component_count = 0;
    std::string_view string_value;

    friend bool operator==(const SchemaDefaultValue &,
                           const SchemaDefaultValue &) = default;
};

inline constexpr std::size_t maxSchemaNodeCount = 64;
inline constexpr std::size_t maxSchemaVocabularyValues =
    maxStructFieldEnumValues;
inline constexpr std::uint16_t noSchemaNodeParent = UINT16_MAX;

struct SchemaNodeEntry {
    SchemaNodeTag tag = SchemaNodeTag::Struct;
    std::string_view name;
    StructFieldPresence presence = StructFieldPresence::Required;
    StructFieldType leaf_type = StructFieldType::String;
    internal::StructFieldRangeDescriptor range;
    std::string_view unit;
    SchemaDefaultValue default_value;
    std::array<SchemaVocabularyValue, maxSchemaVocabularyValues> vocabulary{};
    std::uint8_t vocabulary_count = 0;
    std::uint16_t parent = noSchemaNodeParent;

    friend bool operator==(const SchemaNodeEntry &,
                           const SchemaNodeEntry &) = default;
};

struct SchemaNode {
    std::array<SchemaNodeEntry, maxSchemaNodeCount> nodes{};
    std::uint16_t node_count = 1;

    static SchemaNode fromFlat(std::span<const StructFieldSchema> fields);
    bool recursive() const noexcept;

    constexpr const SchemaNodeEntry &root() const noexcept { return nodes[0]; }
    constexpr const SchemaNodeEntry &at(std::size_t index) const noexcept {
        return nodes[index];
    }
    constexpr std::size_t childCount(std::size_t parent) const noexcept {
        std::size_t count = 0;
        for (std::size_t index = 1; index < node_count; ++index)
            count += nodes[index].parent == parent;
        return count;
    }
    constexpr std::size_t childIndex(std::size_t parent,
                                     std::size_t ordinal) const noexcept {
        for (std::size_t index = 1; index < node_count; ++index) {
            if (nodes[index].parent != parent) continue;
            if (ordinal-- == 0) return index;
        }
        return node_count;
    }

    friend bool operator==(const SchemaNode &, const SchemaNode &) = default;
};

template <class Enum> class Flags {
    static_assert(std::is_enum_v<Enum>, "Flags vocabulary must be an enum");
    using Raw = std::underlying_type_t<Enum>;
    using Storage = std::make_unsigned_t<Raw>;
    Storage bits_ = 0;

  public:
    using enum_type = Enum;
    using storage_type = Storage;

    constexpr Flags() noexcept = default;
    constexpr explicit Flags(Storage bits) noexcept : bits_{bits} {}
    constexpr Storage bits() const noexcept { return bits_; }
    constexpr bool contains(Enum value) const noexcept {
        const auto requested = static_cast<Storage>(value);
        return (bits_ & requested) == requested;
    }
    constexpr void set(Enum value) noexcept {
        bits_ |= static_cast<Storage>(value);
    }
    friend constexpr bool operator==(Flags, Flags) noexcept = default;
};

struct RowEnvelope {
    std::uint32_t id = 0;
    std::uint32_t version = 1;
    std::optional<std::string> hint_json;

    friend bool operator==(const RowEnvelope &, const RowEnvelope &) = default;
};

template <class Row> struct ListRow {
    RowEnvelope envelope;
    Row value;

    friend bool operator==(const ListRow &, const ListRow &) = default;
};

template <class Row> struct List {
    using row_type = Row;
    std::uint32_t next_id = 1;
    std::vector<ListRow<Row>> rows;

    friend bool operator==(const List &, const List &) = default;
};

inline SchemaNode SchemaNode::fromFlat(
    std::span<const StructFieldSchema> fields) {
    SchemaNode root;
    if (fields.size() + 1 > maxSchemaNodeCount)
        throw std::length_error("flat schema exceeds SchemaNode capacity");
    for (const auto &field : fields) {
        SchemaNodeEntry child;
        child.tag = field.type == StructFieldType::Flags
                        ? SchemaNodeTag::Flags
                        : (field.type == StructFieldType::List
                               ? SchemaNodeTag::List
                               : SchemaNodeTag::Leaf);
        child.name = field.name;
        child.leaf_type = field.type;
        child.range = std::visit(
            [](const auto &range) -> internal::StructFieldRangeDescriptor {
                using R = std::decay_t<decltype(range)>;
                if constexpr (std::same_as<R, std::monostate>) return {};
                else if constexpr (std::same_as<R, std::pair<std::int64_t,
                                                             std::int64_t>>)
                    return {.kind = internal::StructFieldRangeKind::Signed,
                            .signed_min = range.first,
                            .signed_max = range.second};
                else if constexpr (std::same_as<R, std::pair<std::uint64_t,
                                                             std::uint64_t>>)
                    return {.kind = internal::StructFieldRangeKind::Unsigned,
                            .unsigned_min = range.first,
                            .unsigned_max = range.second};
                else
                    return {.kind = internal::StructFieldRangeKind::Floating,
                            .floating_min = range.first,
                            .floating_max = range.second};
            }, field.range);
        child.unit = field.unit;
        child.parent = 0;
        for (std::size_t index = 0; index < field.enum_value_count; ++index) {
            child.vocabulary[child.vocabulary_count++] =
                SchemaVocabularyValue{field.enum_values[index], index, false};
        }
        root.nodes[root.node_count++] = child;
    }
    return root;
}

inline bool SchemaNode::recursive() const noexcept {
    for (std::size_t index = 0; index < node_count; ++index)
        if (nodes[index].tag == SchemaNodeTag::Flags ||
            nodes[index].tag == SchemaNodeTag::List)
            return true;
    return false;
}

struct EventPayloadPolicy final {};
struct BehaviorParamsPolicy final {};

class ComponentPolicy final {
    std::string_view codec;

  public:
    ComponentPolicy() = delete;

    explicit consteval ComponentPolicy(std::string_view codec_name) : codec(codec_name) {
        if (codec.empty()) {
            throw "component policy requires a non-empty codec name";
        }
    }

    constexpr std::string_view codecName() const noexcept {
        return codec;
    }
};

inline constexpr EventPayloadPolicy eventPayloadPolicy{};
inline constexpr BehaviorParamsPolicy behaviorParamsPolicy{};

consteval ComponentPolicy componentPolicy(std::string_view codec_name) {
    return ComponentPolicy{codec_name};
}

namespace internal {

template <class> inline constexpr bool always_false_v = false;

template <class T> struct OptionalTraits { static constexpr bool value = false; };
template <class T> struct OptionalTraits<std::optional<T>> {
    static constexpr bool value = true;
    using value_type = T;
};
template <class T> inline constexpr bool is_optional_v = OptionalTraits<T>::value;

template <class T> struct FlagsTraits { static constexpr bool value = false; };
template <class Enum> struct FlagsTraits<Flags<Enum>> {
    static constexpr bool value = true;
    using enum_type = Enum;
};
template <class T> inline constexpr bool is_flags_v = FlagsTraits<T>::value;

template <class T> struct ListTraits { static constexpr bool value = false; };
template <class Row> struct ListTraits<List<Row>> {
    static constexpr bool value = true;
    using row_type = Row;
};
template <class T> inline constexpr bool is_list_v = ListTraits<T>::value;

template <class T> struct UnwrapOptional { using type = T; };
template <class T> struct UnwrapOptional<std::optional<T>> { using type = T; };
template <class T> using unwrap_optional_t = typename UnwrapOptional<T>::type;

template <class> struct MemberPointerTraits;
template <class Owner, class Member> struct MemberPointerTraits<Member Owner::*> {
    using owner_type = Owner;
    using member_type = std::remove_cv_t<Member>;
};

struct NoStructFieldRange {};
struct SignedStructFieldRange {
    std::int64_t min;
    std::int64_t max;
};
struct UnsignedStructFieldRange {
    std::uint64_t min;
    std::uint64_t max;
};
struct FloatingStructFieldRange {
    double min;
    double max;
};

// POD form used while the owner type is still incomplete. This preserves the
// WP71/MSVC invariant that no variant is constructed in the class definition.
struct StructFieldDescriptor {
    std::string_view name;
    StructFieldType type;
    StructFieldRangeDescriptor range;
    std::string_view unit;
    std::string_view enum_values[maxStructFieldEnumValues]{};
    std::size_t enum_value_count = 0;
};

template <class T> consteval StructFieldType structFieldType() {
    using Value = unwrap_optional_t<T>;
    if constexpr (is_list_v<Value>) {
        return StructFieldType::List;
    } else if constexpr (is_flags_v<Value>) {
        return StructFieldType::Flags;
    } else if constexpr (std::same_as<Value, std::int8_t>) {
        return StructFieldType::I8;
    } else if constexpr (std::same_as<Value, std::int16_t>) {
        return StructFieldType::I16;
    } else if constexpr (std::same_as<Value, std::int32_t>) {
        return StructFieldType::I32;
    } else if constexpr (std::same_as<Value, std::int64_t>) {
        return StructFieldType::I64;
    } else if constexpr (std::same_as<Value, std::uint8_t>) {
        return StructFieldType::U8;
    } else if constexpr (std::same_as<Value, std::uint16_t>) {
        return StructFieldType::U16;
    } else if constexpr (std::same_as<Value, std::uint32_t>) {
        return StructFieldType::U32;
    } else if constexpr (std::same_as<Value, std::uint64_t>) {
        return StructFieldType::U64;
    } else if constexpr (std::same_as<Value, float>) {
        return StructFieldType::F32;
    } else if constexpr (std::same_as<Value, double>) {
        return StructFieldType::F64;
    } else if constexpr (std::same_as<Value, vec2>) {
        return StructFieldType::Vec2;
    } else if constexpr (std::same_as<Value, vec3>) {
        return StructFieldType::Vec3;
    } else if constexpr (std::same_as<Value, vec4>) {
        return StructFieldType::Vec4;
    } else if constexpr (std::same_as<Value, quat>) {
        return StructFieldType::Quat;
    } else if constexpr (std::same_as<Value, std::string>) {
        return StructFieldType::String;
    } else if constexpr (std::same_as<Value, bool>) {
        return StructFieldType::Bool;
    } else if constexpr (std::is_enum_v<Value>) {
        return StructFieldType::Enum;
    } else {
        static_assert(always_false_v<Value>, "struct field type is not supported");
    }
}

consteval void validateFloatingRange(FloatingStructFieldRange range, StructFieldType type) {
    constexpr double max_f64 = std::numeric_limits<double>::max();
    const auto finite = [](double value) { return value == value && value >= -max_f64 && value <= max_f64; };
    if (!finite(range.min) || !finite(range.max)) {
        throw "struct field range bounds must be finite";
    }
    if (range.min > range.max) {
        throw "struct field range minimum exceeds maximum";
    }
    if (type == StructFieldType::F32 || type == StructFieldType::Vec2 || type == StructFieldType::Vec3 ||
        type == StructFieldType::Vec4) {
        constexpr double max_f32 = static_cast<double>(std::numeric_limits<float>::max());
        if (range.min < -max_f32 || range.min > max_f32 || range.max < -max_f32 || range.max > max_f32) {
            throw "struct field F32 range bound is not representable as F32";
        }
    }
}

constexpr StructFieldRange materializeStructFieldRange(const StructFieldRangeDescriptor &range) {
    switch (range.kind) {
    case StructFieldRangeKind::None:
        return StructFieldRange{std::monostate{}};
    case StructFieldRangeKind::Signed:
        return StructFieldRange{std::pair<std::int64_t, std::int64_t>{range.signed_min, range.signed_max}};
    case StructFieldRangeKind::Unsigned:
        return StructFieldRange{std::pair<std::uint64_t, std::uint64_t>{range.unsigned_min, range.unsigned_max}};
    case StructFieldRangeKind::Floating:
        return StructFieldRange{std::pair<double, double>{range.floating_min, range.floating_max}};
    }
    return StructFieldRange{std::monostate{}};
}

constexpr StructFieldSchema materializeStructField(const StructFieldDescriptor &field) {
    StructFieldSchema result{field.name, field.type,
                             materializeStructFieldRange(field.range), field.unit};
    result.enum_value_count = field.enum_value_count;
    for (std::size_t index = 0; index < field.enum_value_count; ++index) {
        result.enum_values[index] = field.enum_values[index];
    }
    return result;
}

template <class T>
inline constexpr bool is_event_field_type_v = !std::same_as<T, bool> && !std::is_enum_v<T>;

} // namespace internal

constexpr internal::SignedStructFieldRange irange(std::int64_t min, std::int64_t max) {
    return internal::SignedStructFieldRange{min, max};
}

constexpr internal::UnsignedStructFieldRange urange(std::uint64_t min, std::uint64_t max) {
    return internal::UnsignedStructFieldRange{min, max};
}

constexpr internal::FloatingStructFieldRange frange(double min, double max) {
    return internal::FloatingStructFieldRange{min, max};
}

template <auto Member> struct StructFieldDeclaration {
    static_assert(std::is_member_object_pointer_v<decltype(Member)>,
                  "field requires a pointer to a data member");

    using traits = internal::MemberPointerTraits<decltype(Member)>;
    using owner_type = typename traits::owner_type;
    using member_type = typename traits::member_type;

    static constexpr auto member = Member;
    internal::StructFieldDescriptor descriptor;
};

template <auto Member, class Range = internal::NoStructFieldRange>
consteval auto field(std::string_view name, Range range = {}, std::string_view unit = {}) {
    static_assert(std::is_member_object_pointer_v<decltype(Member)>,
                  "field requires a pointer to a data member");
    using MemberType = typename internal::MemberPointerTraits<decltype(Member)>::member_type;
    constexpr auto type = internal::structFieldType<MemberType>();
    if (name.empty()) {
        throw "struct field name must not be empty";
    }
    if (name.starts_with("$")) {
        throw "ReservedFieldName: struct field names beginning with '$' are reserved";
    }

    internal::StructFieldRangeDescriptor range_descriptor{};
    if constexpr (std::same_as<Range, internal::NoStructFieldRange>) {
    } else if constexpr (std::same_as<Range, internal::SignedStructFieldRange>) {
        if (type != StructFieldType::I8 && type != StructFieldType::I16 && type != StructFieldType::I32 &&
            type != StructFieldType::I64) {
            throw "signed integer range does not match struct field type";
        }
        if (range.min > range.max) {
            throw "struct field range minimum exceeds maximum";
        }
        range_descriptor = {.kind = internal::StructFieldRangeKind::Signed,
                            .signed_min = range.min,
                            .signed_max = range.max};
    } else if constexpr (std::same_as<Range, internal::UnsignedStructFieldRange>) {
        if (type != StructFieldType::U8 && type != StructFieldType::U16 && type != StructFieldType::U32 &&
            type != StructFieldType::U64) {
            throw "unsigned integer range does not match struct field type";
        }
        if (range.min > range.max) {
            throw "struct field range minimum exceeds maximum";
        }
        range_descriptor = {.kind = internal::StructFieldRangeKind::Unsigned,
                            .unsigned_min = range.min,
                            .unsigned_max = range.max};
    } else if constexpr (std::same_as<Range, internal::FloatingStructFieldRange>) {
        if (type != StructFieldType::F32 && type != StructFieldType::F64 && type != StructFieldType::Vec2 &&
            type != StructFieldType::Vec3 && type != StructFieldType::Vec4) {
            throw "floating-point range does not match struct field type";
        }
        internal::validateFloatingRange(range, type);
        range_descriptor = {.kind = internal::StructFieldRangeKind::Floating,
                            .floating_min = range.min,
                            .floating_max = range.max};
    } else {
        static_assert(internal::always_false_v<Range>, "unsupported struct field range descriptor");
    }

    return StructFieldDeclaration<Member>{
        .descriptor = {.name = name, .type = type, .range = range_descriptor, .unit = unit}};
}

template <class Enum> struct StructEnumValue {
    static_assert(std::is_enum_v<Enum>, "enumValue requires an enum type");
    std::string_view name;
    Enum value;
};

template <class Enum> consteval StructEnumValue<Enum> enumValue(std::string_view name, Enum value) {
    if (name.empty()) {
        throw "enum value name must not be empty";
    }
    return {name, value};
}

template <class Enum, std::size_t N> struct StructEnumValues {
    using enum_type = Enum;
    std::array<StructEnumValue<Enum>, N> values;
};

namespace internal {

template <class Enum, std::size_t N>
consteval void validateFlagsVocabularyNotOneHot(
    const std::array<StructEnumValue<Enum>, N> &values) {
    using Raw = std::underlying_type_t<Enum>;
    using Unsigned = std::make_unsigned_t<Raw>;
    for (const auto &entry : values) {
        const auto bits = static_cast<Unsigned>(entry.value);
        if (bits == 0 || (bits & (bits - 1)) != 0)
            throw "FlagsVocabularyNotOneHot: Flags values must be nonzero one-hot bits";
    }
}

} // namespace internal

template <class First, class... Rest> consteval auto enumValues(First first, Rest... rest) {
    static_assert((std::same_as<First, Rest> && ...), "enumValues entries must use one enum type");
    using Enum = decltype(first.value);
    static_assert(std::is_enum_v<Enum>, "enumValues requires values returned by enumValue");
    std::array<StructEnumValue<Enum>, 1 + sizeof...(Rest)> result{first, rest...};
    for (std::size_t i = 0; i < result.size(); ++i) {
        for (std::size_t j = i + 1; j < result.size(); ++j) {
            if (result[i].name == result[j].name || result[i].value == result[j].value) {
                throw "enum value names and values must be unique";
            }
        }
    }
    return StructEnumValues<Enum, result.size()>{.values = result};
}

struct NoStructEnumValues {};
struct EmptyListDefault {};

template <class Declaration, class EnumValues = NoStructEnumValues>
struct RequiredStructField {
    Declaration field;
    EnumValues enum_values;
};

template <class Declaration, class Default, class EnumValues = NoStructEnumValues>
struct DefaultedStructField {
    Declaration field;
    Default default_value;
    EnumValues enum_values;
};

template <class Declaration, class EnumValues = NoStructEnumValues>
struct OptionalOverrideStructField {
    Declaration field;
    EnumValues enum_values;
};

template <class Declaration> consteval auto required(Declaration declaration) {
    static_assert(requires { typename Declaration::owner_type; typename Declaration::member_type; },
                  "required accepts only values returned by field");
    return RequiredStructField<Declaration>{.field = declaration,
                                             .enum_values = {}};
}

template <class Declaration, class Enum, std::size_t N>
consteval auto required(Declaration declaration,
                        StructEnumValues<Enum, N> values) {
    static_assert(requires { typename Declaration::owner_type; typename Declaration::member_type; },
                  "required accepts only values returned by field");
    using Member = typename Declaration::member_type;
    if constexpr (internal::is_flags_v<Member>) {
        static_assert(
            std::same_as<typename internal::FlagsTraits<Member>::enum_type,
                         Enum>,
            "Flags vocabulary enum must match Flags<Enum>");
        internal::validateFlagsVocabularyNotOneHot(values.values);
    } else {
        static_assert(std::same_as<Member, Enum>,
                      "enum vocabulary must match the field enum type");
    }
    return RequiredStructField<Declaration, StructEnumValues<Enum, N>>{
        .field = declaration, .enum_values = values};
}

template <class Declaration>
consteval auto optionalOverride(Declaration declaration) {
    static_assert(requires { typename Declaration::owner_type; typename Declaration::member_type; },
                  "optionalOverride accepts only values returned by field");
    using Member = typename Declaration::member_type;
    static_assert(internal::is_optional_v<Member>,
                  "OptionalOverride fields must use std::optional<T>");
    using Value = internal::unwrap_optional_t<Member>;
    static_assert(!internal::is_flags_v<Value>,
                  "Flags OptionalOverride fields require enumValues vocabulary");
    static_assert(!internal::is_list_v<Value>,
                  "List fields cannot be OptionalOverride");
    return OptionalOverrideStructField<Declaration>{.field = declaration,
                                                     .enum_values = {}};
}

template <class Declaration, class Enum, std::size_t N>
consteval auto optionalOverride(Declaration declaration,
                                StructEnumValues<Enum, N> values) {
    static_assert(requires { typename Declaration::owner_type; typename Declaration::member_type; },
                  "optionalOverride accepts only values returned by field");
    using Member = typename Declaration::member_type;
    static_assert(internal::is_optional_v<Member>,
                  "OptionalOverride fields must use std::optional<T>");
    using Value = internal::unwrap_optional_t<Member>;
    static_assert(internal::is_flags_v<Value> || std::is_enum_v<Value>,
                  "OptionalOverride vocabulary applies to Enum or Flags<Enum>");
    if constexpr (internal::is_flags_v<Value>) {
        static_assert(std::same_as<
                          typename internal::FlagsTraits<Value>::enum_type,
                          Enum>,
                      "Flags vocabulary enum must match Flags<Enum>");
        internal::validateFlagsVocabularyNotOneHot(values.values);
    } else {
        static_assert(std::same_as<Value, Enum>,
                      "enum vocabulary must match the optional enum type");
    }
    return OptionalOverrideStructField<Declaration, StructEnumValues<Enum, N>>{
        .field = declaration, .enum_values = values};
}

template <class Declaration, class Default> consteval auto defaulted(Declaration declaration, Default &&value) {
    static_assert(requires { typename Declaration::owner_type; typename Declaration::member_type; },
                  "defaulted accepts only values returned by field");
    using Member = typename Declaration::member_type;
    if constexpr (std::same_as<Member, std::string>) {
        static_assert(std::constructible_from<std::string_view, Default>,
                      "string field defaults must be string literals or string_view");
        return DefaultedStructField<Declaration, std::string_view>{
            .field = declaration, .default_value = std::string_view{std::forward<Default>(value)}, .enum_values = {}};
    } else {
        static_assert(std::convertible_to<Default, Member>, "field default must be convertible to the member type");
        return DefaultedStructField<Declaration, Member>{
            .field = declaration, .default_value = static_cast<Member>(std::forward<Default>(value)), .enum_values = {}};
    }
}

template <class Declaration, class Default, class Enum, std::size_t N>
consteval auto defaulted(Declaration declaration, Default &&value, StructEnumValues<Enum, N> values) {
    static_assert(requires { typename Declaration::owner_type; typename Declaration::member_type; },
                  "defaulted accepts only values returned by field");
    using Member = typename Declaration::member_type;
    if constexpr (internal::is_flags_v<Member>) {
        static_assert(
            std::same_as<typename internal::FlagsTraits<Member>::enum_type, Enum>,
            "Flags defaults and enumValues must match the field enum type");
        internal::validateFlagsVocabularyNotOneHot(values.values);
    } else {
        static_assert(std::same_as<Member, Enum>,
                      "enum defaults and enumValues must match the field enum type");
    }
    const auto default_value = static_cast<Member>(std::forward<Default>(value));
    if constexpr (std::same_as<Member, Enum>) {
        bool found = false;
        for (const auto &entry : values.values) {
            found = found || entry.value == default_value;
        }
        if (!found) {
            throw "enum default must be present in enumValues";
        }
    }
    return DefaultedStructField<Declaration, Member, StructEnumValues<Enum, N>>{
        .field = declaration, .default_value = default_value, .enum_values = values};
}

// A List has a fixed empty default, but its runtime representation owns a
// std::vector. Keep that variable-length value out of the constexpr schema.
template <class Declaration> consteval auto defaulted(Declaration declaration) {
    static_assert(requires { typename Declaration::owner_type; typename Declaration::member_type; },
                  "defaulted accepts only values returned by field");
    using Member = typename Declaration::member_type;
    static_assert(internal::is_list_v<Member>,
                  "the one-argument defaulted form is only for List fields");
    return DefaultedStructField<Declaration, EmptyListDefault>{
        .field = declaration, .default_value = {}, .enum_values = {}};
}

namespace internal {

template <class> struct PolicyFieldTraits;

template <class Declaration, class EnumValues>
struct PolicyFieldTraits<RequiredStructField<Declaration, EnumValues>> {
    using declaration_type = Declaration;
    using owner_type = typename Declaration::owner_type;
    using member_type = typename Declaration::member_type;
    static constexpr StructFieldPresence presence = StructFieldPresence::Required;
    static constexpr bool has_enum_values =
        !std::same_as<EnumValues, NoStructEnumValues>;
};

template <class Declaration, class Default, class EnumValues>
struct PolicyFieldTraits<DefaultedStructField<Declaration, Default, EnumValues>> {
    using declaration_type = Declaration;
    using owner_type = typename Declaration::owner_type;
    using member_type = typename Declaration::member_type;
    static constexpr StructFieldPresence presence = StructFieldPresence::Defaulted;
    static constexpr bool has_enum_values = !std::same_as<EnumValues, NoStructEnumValues>;
};

template <class Declaration, class EnumValues>
struct PolicyFieldTraits<OptionalOverrideStructField<Declaration, EnumValues>> {
    using declaration_type = Declaration;
    using owner_type = typename Declaration::owner_type;
    using member_type = typename Declaration::member_type;
    static constexpr StructFieldPresence presence =
        StructFieldPresence::OptionalOverride;
    static constexpr bool has_enum_values =
        !std::same_as<EnumValues, NoStructEnumValues>;
};

template <class T> concept PolicyField = requires {
    typename PolicyFieldTraits<T>::declaration_type;
    typename PolicyFieldTraits<T>::owner_type;
    typename PolicyFieldTraits<T>::member_type;
};

template <class T>
concept StructUseSitePolicy = std::same_as<T, EventPayloadPolicy> || std::same_as<T, BehaviorParamsPolicy> ||
                              std::same_as<T, ComponentPolicy>;

template <class First, class... Rest>
inline constexpr bool same_field_owner_v =
    (std::same_as<typename PolicyFieldTraits<First>::owner_type,
                  typename PolicyFieldTraits<Rest>::owner_type> &&
     ...);

template <class PolicyField>
inline constexpr bool event_policy_field_v =
    PolicyFieldTraits<PolicyField>::presence == StructFieldPresence::Required &&
    is_event_field_type_v<typename PolicyFieldTraits<PolicyField>::member_type>;

template <class PolicyField>
inline constexpr bool behavior_policy_field_v =
    PolicyFieldTraits<PolicyField>::presence == StructFieldPresence::Defaulted &&
    (!std::is_enum_v<typename PolicyFieldTraits<PolicyField>::member_type> ||
     PolicyFieldTraits<PolicyField>::has_enum_values);

template <class Schema>
consteval bool schemaContainsList(const Schema &schema) {
    for (const auto &field : schema.fields) {
        if (field.type == StructFieldType::List) return true;
    }
    return false;
}

template <class PolicyField>
consteval bool validRecursivePolicyField() {
    using Traits = PolicyFieldTraits<PolicyField>;
    using Member = unwrap_optional_t<typename Traits::member_type>;
    if constexpr (is_flags_v<Member>) {
        return Traits::has_enum_values;
    } else if constexpr (std::is_enum_v<Member>) {
        return Traits::has_enum_values;
    } else if constexpr (is_list_v<Member>) {
        using Row = typename ListTraits<Member>::row_type;
        if constexpr (!requires { Row::schema; }) {
            return false;
        } else {
            return !schemaContainsList(Row::schema);
        }
    }
    return true;
}

template <class Schema>
consteval bool schemaRequiresV3(const Schema &schema) {
    for (const auto &field : schema.fields) {
        if (field.type == StructFieldType::Flags ||
            field.type == StructFieldType::List) {
            return true;
        }
    }
    return false;
}

} // namespace internal

template <class Policy, class... PolicyFields> struct StructSchemaDescriptor {
    using policy_type = Policy;

    Policy policy;
    std::tuple<PolicyFields...> declarations;
    std::array<internal::StructFieldDescriptor, sizeof...(PolicyFields)> fields;
    std::array<StructFieldPresence, sizeof...(PolicyFields)> presence;
};

template <class Policy, class... PolicyFields>
consteval auto structFields(Policy policy, PolicyFields... policy_fields) {
    if constexpr (!internal::StructUseSitePolicy<Policy>) {
        static_assert(internal::always_false_v<Policy>,
                      "structFields requires an explicit EventPayloadPolicy, BehaviorParamsPolicy, or ComponentPolicy");
    } else if constexpr (!(internal::PolicyField<PolicyFields> && ...)) {
        static_assert((internal::PolicyField<PolicyFields> && ...),
                      "every struct field requires an explicit required(...) or defaulted(...) policy");
    } else {
        if constexpr (sizeof...(PolicyFields) > 1) {
            static_assert(internal::same_field_owner_v<PolicyFields...>,
                          "all fields in a struct schema must belong to one owner type");
        }
        static_assert((internal::validRecursivePolicyField<PolicyFields>() && ...),
                      "ListNestingUnsupported or Flags vocabulary is missing");
        if constexpr (std::same_as<Policy, EventPayloadPolicy>) {
            static_assert((internal::event_policy_field_v<PolicyFields> && ...),
                          "EventPayload fields are all required and cannot use Bool or Enum");
        } else if constexpr (std::same_as<Policy, BehaviorParamsPolicy>) {
            static_assert((internal::behavior_policy_field_v<PolicyFields> && ...),
                          "BehaviorParams fields require explicit defaults and Enum fields require enumValues");
        }

        std::array<internal::StructFieldDescriptor, sizeof...(PolicyFields)> field_descriptors{
            policy_fields.field.descriptor...};
        const auto copy_enum_values = [&]<class PolicyField>(
                                          std::size_t index,
                                          const PolicyField &policy_field) {
            if constexpr (internal::PolicyFieldTraits<PolicyField>::has_enum_values) {
                static_assert(
                    std::tuple_size_v<decltype(policy_field.enum_values.values)> <=
                        maxStructFieldEnumValues,
                    "enumValues exceeds maxStructFieldEnumValues");
                auto &target = field_descriptors[index];
                for (const auto &entry : policy_field.enum_values.values) {
                    target.enum_values[target.enum_value_count++] = entry.name;
                }
            }
        };
        std::size_t enum_index = 0;
        (copy_enum_values(enum_index++, policy_fields), ...);
        for (std::size_t i = 0; i < field_descriptors.size(); ++i) {
            for (std::size_t j = i + 1; j < field_descriptors.size(); ++j) {
                if (field_descriptors[i].name == field_descriptors[j].name) {
                    throw "struct field names must be unique";
                }
            }
        }
        return StructSchemaDescriptor<Policy, PolicyFields...>{
            .policy = policy,
            .declarations = std::tuple<PolicyFields...>{policy_fields...},
            .fields = field_descriptors,
            .presence = {internal::PolicyFieldTraits<PolicyFields>::presence...},
        };
    }
}

} // namespace Pelican
