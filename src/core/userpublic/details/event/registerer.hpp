#pragma once

#include "payloadschema.hpp"
#include "../reload/registrationowner.hpp"
#include "../../serialize/jsonarchive.hpp"
#include "../../serialize/serialize.hpp"

#include <concepts>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <vector>

namespace Pelican {

class GameContext;

namespace internal {

template <int Index> struct EventCatalogTag {};
template <class Event> struct EventCatalogEntry {
    using type = Event;
};

struct QueuedEvent {
    std::type_index type = std::type_index{typeid(void)};
    std::string name;
    std::shared_ptr<const void> payload;
    RegistrationOwner owner = engineRegistrationOwner;
};

struct EventQueueDrainResult {
    std::size_t pending = 0;
    std::size_t frozen = 0;

    std::size_t total() const noexcept { return pending + frozen; }
};

using JsonPayloadLoadFn = std::shared_ptr<const void> (*)(const void *payload_json);
using RefFieldListFn = std::vector<std::string> (*)();

struct EventTypeRegistration {
    std::string name;
    std::type_index type = std::type_index{typeid(void)};
    EventSchemaState schema_state = EventSchemaState::Opaque;
    EventPayloadSchema schema{};
    JsonPayloadLoadFn load_json_payload = nullptr;
    RefFieldListFn list_ref_fields = nullptr;
    RegistrationOwner owner = engineRegistrationOwner;
    RegistrationToken token;
};

class UserEventRegistererTemplatePublic {
    std::vector<EventTypeRegistration> event_types;
    std::vector<QueuedEvent> pending_events;
    std::vector<QueuedEvent> deliver_now_events;
    std::size_t payload_load_calls = 0;
    bool catalog_frozen = false;

    PELICAN_API RegistrationToken __registerEvent(EventTypeRegistration registration);
    PELICAN_API void __emit(QueuedEvent event);
    const EventTypeRegistration &validateByName(std::string_view name, const void *payload_json) const;

    friend void unregisterEvent(RegistrationToken token);
    friend void unregisterEvents(RegistrationOwner owner) noexcept;
    friend std::size_t eventRegistrationCount(RegistrationOwner owner) noexcept;

  public:
    template <class Event> RegistrationToken registerEvent(std::string name) {
        using EventType = std::remove_cvref_t<Event>;
        static_assert(std::is_object_v<EventType>, "events must be object types");
        static_assert(!std::is_pointer_v<EventType>, "events must be emitted by value, not pointer");
        static_assert(std::copy_constructible<EventType>, "events must be copy constructible");

        constexpr bool has_descriptor = requires { EventType::pelican_payload; };
        constexpr bool json_serializable = ISerializable<EventType, JsonArchiveLoader>;
        constexpr bool record_serializable = ISerializable<EventType, RefFieldRecorder>;
        if constexpr (has_descriptor) {
            static_assert(std::default_initializable<EventType>,
                          "events with pelican_payload must be default initializable");
            static_assert(json_serializable,
                          "events with pelican_payload must be serializable with JsonArchiveLoader");
            static_assert(std::is_nothrow_default_constructible_v<EventType>,
                          "events with pelican_payload must be nothrow default constructible");
        }

        if constexpr (has_descriptor) {
            static_assert(record_serializable,
                          "typed event ref must support the event catalog recording archive");
        }

        return __registerEvent(EventTypeRegistration{
            .name = std::move(name),
            .type = std::type_index{typeid(EventType)},
            .schema_state = []() constexpr {
                if constexpr (has_descriptor) {
                    return EventSchemaState::Typed;
                } else if constexpr (!json_serializable && !record_serializable &&
                                     std::default_initializable<EventType>) {
                    return EventSchemaState::Payloadless;
                } else {
                    return EventSchemaState::Opaque;
                }
            }(),
            .schema = []() constexpr {
                if constexpr (has_descriptor) {
                    return internal::eventPayloadSchema<EventType>();
                } else {
                    return EventPayloadSchema{};
                }
            }(),
            .load_json_payload =
                []() constexpr -> JsonPayloadLoadFn {
                if constexpr (has_descriptor) {
                    return [](const void *payload_json) -> std::shared_ptr<const void> {
                        EventType event{};
                        JsonArchiveLoader archive{payload_json};
                        event.ref(archive);
                        return std::make_shared<EventType>(std::move(event));
                    };
                } else if constexpr (!json_serializable && !record_serializable &&
                                     std::default_initializable<EventType>) {
                    return [](const void *) -> std::shared_ptr<const void> {
                        return std::make_shared<EventType>();
                    };
                } else {
                    return nullptr;
                }
            }(),
            .list_ref_fields = []() constexpr -> RefFieldListFn {
                if constexpr (has_descriptor) {
                    return []() -> std::vector<std::string> {
                        EventType event{};
                        RefFieldRecorder recorder;
                        event.ref(recorder);
                        return recorder.names;
                    };
                } else {
                    return nullptr;
                }
            }(),
        });
    }

