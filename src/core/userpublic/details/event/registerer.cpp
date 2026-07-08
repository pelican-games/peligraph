#include "registerer.hpp"

#include "../system/registerer.hpp"

#include <algorithm>
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

} // namespace

namespace internal {

void UserEventRegistererTemplatePublic::__registerEvent(EventTypeRegistration registration) {
    registration.name = eventDisplayName(std::move(registration.name));

    const auto same_name =
        std::find_if(event_types.begin(), event_types.end(), [&](const EventTypeRegistration &existing) {
            return existing.name == registration.name;
        });
    if (same_name != event_types.end()) {
        if (same_name->type == registration.type) {
            return;
        }
        throw std::runtime_error("duplicate event name: " + registration.name);
    }

    const auto same_type =
        std::find_if(event_types.begin(), event_types.end(), [&](const EventTypeRegistration &existing) {
            return existing.type == registration.type;
        });
    if (same_type != event_types.end()) {
        if (same_type->name == registration.name) {
            return;
        }
        throw std::runtime_error("duplicate event type registered as '" + same_type->name + "' and '" +
                                 registration.name + "'");
    }

    event_types.push_back(std::move(registration));
}

void UserEventRegistererTemplatePublic::__emit(QueuedEvent event) {
    pending_events.push_back(std::move(event));
}

std::size_t UserEventRegistererTemplatePublic::emitByName(std::string_view name, const void *payload_json) {
    const auto *registration = findByName(name);
    if (registration == nullptr) {
        throw std::runtime_error("unknown event type: " + std::string{name});
    }
    if (registration->load_json_payload == nullptr) {
        throw std::runtime_error("event type does not support JSON payload loading: " + std::string{name});
    }

    __emit(QueuedEvent{
        .type = registration->type,
        .name = registration->name,
        .payload = registration->load_json_payload(payload_json),
    });
    return 1;
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

std::vector<QueuedEvent> UserEventRegistererTemplatePublic::drainPendingEvents() {
    std::vector<QueuedEvent> events;
    events.swap(pending_events);
    return events;
}

void UserEventRegistererTemplatePublic::clearPendingEvents() {
    pending_events.clear();
}

UserEventRegistererTemplatePublic &getEventRegisterer() {
    static UserEventRegistererTemplatePublic registerer;
    return registerer;
}

void dispatchPendingEvents(GameContext &ctx) {
    auto events = getEventRegisterer().drainPendingEvents();
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

} // namespace internal

} // namespace Pelican
