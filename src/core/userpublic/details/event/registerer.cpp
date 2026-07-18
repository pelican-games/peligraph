#include "registerer.hpp"
#include "../behavior/registerer.hpp"
#include "../system/registerer.hpp"

#include <exception>
#include "../system/registerer.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <ranges>
#include <utility>

namespace Pelican {

namespace {

std::string eventDisplayName(std::string name) {
    const auto qualifier = name.rfind("::");
    if (qualifier != std::string::npos) {
        name.erase(0, qualifier + 2);
    }
    return name;
}

using Json = nlohmann::json;

[[noreturn]] void validationError(EventPayloadErrorCode code, std::string message) {
    throw EventPayloadValidationError(code, std::move(message));
}

std::string fieldMessage(std::string_view event_name, std::string_view field_name, std::string_view detail) {
    return "event '" + std::string{event_name} + "' field '" + std::string{field_name} + "' " +
           std::string{detail};
}

bool isSignedType(PayloadFieldType type) {
    return type >= PayloadFieldType::I8 && type <= PayloadFieldType::I64;
}

bool isUnsignedType(PayloadFieldType type) {
    return type >= PayloadFieldType::U8 && type <= PayloadFieldType::U64;
}

std::pair<std::int64_t, std::int64_t> signedLimits(PayloadFieldType type) {
    switch (type) {
    case PayloadFieldType::I8:
        return {std::numeric_limits<std::int8_t>::min(), std::numeric_limits<std::int8_t>::max()};
    case PayloadFieldType::I16:
        return {std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()};
    case PayloadFieldType::I32:
        return {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()};
    case PayloadFieldType::I64:
        return {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()};
    default:
        return {0, 0};
    }
}

std::uint64_t unsignedLimit(PayloadFieldType type) {
    switch (type) {
    case PayloadFieldType::U8:
        return std::numeric_limits<std::uint8_t>::max();
    case PayloadFieldType::U16:
        return std::numeric_limits<std::uint16_t>::max();
    case PayloadFieldType::U32:
        return std::numeric_limits<std::uint32_t>::max();
    case PayloadFieldType::U64:
        return std::numeric_limits<std::uint64_t>::max();
    default:
        return 0;
    }
}

constexpr std::uint64_t max_safe_json_integer = UINT64_C(9007199254740992);

std::int64_t validateSigned(const Json &value, const PayloadFieldSchema &field, std::string_view event_name) {
    std::int64_t result = 0;
    if (value.is_number_unsigned()) {
        const auto unsigned_value = value.get<std::uint64_t>();
        if (unsigned_value > max_safe_json_integer || unsigned_value > static_cast<std::uint64_t>(INT64_MAX)) {
            validationError(EventPayloadErrorCode::TypeMismatch,
                            fieldMessage(event_name, field.name, "is outside the exact JSON integer range"));
        }
        result = static_cast<std::int64_t>(unsigned_value);
    } else if (value.is_number_integer()) {
        result = value.get<std::int64_t>();
        if (result < -static_cast<std::int64_t>(max_safe_json_integer) ||
            result > static_cast<std::int64_t>(max_safe_json_integer)) {
            validationError(EventPayloadErrorCode::TypeMismatch,
                            fieldMessage(event_name, field.name, "is outside the exact JSON integer range"));
        }
    } else {
        validationError(EventPayloadErrorCode::TypeMismatch,
                        fieldMessage(event_name, field.name, "must be an integer token"));
    }
    const auto [min_value, max_value] = signedLimits(field.type);
    if (result < min_value || result > max_value) {
        validationError(EventPayloadErrorCode::TypeMismatch,
                        fieldMessage(event_name, field.name, "is not representable by its integer type"));
    }
    if (const auto *range = std::get_if<std::pair<std::int64_t, std::int64_t>>(&field.range);
        range != nullptr && (result < range->first || result > range->second)) {
        validationError(EventPayloadErrorCode::OutOfRange, fieldMessage(event_name, field.name, "is out of range"));
    }
    return result;
}

std::uint64_t validateUnsigned(const Json &value, const PayloadFieldSchema &field, std::string_view event_name) {
    std::uint64_t result = 0;
    if (value.is_number_unsigned()) {
        result = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value < 0) {
            validationError(EventPayloadErrorCode::TypeMismatch,
                            fieldMessage(event_name, field.name, "must not be negative"));
        }
        result = static_cast<std::uint64_t>(signed_value);
    } else {
        validationError(EventPayloadErrorCode::TypeMismatch,
                        fieldMessage(event_name, field.name, "must be an integer token"));
    }
    if (result > max_safe_json_integer) {
        validationError(EventPayloadErrorCode::TypeMismatch,
                        fieldMessage(event_name, field.name, "is outside the exact JSON integer range"));
    }
    if (result > unsignedLimit(field.type)) {
        validationError(EventPayloadErrorCode::TypeMismatch,
                        fieldMessage(event_name, field.name, "is not representable by its integer type"));
    }
    if (const auto *range = std::get_if<std::pair<std::uint64_t, std::uint64_t>>(&field.range);
        range != nullptr && (result < range->first || result > range->second)) {
        validationError(EventPayloadErrorCode::OutOfRange, fieldMessage(event_name, field.name, "is out of range"));
    }
    return result;
}

void validateFloating(const Json &value, const PayloadFieldSchema &field, std::string_view event_name,
                      bool requires_f32) {
    if (!value.is_number()) {
        validationError(EventPayloadErrorCode::TypeMismatch,
                        fieldMessage(event_name, field.name, "must be a number"));
    }
    const double result = value.get<double>();
    if (!std::isfinite(result)) {
        validationError(EventPayloadErrorCode::TypeMismatch,
                        fieldMessage(event_name, field.name, "must be finite"));
    }
    if (requires_f32) {
        constexpr double max_f32 = static_cast<double>(std::numeric_limits<float>::max());
        if (result < -max_f32 || result > max_f32) {
            validationError(EventPayloadErrorCode::TypeMismatch,
                            fieldMessage(event_name, field.name, "is not representable as F32"));
        }
    }
    if (const auto *range = std::get_if<std::pair<double, double>>(&field.range);
        range != nullptr && (result < range->first || result > range->second)) {
        validationError(EventPayloadErrorCode::OutOfRange, fieldMessage(event_name, field.name, "is out of range"));
    }
}

std::size_t vectorLength(PayloadFieldType type) {
    switch (type) {
    case PayloadFieldType::Vec2:
        return 2;
    case PayloadFieldType::Vec3:
        return 3;
    case PayloadFieldType::Vec4:
    case PayloadFieldType::Quat:
        return 4;
    default:
        return 0;
    }
}

void validateField(const Json &value, const PayloadFieldSchema &field, std::string_view event_name) {
    if (isSignedType(field.type)) {
        (void)validateSigned(value, field, event_name);
        return;
    }
    if (isUnsignedType(field.type)) {
        (void)validateUnsigned(value, field, event_name);
        return;
    }
    if (field.type == PayloadFieldType::F32 || field.type == PayloadFieldType::F64) {
        validateFloating(value, field, event_name, field.type == PayloadFieldType::F32);
        return;
    }
    if (field.type == PayloadFieldType::String) {
        if (!value.is_string()) {
            validationError(EventPayloadErrorCode::TypeMismatch,
                            fieldMessage(event_name, field.name, "must be a string"));
        }
        return;
    }
    if (!value.is_array() || value.size() != vectorLength(field.type)) {
        validationError(EventPayloadErrorCode::TypeMismatch,
                        fieldMessage(event_name, field.name, "has an invalid array shape"));
    }
    for (const auto &component : value) {
        validateFloating(component, field, event_name, true);
    }
}

void validateObject(std::string_view event_name, const EventPayloadSchema &schema, const Json &payload) {
    if (!payload.is_object()) {
        validationError(EventPayloadErrorCode::PayloadMustBeObject,
                        "event '" + std::string{event_name} + "' payload must be an object");
    }
    if (schema.unknown_fields_reject) {
        for (auto it = payload.begin(); it != payload.end(); ++it) {
            const bool known = std::ranges::any_of(schema.fields, [&](const PayloadFieldSchema &field) {
                return field.name == it.key();
            });
            if (!known) {
                validationError(EventPayloadErrorCode::UnknownField,
                                fieldMessage(event_name, it.key(), "is unknown"));
            }
        }
    }
    for (const auto &field : schema.fields) {
        const auto it = payload.find(field.name);
        if (it == payload.end()) {
            validationError(EventPayloadErrorCode::MissingField,
                            fieldMessage(event_name, field.name, "is required"));
        }
        validateField(*it, field, event_name);
    }
}

} // namespace