    template <class Event> void emit(const Event &event) {
        using EventType = std::remove_cvref_t<Event>;
        static_assert(std::is_object_v<EventType>, "events must be object types");
        static_assert(!std::is_pointer_v<EventType>, "events must be emitted by value, not pointer");
        static_assert(std::copy_constructible<EventType>, "events must be copy constructible");

        const auto *registration = findByType(std::type_index{typeid(EventType)});
        if (registration == nullptr) {
            throw std::runtime_error("event type is not registered");
        }
        __emit(QueuedEvent{
            .type = std::type_index{typeid(EventType)},
            .name = registration->name,
            .payload = std::make_shared<EventType>(event),
            .owner = registration->owner,
        });
    }

    std::size_t emitByName(std::string_view name, const void *payload_json);
    void validateEventPayload(std::string_view name, const void *payload_json) const;
    EventSchemaLookup findEventSchema(std::string_view name) const;
    PELICAN_API const EventTypeRegistration *findByType(std::type_index type) const;
    const EventTypeRegistration *findByName(std::string_view name) const;
    void validateCatalogAndFreeze();
    void freezeCatalog() noexcept;
    std::size_t pendingEventCount() const noexcept;
    std::size_t payloadLoadCallCount() const noexcept;
    void freezePendingEventsForFrame();
    std::vector<QueuedEvent> drainFrozenEvents();
    EventQueueDrainResult drainForTeardown() noexcept;
    void clearPendingEvents();
};

PELICAN_API UserEventRegistererTemplatePublic &getEventRegisterer();
void freezePendingEventsForFrame();
void dispatchFrozenEvents(GameContext &ctx);
EventQueueDrainResult drainPendingEventsForTeardown() noexcept;
void clearPendingEvents();
std::size_t emitEventByName(std::string_view name, const void *payload_json);
void validateEventPayload(std::string_view name, const void *payload_json);
EventSchemaLookup findEventSchema(std::string_view name);
void validateEventCatalog();
void unregisterEvent(RegistrationToken token);
void unregisterEvents(RegistrationOwner owner) noexcept;
std::size_t eventRegistrationCount(RegistrationOwner owner) noexcept;

} // namespace internal

} // namespace Pelican

#ifndef PELICAN_DETAIL_CONCAT_INNER
#define PELICAN_DETAIL_CONCAT_INNER(a, b) a##b
#endif
#ifndef PELICAN_DETAIL_CONCAT
#define PELICAN_DETAIL_CONCAT(a, b) PELICAN_DETAIL_CONCAT_INNER(a, b)
#endif

#define PELICAN_REGISTER_EVENT_IMPL(Type, unique_id)                                                               \
    static auto pelicanEventCatalogEntry(::Pelican::internal::EventCatalogTag<unique_id>)                           \
        -> ::Pelican::internal::EventCatalogEntry<Type> {                                                          \
        return {};                                                                                                 \
    }                                                                                                              \
    namespace {                                                                                                    \
    struct PELICAN_DETAIL_CONCAT(PelicanEventAutoRegister_, unique_id) {                                           \
        ::Pelican::internal::RegistrationToken token;                                                             \
        PELICAN_DETAIL_CONCAT(PelicanEventAutoRegister_, unique_id)() {                                            \
            token = ::Pelican::internal::getEventRegisterer().registerEvent<Type>(#Type);                          \
        }                                                                                                          \
    };                                                                                                             \
    static const PELICAN_DETAIL_CONCAT(PelicanEventAutoRegister_, unique_id)                                       \
        PELICAN_DETAIL_CONCAT(pelican_event_auto_register_, unique_id);                                            \
    }
#define PELICAN_REGISTER_EVENT(Type) PELICAN_REGISTER_EVENT_IMPL(Type, __COUNTER__)
