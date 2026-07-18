#include "registerer.hpp"

#include <algorithm>

namespace Pelican {

namespace internal {

RegistrationToken UserGameSystemRegistererTemplatePublic::__registerSystem(
    GameSystemRegistration registration) {
    registration.owner = currentRegistrationOwner();
    registration.token = acquireRegistrationToken(
        RegistrationKind::system, registration.owner, registration.name);
    const auto token = registration.token;
    try {
        systems.push_back(std::move(registration));
    } catch (...) {
        (void)releaseRegistrationToken(token, RegistrationKind::system);
        throw;
    }
    return token;
}

const std::vector<GameSystemRegistration> &UserGameSystemRegistererTemplatePublic::registeredSystems() const noexcept {
    return systems;
}

UserGameSystemRegistererTemplatePublic &getGameSystemRegisterer() {
    static UserGameSystemRegistererTemplatePublic registerer;
    return registerer;
}

std::vector<GameSystemRegistration> sortGameSystemRegistrations(std::vector<GameSystemRegistration> systems) {
    std::sort(systems.begin(), systems.end(), [](const GameSystemRegistration &lhs,
                                                 const GameSystemRegistration &rhs) {
        if (lhs.order != rhs.order) {
            return lhs.order < rhs.order;
        }
        return lhs.name < rhs.name;
    });
    return systems;
}

void updateRegisteredGameSystems(GameContext &ctx) {
    auto systems = sortGameSystemRegistrations(getGameSystemRegisterer().registeredSystems());
    for (const auto &system : systems) {
        if (system.update != nullptr) {
            ScopedRegistrationOwner owner_scope{system.owner};
            system.update(ctx);
        }
    }
}

void dispatchEventToRegisteredGameSystems(const QueuedEvent &event, GameContext &ctx) {
    auto systems = sortGameSystemRegistrations(getGameSystemRegisterer().registeredSystems());
    for (const auto &system : systems) {
        for (const auto &handler : system.event_handlers) {
            if (handler.event_type == event.type && handler.dispatch != nullptr) {
                ScopedRegistrationOwner owner_scope{system.owner};
                handler.dispatch(event.payload.get(), ctx);
            }
        }
        if (system.dispatch_queued_event != nullptr) {
            ScopedRegistrationOwner owner_scope{system.owner};
            system.dispatch_queued_event(event, ctx);
        }
    }
}

void unregisterGameSystem(RegistrationToken token) {
    auto &systems = getGameSystemRegisterer().systems;
    const auto found = std::find_if(
        systems.begin(), systems.end(),
        [token](const GameSystemRegistration &system) { return system.token == token; });
    if (found == systems.end()) {
        throw std::runtime_error("stale game system registration token");
    }
    systems.erase(found);
    (void)releaseRegistrationToken(token, RegistrationKind::system);
}

void unregisterGameSystems(RegistrationOwner owner) noexcept {
    auto &systems = getGameSystemRegisterer().systems;
    for (const auto token : registrationTokens(owner, RegistrationKind::system)) {
        const auto found = std::find_if(
            systems.begin(), systems.end(),
            [token](const GameSystemRegistration &system) { return system.token == token; });
        if (found != systems.end()) systems.erase(found);
        (void)releaseRegistrationToken(token, RegistrationKind::system);
    }
}

std::size_t gameSystemRegistrationCount(RegistrationOwner owner) noexcept {
    return static_cast<std::size_t>(std::count_if(
        getGameSystemRegisterer().registeredSystems().begin(),
        getGameSystemRegisterer().registeredSystems().end(),
        [owner](const GameSystemRegistration &system) { return system.owner == owner; }));
}

} // namespace internal

} // namespace Pelican