namespace internal {

RegistrationToken UserEventRegistererTemplatePublic::__registerEvent(
    EventTypeRegistration registration) {
    registration.owner = currentRegistrationOwner();
    registration.name = eventDisplayName(std::move(registration.name));

    const auto same_name =
        std::find_if(event_types.begin(), event_types.end(), [&](const EventTypeRegistration &existing) {
            return existing.name == registration.name;
        });
    if (same_name != event_types.end()) {
        if (same_name->type == registration.type) {
            if (same_name->owner == registration.owner ||
                same_name->owner == engineRegistrationOwner ||
                registration.owner == engineRegistrationOwner) {
                return same_name->token;
            }
        }
        if (same_name->owner == engineRegistrationOwner || registration.owner == engineRegistrationOwner) {
            throw std::runtime_error("duplicate event name: " + registration.name);
        }
    }

    const auto same_type =
        std::find_if(event_types.begin(), event_types.end(), [&](const EventTypeRegistration &existing) {
            return existing.type == registration.type;
        });
    if (same_type != event_types.end()) {
        if (same_type->name == registration.name) {
            if (same_type->owner == registration.owner ||
                same_type->owner == engineRegistrationOwner ||
                registration.owner == engineRegistrationOwner) {
                return same_type->token;
            }
        }
        if (same_type->owner == engineRegistrationOwner || registration.owner == engineRegistrationOwner) {
            throw std::runtime_error("duplicate event type registered as '" + same_type->name + "' and '" +
                                     registration.name + "'");
        }
    }

    if (catalog_frozen && registration.owner == engineRegistrationOwner) {
        throw std::runtime_error("event registration after catalog validation is forbidden");
    }

    registration.token = acquireRegistrationToken(
        RegistrationKind::event, registration.owner, registration.name);
    const auto token = registration.token;
    try {
        event_types.push_back(std::move(registration));
    } catch (...) {
        (void)releaseRegistrationToken(token, RegistrationKind::event);
        throw;
    }
    return token;
}

void UserEventRegistererTemplatePublic::__emit(QueuedEvent event) {
    pending_events.push_back(std::move(event));
}

std::size_t UserEventRegistererTemplatePublic::emitByName(std::string_view name, const void *payload_json) {
    const auto &registration = validateByName(name, payload_json);

    __emit(QueuedEvent{
        .type = registration.type,
        .name = registration.name,
        .payload = (++payload_load_calls, registration.load_json_payload(payload_json)),
        .owner = registration.owner,
    });
    return 1;
}

const EventTypeRegistration &
UserEventRegistererTemplatePublic::validateByName(std::string_view name, const void *payload_json) const {
    const auto *registration = findByName(name);
    if (registration == nullptr) {
        validationError(EventPayloadErrorCode::UnknownEvent, "unknown event type: " + std::string{name});
    }
    if (registration->schema_state == EventSchemaState::Opaque) {
        validationError(EventPayloadErrorCode::NoPayloadEvent,
                        "event type does not expose a payload schema: " + std::string{name});
    }
    if (registration->schema_state == EventSchemaState::Payloadless) {
        if (payload_json == nullptr) {
            return *registration;
        }
        validateObject(name, EventPayloadSchema{}, *static_cast<const Json *>(payload_json));
        return *registration;
    }
    if (payload_json == nullptr) {
        validationError(EventPayloadErrorCode::PayloadRequired,
                        "typed event requires an object payload: " + std::string{name});
    }
    validateObject(name, registration->schema, *static_cast<const Json *>(payload_json));
    return *registration;
}

void UserEventRegistererTemplatePublic::validateEventPayload(std::string_view name, const void *payload_json) const {
    (void)validateByName(name, payload_json);
}

EventSchemaLookup UserEventRegistererTemplatePublic::findEventSchema(std::string_view name) const {
    const auto *registration = findByName(name);
    if (registration == nullptr) {
        return {};
    }
    static constexpr EventPayloadSchema empty_schema{};
    return EventSchemaLookup{
        .state = registration->schema_state,
        .schema = registration->schema_state == EventSchemaState::Typed
                      ? &registration->schema
                      : (registration->schema_state == EventSchemaState::Payloadless ? &empty_schema : nullptr),
    };
}

const EventTypeRegistration *UserEventRegistererTemplatePublic::findByType(std::type_index type) const {
    const auto it = std::find_if(event_types.begin(), event_types.end(), [&](const EventTypeRegistration &registration) {
        return registration.type == type;
    });
    return it == event_types.end() ? nullptr : &*it;
}

const EventTypeRegistration *UserEventRegistererTemplatePublic::findByName(std::string_view name) const {
    const auto it = std::find_if(event_types.begin(), event_types.end(), [&](const EventTypeRegistration &registration) {
        return registration.name == name;
    });
    return it == event_types.end() ? nullptr : &*it;
}

void UserEventRegistererTemplatePublic::validateCatalogAndFreeze() {
    if (catalog_frozen) {
        return;
    }
    for (const auto &registration : event_types) {
        if (registration.schema_state != EventSchemaState::Typed) {
            continue;
        }
        const auto actual = registration.list_ref_fields();
        const auto expected = registration.schema.fields;
        if (actual.size() != expected.size()) {
            throw std::runtime_error("event catalog mismatch for '" + registration.name + "': descriptor has " +
                                     std::to_string(expected.size()) + " fields but ref records " +
                                     std::to_string(actual.size()));
        }
        for (std::size_t i = 0; i < expected.size(); ++i) {
            if (actual[i] != expected[i].name) {
                throw std::runtime_error("event catalog mismatch for '" + registration.name + "' at field " +
                                         std::to_string(i) + ": descriptor '" + std::string{expected[i].name} +
                                         "', ref '" + actual[i] + "'");
            }
        }
    }
    catalog_frozen = true;
}

void UserEventRegistererTemplatePublic::freezeCatalog() noexcept {
    catalog_frozen = true;
}

std::size_t UserEventRegistererTemplatePublic::pendingEventCount() const noexcept {
    return pending_events.size() + deliver_now_events.size();
}

std::size_t UserEventRegistererTemplatePublic::payloadLoadCallCount() const noexcept {
    return payload_load_calls;
}

void UserEventRegistererTemplatePublic::freezePendingEventsForFrame() {
#ifndef NDEBUG
    validateCatalogAndFreeze();
#else
    freezeCatalog();
#endif
    assert(deliver_now_events.empty() && "frozen events must be dispatched before the next frame");
    deliver_now_events.swap(pending_events);
}

std::vector<QueuedEvent> UserEventRegistererTemplatePublic::drainFrozenEvents() {
    std::vector<QueuedEvent> events;
    events.swap(deliver_now_events);
    return events;
}

void UserEventRegistererTemplatePublic::clearPendingEvents() {
    pending_events.clear();
    deliver_now_events.clear();
}

UserEventRegistererTemplatePublic &getEventRegisterer() {
    static UserEventRegistererTemplatePublic registerer;
    return registerer;
}

void freezePendingEventsForFrame() {
    getEventRegisterer().freezePendingEventsForFrame();
}

void dispatchFrozenEvents(GameContext &ctx) {
    auto events = getEventRegisterer().drainFrozenEvents();
    for (const auto &event : events) {
        dispatchEventToRegisteredGameSystems(event, ctx);
    }
}

void clearPendingEvents() {
    getEventRegisterer().clearPendingEvents();
}

std::size_t emitEventByName(std::string_view name, const void *payload_json) {
    return getEventRegisterer().emitByName(name, payload_json);
}

void validateEventPayload(std::string_view name, const void *payload_json) {
    getEventRegisterer().validateEventPayload(name, payload_json);
}

EventSchemaLookup findEventSchema(std::string_view name) {
    return getEventRegisterer().findEventSchema(name);
}

void validateEventCatalog() {
    getEventRegisterer().validateCatalogAndFreeze();
}

void unregisterEvent(RegistrationToken token) {
    auto &registerer = getEventRegisterer();
    const auto found = std::find_if(
        registerer.event_types.begin(), registerer.event_types.end(),
        [token](const EventTypeRegistration &registration) {
            return registration.token == token;
        });
    if (found == registerer.event_types.end()) {
        throw std::runtime_error("stale event registration token");
    }

    const auto replacement = std::find_if(
        registerer.event_types.begin(), registerer.event_types.end(),
        [&](const EventTypeRegistration &registration) {
            return &registration != &*found && registration.type == found->type;
        });
    if (replacement == registerer.event_types.end()) {
        for (const auto &system : getGameSystemRegisterer().registeredSystems()) {
            const auto handler = std::find_if(
                system.event_handlers.begin(), system.event_handlers.end(),
                [&](const GameSystemEventHandlerRegistration &candidate) {
                    return candidate.event_type == found->type;
                });
            if (handler != system.event_handlers.end()) {
                throw std::runtime_error(
                    "cannot unregister event '" + found->name +
                    "': dependent game system '" + system.name + "' remains");
            }
        }
        for (const auto &behavior : getBehaviorRegisterer().registeredBehaviors()) {
            const auto handler = std::find_if(
                behavior.event_handlers.begin(), behavior.event_handlers.end(),
                [&](const BehaviorEventHandlerRegistration &candidate) {
                    return candidate.event_type == found->type;
                });
            if (handler != behavior.event_handlers.end()) {
                throw std::runtime_error(
                    "cannot unregister event '" + found->name +
                    "': dependent behavior '" + behavior.stable_name + "' remains");
            }
        }
    }

    registerer.event_types.erase(found);
    (void)releaseRegistrationToken(token, RegistrationKind::event);
}

void unregisterEvents(RegistrationOwner owner) noexcept {
    auto &registerer = getEventRegisterer();
    const auto removed = [owner](const QueuedEvent &event) {
        return event.owner == owner;
    };
    std::erase_if(registerer.pending_events, removed);
    std::erase_if(registerer.deliver_now_events, removed);
    for (const auto token : registrationTokens(owner, RegistrationKind::event)) {
        try {
            unregisterEvent(token);
        } catch (...) {
            // Continuing into FreeLibrary would leave a dependent registration
            // pointing at an unloaded event type.
            std::terminate();
        }
    }
}

std::size_t eventRegistrationCount(RegistrationOwner owner) noexcept {
    const auto &events = getEventRegisterer().event_types;
    return static_cast<std::size_t>(std::count_if(events.begin(), events.end(),
        [owner](const EventTypeRegistration &event) { return event.owner == owner; }));
}

} // namespace internal

} // namespace Pelican
