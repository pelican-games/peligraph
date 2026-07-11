#pragma once

#include "../../geom/quat.hpp"
#include "../../geom/vec.hpp"

#include <array>
#include <concepts>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Pelican {

enum class PayloadFieldType : std::uint8_t {
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
};

using PayloadRange = std::variant<std::monostate, std::pair<std::int64_t, std::int64_t>,
                                  std::pair<std::uint64_t, std::uint64_t>, std::pair<double, double>>;

struct PayloadFieldSchema {
    std::string_view name;
    PayloadFieldType type;
    PayloadRange range;
    std::string_view unit;

    constexpr PayloadFieldSchema(std::string_view field_name, PayloadFieldType field_type, PayloadRange field_range,
                                 std::string_view field_unit = {})
        : name(field_name), type(field_type), range(std::move(field_range)), unit(field_unit) {}
};

struct EventPayloadSchema {
    std::span<const PayloadFieldSchema> fields;
    bool unknown_fields_reject = true;
};

namespace internal {

template <class> inline constexpr bool always_false_v = false;

struct RefFieldRecorder {
    std::vector<std::string> names;

    template <class T> void prop(const char *field_name, T &) {
        names.emplace_back(field_name);
    }
};

template <class> struct MemberPointerTraits;
template <class Owner, class Member> struct MemberPointerTraits<Member Owner::*> {
    using member_type = std::remove_cv_t<Member>;
};

template <class T> consteval PayloadFieldType payloadFieldType() {
    if constexpr (std::same_as<T, std::int8_t>) {
        return PayloadFieldType::I8;
    } else if constexpr (std::same_as<T, std::int16_t>) {
        return PayloadFieldType::I16;
    } else if constexpr (std::same_as<T, std::int32_t>) {
        return PayloadFieldType::I32;
    } else if constexpr (std::same_as<T, std::int64_t>) {
        return PayloadFieldType::I64;
    } else if constexpr (std::same_as<T, std::uint8_t>) {
        return PayloadFieldType::U8;
    } else if constexpr (std::same_as<T, std::uint16_t>) {
        return PayloadFieldType::U16;
    } else if constexpr (std::same_as<T, std::uint32_t>) {
        return PayloadFieldType::U32;
    } else if constexpr (std::same_as<T, std::uint64_t>) {
        return PayloadFieldType::U64;
    } else if constexpr (std::same_as<T, float>) {
        return PayloadFieldType::F32;
    } else if constexpr (std::same_as<T, double>) {
        return PayloadFieldType::F64;
    } else if constexpr (std::same_as<T, vec2>) {
        return PayloadFieldType::Vec2;
    } else if constexpr (std::same_as<T, vec3>) {
        return PayloadFieldType::Vec3;
    } else if constexpr (std::same_as<T, vec4>) {
        return PayloadFieldType::Vec4;
    } else if constexpr (std::same_as<T, quat>) {
        return PayloadFieldType::Quat;
    } else if constexpr (std::same_as<T, std::string>) {
        return PayloadFieldType::String;
    } else {
        static_assert(always_false_v<T>, "event payload field type is not supported by JsonArchiveLoader");
    }
}

struct NoPayloadRange {};
struct SignedPayloadRange {
    std::int64_t min;
    std::int64_t max;
};
struct UnsignedPayloadRange {
    std::uint64_t min;
    std::uint64_t max;
};
struct FloatingPayloadRange {
    double min;
    double max;
};

enum class PayloadRangeKind : std::uint8_t { None, Signed, Unsigned, Floating };

struct PayloadRangeDescriptor {
    PayloadRangeKind kind = PayloadRangeKind::None;
    std::int64_t signed_min = 0;
    std::int64_t signed_max = 0;
    std::uint64_t unsigned_min = 0;
    std::uint64_t unsigned_max = 0;
    double floating_min = 0;
    double floating_max = 0;
};

struct PayloadFieldDescriptor {
    std::string_view name;
    PayloadFieldType type;
    PayloadRangeDescriptor range;
    std::string_view unit;
};

consteval void validateFloatingRange(FloatingPayloadRange range, PayloadFieldType type) {
    constexpr double max_f64 = std::numeric_limits<double>::max();
    const auto finite = [](double value) { return value == value && value >= -max_f64 && value <= max_f64; };
    if (!finite(range.min) || !finite(range.max)) {
        throw "event payload range bounds must be finite";
    }
    if (range.min > range.max) {
        throw "event payload range minimum exceeds maximum";
    }
    if (type == PayloadFieldType::F32 || type == PayloadFieldType::Vec2 || type == PayloadFieldType::Vec3 ||
        type == PayloadFieldType::Vec4) {
        constexpr double max_f32 = static_cast<double>(std::numeric_limits<float>::max());
        if (range.min < -max_f32 || range.min > max_f32 || range.max < -max_f32 || range.max > max_f32) {
            throw "event payload F32 range bound is not representable as F32";
        }
    }
}

} // namespace internal

constexpr internal::SignedPayloadRange irange(std::int64_t min, std::int64_t max) {
    return internal::SignedPayloadRange{min, max};
}

constexpr internal::UnsignedPayloadRange urange(std::uint64_t min, std::uint64_t max) {
    return internal::UnsignedPayloadRange{min, max};
}

constexpr internal::FloatingPayloadRange frange(double min, double max) {
    return internal::FloatingPayloadRange{min, max};
}

