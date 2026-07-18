#pragma once

#include "../schema/structfieldschema.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Pelican {

// WP71 names stay source-compatible. Their representation now comes from the
// use-site-neutral StructFieldSchema layer.
using PayloadFieldType = StructFieldType;
using PayloadRange = StructFieldRange;
using PayloadFieldSchema = StructFieldSchema;

struct EventPayloadSchema {
    std::span<const PayloadFieldSchema> fields;
    bool unknown_fields_reject = true;
};

namespace internal {

struct RefFieldRecorder {
    std::vector<std::string> names;

    template <class T> void prop(const char *field_name, T &) {
        names.emplace_back(field_name);
    }
};

} // namespace internal

template <std::size_t N> struct EventPayloadDescriptor {
    using policy_type = EventPayloadPolicy;

    EventPayloadPolicy policy;
    std::array<internal::StructFieldDescriptor, N> fields;
    std::array<StructFieldPresence, N> presence;
};

// Compatibility surface for existing EventPayload declarations. The function
// name is the explicit EventPayload use site; it cannot select any other policy
// and it maps every field to Required. Event descriptors intentionally retain
// only the POD form so a 15-field static descriptor stays below MSVC's nested
// initializer limit (WP71's original materialization constraint).
template <class... Fields> consteval auto payloadFields(Fields... fields) {
    static_assert((requires { typename Fields::owner_type; typename Fields::member_type; } && ...),
                  "payloadFields accepts only values returned by Pelican::field");
    static_assert((internal::is_event_field_type_v<typename Fields::member_type> && ...),
                  "EventPayload fields cannot use Bool or Enum");
    std::array<internal::StructFieldDescriptor, sizeof...(Fields)> descriptors{fields.descriptor...};
    for (std::size_t i = 0; i < descriptors.size(); ++i) {
        for (std::size_t j = i + 1; j < descriptors.size(); ++j) {
            if (descriptors[i].name == descriptors[j].name) {
                throw "event payload field names must be unique";
            }
        }
    }
    std::array<StructFieldPresence, sizeof...(Fields)> presence{};
    for (auto &entry : presence) {
        entry = StructFieldPresence::Required;
    }
    return EventPayloadDescriptor<sizeof...(Fields)>{
        .policy = eventPayloadPolicy, .fields = descriptors, .presence = presence};
}

namespace internal {

template <class Event, std::size_t... I>
constexpr auto materializeEventFields(std::index_sequence<I...>) {
    return std::array<PayloadFieldSchema, sizeof...(I)>{
        materializeStructField(Event::pelican_payload.fields[I])...};
}

template <class Event> EventPayloadSchema eventPayloadSchema() {
    static_assert(std::same_as<typename decltype(Event::pelican_payload)::policy_type, EventPayloadPolicy>,
                  "pelican_payload must use EventPayloadPolicy");
    constexpr std::size_t field_count = Event::pelican_payload.fields.size();
    static constexpr auto fields = materializeEventFields<Event>(std::make_index_sequence<field_count>{});
    return EventPayloadSchema{.fields = std::span<const PayloadFieldSchema>{fields},
                              .unknown_fields_reject = true};
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
