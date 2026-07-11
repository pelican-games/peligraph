#pragma once

#include "../../serialize/jsonarchive.hpp"
#include "../../serialize/serialize.hpp"

#include <concepts>
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
};

using JsonPayloadLoadFn = std::shared_ptr<const void> (*)(const void *payload_json);

struct EventTypeRegistration {
    std::string name;
    std::type_index type = std::type_index{typeid(void)};
    JsonPayloadLoadFn load_json_payload = nullptr;
};

class UserEventRegistererTemplatePublic {
    std::vector<EventTypeRegistration> event_types;
    std::vector<QueuedEvent> pending_events;
    std::vector<QueuedEvent> deliver_now_events;

    void __registerEvent(EventTypeRegistration registration);
    void __emit(QueuedEvent event);

  public:
    template <class Event> void registerEvent(std::string name) {
        using EventType = std::remove_cvref_t<Event>;
        static_assert(std::is_object_v<EventType>, "events must be object types");
        static_assert(!std::is_pointer_v<EventType>, "events must be emitted by value, not pointer");
        static_assert(std::copy_constructible<EventType>, "events must be copy constructible");

        __registerEvent(EventTypeRegistration{
            .name = std::move(name),
            .type = std::type_index{typeid(EventType)},
            .load_json_payload =
                []() constexpr -> JsonPayloadLoadFn {
                if constexpr (std::default_initializable<EventType> &&
                              ISerializable<EventType, JsonArchiveLoader>) {
                    return [](const void *payload_json) -> std::shared_ptr<const void> {
                        EventType event{};
                        JsonArchiveLoader archive{payload_json};
                        event.ref(archive);
                        return std::make_shared<EventType>(std::move(event));
                    };
                } else if constexpr (std::default_initializable<EventType>) {
                    return [](const void *) -> std::shared_ptr<const void> {
                        return std::make_shared<EventType>();
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
        });
    }

    std::size_t emitByName(std::string_view name, const void *payload_json);
    const EventTypeRegistration *findByType(std::type_index type) const;
    const EventTypeRegistration *findByName(std::string_view name) const;
    void freezePendingEventsForFrame();
    std::vector<QueuedEvent> drainFrozenEvents();
    void clearPendingEvents();
};

UserEventRegistererTemplatePublic &getEventRegisterer();
void freezePendingEventsForFrame();
void dispatchFrozenEvents(GameContext &ctx);
void clearPendingEvents();
std::size_t emitEventByName(std::string_view name, const void *payload_json);

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
        PELICAN_DETAIL_CONCAT(PelicanEventAutoRegister_, unique_id)() {                                            \
            ::Pelican::internal::getEventRegisterer().registerEvent<Type>(#Type);                                  \
        }                                                                                                          \
    };                                                                                                             \
    static const PELICAN_DETAIL_CONCAT(PelicanEventAutoRegister_, unique_id)                                       \
        PELICAN_DETAIL_CONCAT(pelican_event_auto_register_, unique_id);                                            \
    }
#define PELICAN_REGISTER_EVENT(Type) PELICAN_REGISTER_EVENT_IMPL(Type, __COUNTER__)