template <auto Member, class Range = internal::NoPayloadRange>
consteval internal::PayloadFieldDescriptor field(std::string_view name, Range range = {}, std::string_view unit = {}) {
    static_assert(std::is_member_object_pointer_v<decltype(Member)>, "field requires a pointer to a data member");
    using MemberType = typename internal::MemberPointerTraits<decltype(Member)>::member_type;
    constexpr auto type = internal::payloadFieldType<MemberType>();
    if (name.empty()) {
        throw "event payload field name must not be empty";
    }
    if constexpr (std::same_as<Range, internal::NoPayloadRange>) {
        return {name, type, {}, unit};
    } else if constexpr (std::same_as<Range, internal::SignedPayloadRange>) {
        if (type != PayloadFieldType::I8 && type != PayloadFieldType::I16 && type != PayloadFieldType::I32 &&
            type != PayloadFieldType::I64) {
            throw "signed integer range does not match payload field type";
        }
        if (range.min > range.max) {
            throw "event payload range minimum exceeds maximum";
        }
        return {name,
                type,
                {.kind = internal::PayloadRangeKind::Signed,
                 .signed_min = range.min,
                 .signed_max = range.max},
                unit};
    } else if constexpr (std::same_as<Range, internal::UnsignedPayloadRange>) {
        if (type != PayloadFieldType::U8 && type != PayloadFieldType::U16 && type != PayloadFieldType::U32 &&
            type != PayloadFieldType::U64) {
            throw "unsigned integer range does not match payload field type";
        }
        if (range.min > range.max) {
            throw "event payload range minimum exceeds maximum";
        }
        return {name,
                type,
                {.kind = internal::PayloadRangeKind::Unsigned,
                 .unsigned_min = range.min,
                 .unsigned_max = range.max},
                unit};
    } else if constexpr (std::same_as<Range, internal::FloatingPayloadRange>) {
        if (type != PayloadFieldType::F32 && type != PayloadFieldType::F64 && type != PayloadFieldType::Vec2 &&
            type != PayloadFieldType::Vec3 && type != PayloadFieldType::Vec4) {
            throw "floating-point range does not match payload field type";
        }
        internal::validateFloatingRange(range, type);
        return {name,
                type,
                {.kind = internal::PayloadRangeKind::Floating,
                 .floating_min = range.min,
                 .floating_max = range.max},
                unit};
    } else {
        static_assert(internal::always_false_v<Range>, "unsupported event payload range descriptor");
    }
}

template <std::size_t N> struct EventPayloadDescriptor {
    std::array<internal::PayloadFieldDescriptor, N> fields;
};

template <class... Fields>
consteval auto payloadFields(Fields... fields) {
    static_assert((std::same_as<Fields, internal::PayloadFieldDescriptor> && ...),
                  "payloadFields accepts only values returned by Pelican::field");
    std::array<internal::PayloadFieldDescriptor, sizeof...(Fields)> result{fields...};
    for (std::size_t i = 0; i < result.size(); ++i) {
        for (std::size_t j = i + 1; j < result.size(); ++j) {
            if (result[i].name == result[j].name) {
                throw "event payload field names must be unique";
            }
        }
    }
    return EventPayloadDescriptor<sizeof...(Fields)>{.fields = result};
}

namespace internal {

constexpr PayloadRange materializeRange(const PayloadRangeDescriptor &range) {
    switch (range.kind) {
    case PayloadRangeKind::None:
        return PayloadRange{std::monostate{}};
    case PayloadRangeKind::Signed:
        return PayloadRange{std::pair<std::int64_t, std::int64_t>{range.signed_min, range.signed_max}};
    case PayloadRangeKind::Unsigned:
        return PayloadRange{std::pair<std::uint64_t, std::uint64_t>{range.unsigned_min, range.unsigned_max}};
    case PayloadRangeKind::Floating:
        return PayloadRange{std::pair<double, double>{range.floating_min, range.floating_max}};
    }
    return PayloadRange{std::monostate{}};
}

constexpr PayloadFieldSchema materializeField(const PayloadFieldDescriptor &field) {
    return PayloadFieldSchema{field.name, field.type, materializeRange(field.range), field.unit};
}

template <class Event, std::size_t... I>
constexpr auto materializeFields(std::index_sequence<I...>) {
    return std::array<PayloadFieldSchema, sizeof...(I)>{materializeField(Event::pelican_payload.fields[I])...};
}

template <class Event> EventPayloadSchema eventPayloadSchema() {
    constexpr std::size_t field_count = Event::pelican_payload.fields.size();
    static constexpr auto fields = materializeFields<Event>(std::make_index_sequence<field_count>{});
    return EventPayloadSchema{.fields = std::span<const PayloadFieldSchema>{fields}, .unknown_fields_reject = true};
}

} // namespace internal

struct EventSchemaLookup {
    enum class State : std::uint8_t { UnknownEvent, Payloadless, Opaque, Typed };

    State state = State::UnknownEvent;
    const EventPayloadSchema *schema = nullptr;
};

using EventSchemaState = EventSchemaLookup::State;

enum class EventPayloadErrorCode : std::uint8_t {
    UnknownEvent,
    NoPayloadEvent,
    PayloadRequired,
    PayloadMustBeObject,
    UnknownField,
    MissingField,
    TypeMismatch,
    OutOfRange,
};

class EventPayloadValidationError : public std::runtime_error {
    EventPayloadErrorCode error_code;

  public:
    EventPayloadValidationError(EventPayloadErrorCode code, std::string message)
        : std::runtime_error(std::move(message)), error_code(code) {}

    EventPayloadErrorCode code() const noexcept {
        return error_code;
    }
};

} // namespace Pelican
