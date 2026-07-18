#pragma once

#include "../../geom/quat.hpp"
#include "../../geom/vec.hpp"

#include <array>
#include <concepts>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

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
};

using StructFieldRange = std::variant<std::monostate, std::pair<std::int64_t, std::int64_t>,
                                      std::pair<std::uint64_t, std::uint64_t>, std::pair<double, double>>;

struct StructFieldSchema {
    std::string_view name;
    StructFieldType type;
    StructFieldRange range;
    std::string_view unit;

    constexpr StructFieldSchema(std::string_view field_name, StructFieldType field_type,
                                StructFieldRange field_range, std::string_view field_unit = {})
        : name(field_name), type(field_type), range(std::move(field_range)), unit(field_unit) {}
};

enum class StructFieldPresence : std::uint8_t { Required, Defaulted };

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

enum class StructFieldRangeKind : std::uint8_t { None, Signed, Unsigned, Floating };

struct StructFieldRangeDescriptor {
    StructFieldRangeKind kind = StructFieldRangeKind::None;
    std::int64_t signed_min = 0;
    std::int64_t signed_max = 0;
    std::uint64_t unsigned_min = 0;
    std::uint64_t unsigned_max = 0;
    double floating_min = 0;
    double floating_max = 0;
};

// POD form used while the owner type is still incomplete. This preserves the
// WP71/MSVC invariant that no variant is constructed in the class definition.
struct StructFieldDescriptor {
    std::string_view name;
    StructFieldType type;
    StructFieldRangeDescriptor range;
    std::string_view unit;
};

template <class T> consteval StructFieldType structFieldType() {
    if constexpr (std::same_as<T, std::int8_t>) {
        return StructFieldType::I8;
    } else if constexpr (std::same_as<T, std::int16_t>) {
        return StructFieldType::I16;
    } else if constexpr (std::same_as<T, std::int32_t>) {
        return StructFieldType::I32;
    } else if constexpr (std::same_as<T, std::int64_t>) {
        return StructFieldType::I64;
    } else if constexpr (std::same_as<T, std::uint8_t>) {
        return StructFieldType::U8;
    } else if constexpr (std::same_as<T, std::uint16_t>) {
        return StructFieldType::U16;
    } else if constexpr (std::same_as<T, std::uint32_t>) {
        return StructFieldType::U32;
    } else if constexpr (std::same_as<T, std::uint64_t>) {
        return StructFieldType::U64;
    } else if constexpr (std::same_as<T, float>) {
        return StructFieldType::F32;
    } else if constexpr (std::same_as<T, double>) {
        return StructFieldType::F64;
    } else if constexpr (std::same_as<T, vec2>) {
        return StructFieldType::Vec2;
    } else if constexpr (std::same_as<T, vec3>) {
        return StructFieldType::Vec3;
    } else if constexpr (std::same_as<T, vec4>) {
        return StructFieldType::Vec4;
    } else if constexpr (std::same_as<T, quat>) {
        return StructFieldType::Quat;
    } else if constexpr (std::same_as<T, std::string>) {
        return StructFieldType::String;
    } else if constexpr (std::same_as<T, bool>) {
        return StructFieldType::Bool;
    } else if constexpr (std::is_enum_v<T>) {
        return StructFieldType::Enum;
    } else {
        static_assert(always_false_v<T>, "struct field type is not supported");
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
    return StructFieldSchema{field.name, field.type, materializeStructFieldRange(field.range), field.unit};
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

template <class Declaration> struct RequiredStructField {
    Declaration field;
};

template <class Declaration, class Default, class EnumValues = NoStructEnumValues>
struct DefaultedStructField {
    Declaration field;
    Default default_value;
    EnumValues enum_values;
};

template <class Declaration> consteval auto required(Declaration declaration) {
    static_assert(requires { typename Declaration::owner_type; typename Declaration::member_type; },
                  "required accepts only values returned by field");
    return RequiredStructField<Declaration>{.field = declaration};
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
    static_assert(std::same_as<Member, Enum>, "enum defaults and enumValues must match the field enum type");
    const auto default_value = static_cast<Member>(std::forward<Default>(value));
    bool found = false;
    for (const auto &entry : values.values) {
        found = found || entry.value == default_value;
    }
    if (!found) {
        throw "enum default must be present in enumValues";
    }
    return DefaultedStructField<Declaration, Member, StructEnumValues<Enum, N>>{
        .field = declaration, .default_value = default_value, .enum_values = values};
}

namespace internal {

template <class> struct PolicyFieldTraits;

template <class Declaration> struct PolicyFieldTraits<RequiredStructField<Declaration>> {
    using declaration_type = Declaration;
    using owner_type = typename Declaration::owner_type;
    using member_type = typename Declaration::member_type;
    static constexpr StructFieldPresence presence = StructFieldPresence::Required;
    static constexpr bool has_enum_values = false;
};

template <class Declaration, class Default, class EnumValues>
struct PolicyFieldTraits<DefaultedStructField<Declaration, Default, EnumValues>> {
    using declaration_type = Declaration;
    using owner_type = typename Declaration::owner_type;
    using member_type = typename Declaration::member_type;
    static constexpr StructFieldPresence presence = StructFieldPresence::Defaulted;
    static constexpr bool has_enum_values = !std::same_as<EnumValues, NoStructEnumValues>;
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
        if constexpr (std::same_as<Policy, EventPayloadPolicy>) {
            static_assert((internal::event_policy_field_v<PolicyFields> && ...),
                          "EventPayload fields are all required and cannot use Bool or Enum");
        } else if constexpr (std::same_as<Policy, BehaviorParamsPolicy>) {
            static_assert((internal::behavior_policy_field_v<PolicyFields> && ...),
                          "BehaviorParams fields require explicit defaults and Enum fields require enumValues");
        }

        std::array<internal::StructFieldDescriptor, sizeof...(PolicyFields)> field_descriptors{
            policy_fields.field.descriptor...};
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
