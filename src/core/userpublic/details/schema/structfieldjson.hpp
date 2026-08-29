#pragma once

#include "structfieldschema.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
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
        if constexpr (std::same_as<Member, std::string>) {
            temporary.*Declaration::member = std::string{policy_field.default_value};
        } else {
            temporary.*Declaration::member = policy_field.default_value;
        }
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
    if constexpr (std::same_as<Member, bool>) {
        if (!value_it->is_boolean()) {
            structFieldError(StructFieldErrorCode::TypeMismatch, path, "must be a boolean");
        }
        member = value_it->get<bool>();
    } else if constexpr (std::is_enum_v<Member>) {
        if (!value_it->is_string()) {
            structFieldError(StructFieldErrorCode::TypeMismatch, path, "must be an enum string");
        }
        const auto &name = value_it->template get_ref<const std::string &>();
        for (const auto &entry : policy_field.enum_values.values) {
            if (entry.name == name) {
                member = entry.value;
                return;
            }
        }
        structFieldError(StructFieldErrorCode::InvalidEnum, path, "is not a declared enum value");
    } else if constexpr (std::is_signed_v<Member> && std::is_integral_v<Member>) {
        member = static_cast<Member>(validateStructSigned(*value_it, schema, path));
    } else if constexpr (std::is_unsigned_v<Member> && std::is_integral_v<Member>) {
        member = static_cast<Member>(validateStructUnsigned(*value_it, schema, path));
    } else if constexpr (std::same_as<Member, float> || std::same_as<Member, double>) {
        member = static_cast<Member>(validateStructFloating(*value_it, schema, path,
                                                            std::same_as<Member, float>));
    } else if constexpr (std::same_as<Member, std::string>) {
        if (!value_it->is_string()) {
            structFieldError(StructFieldErrorCode::TypeMismatch, path, "must be a string");
        }
        member = value_it->template get_ref<const std::string &>();
    } else if constexpr (std::same_as<Member, vec2> || std::same_as<Member, vec3> ||
                         std::same_as<Member, vec4> || std::same_as<Member, quat>) {
        if (!value_it->is_array() || value_it->size() != structVectorLength(schema.type)) {
            structFieldError(StructFieldErrorCode::TypeMismatch, path, "has an invalid array shape");
        }
        std::array<float, 4> components{};
        for (std::size_t i = 0; i < value_it->size(); ++i) {
            components[i] = static_cast<float>(validateStructFloating((*value_it)[i], schema, path, true));
        }
        if constexpr (std::same_as<Member, vec2>) {
            member = {components[0], components[1]};
        } else if constexpr (std::same_as<Member, vec3>) {
            member = {components[0], components[1], components[2]};
        } else {
            member = {components[0], components[1], components[2], components[3]};
        }
    } else {
        static_assert(always_false_v<Member>, "unsupported struct JSON field type");
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
    if constexpr (std::same_as<Member, bool>) {
        return member;
    } else if constexpr (std::is_enum_v<Member>) {
        for (const auto &entry : policy_field.enum_values.values) {
            if (entry.value == member) {
                return entry.name;
            }
        }
        structFieldError(StructFieldErrorCode::InvalidEnum, path, "is not a declared enum value");
    } else if constexpr (std::is_signed_v<Member> && std::is_integral_v<Member>) {
        const StructJson encoded = member;
        (void)validateStructSigned(encoded, schema, path);
        return member;
    } else if constexpr (std::is_unsigned_v<Member> && std::is_integral_v<Member>) {
        const StructJson encoded = member;
        (void)validateStructUnsigned(encoded, schema, path);
        return member;
    } else if constexpr (std::same_as<Member, float> || std::same_as<Member, double>) {
        const StructJson encoded = member;
        (void)validateStructFloating(encoded, schema, path, std::same_as<Member, float>);
        return member;
    } else if constexpr (std::same_as<Member, vec2>) {
        for (const float component : {member.x, member.y}) {
            (void)validateStructFloating(StructJson(component), schema, path, true);
        }
        return nlohmann::ordered_json::array({member.x, member.y});
    } else if constexpr (std::same_as<Member, vec3>) {
        for (const float component : {member.x, member.y, member.z}) {
            (void)validateStructFloating(StructJson(component), schema, path, true);
        }
        return nlohmann::ordered_json::array({member.x, member.y, member.z});
    } else if constexpr (std::same_as<Member, vec4> || std::same_as<Member, quat>) {
        for (const float component : {member.x, member.y, member.z, member.w}) {
            (void)validateStructFloating(StructJson(component), schema, path, true);
        }
        return nlohmann::ordered_json::array({member.x, member.y, member.z, member.w});
    } else {
        return member;
    }
}

} // namespace internal

template <class Params, class... PolicyFields>
void decodeBehaviorParams(const nlohmann::json &payload, Params &destination,
                          const StructSchemaDescriptor<BehaviorParamsPolicy, PolicyFields...> &schema,
                          std::string_view root_path = "params") {
    static_assert(std::default_initializable<Params>, "BehaviorParams must be default constructible");
    static_assert(std::is_nothrow_swappable_v<Params>,
                  "BehaviorParams atomic apply requires a noexcept swap");

    if (!payload.is_object()) {
        internal::structFieldError(StructFieldErrorCode::PayloadMustBeObject, std::string{root_path},
                                   "must be an object");
    }
    for (auto it = payload.begin(); it != payload.end(); ++it) {
        bool known = false;
        for (const auto &field_schema : schema.fields) {
            known = known || field_schema.name == it.key();
        }
        if (!known) {
            const auto path = internal::childFieldPath(root_path, it.key());
            internal::structFieldError(StructFieldErrorCode::UnknownField, path, "is unknown");
        }
    }

    Params temporary{};
    std::apply([&](const auto &...policy_field) {
        (internal::setStructDefault(policy_field, temporary), ...);
        (internal::decodeStructPolicyField(payload, policy_field, temporary, root_path), ...);
        ((void)internal::encodeStructPolicyField(temporary, policy_field, root_path), ...);
    }, schema.declarations);

    using std::swap;
    swap(destination, temporary);
}

template <class Params, class... PolicyFields>
nlohmann::ordered_json
encodeBehaviorParams(const Params &value,
                     const StructSchemaDescriptor<BehaviorParamsPolicy, PolicyFields...> &schema,
                     std::string_view root_path = "params") {
    nlohmann::ordered_json result = nlohmann::ordered_json::object();
    std::apply([&](const auto &...policy_field) {
        ((result[std::string{policy_field.field.descriptor.name}] =
              internal::encodeStructPolicyField(value, policy_field, root_path)),
         ...);
    }, schema.declarations);
    return result;
}

} // namespace Pelican
