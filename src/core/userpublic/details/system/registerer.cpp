#include "registerer.hpp"

#include <algorithm>

namespace Pelican {

namespace internal {

void UserGameSystemRegistererTemplatePublic::__registerSystem(GameSystemRegistration registration) {
    registration.owner = currentRegistrationOwner();
    systems.push_back(std::move(registration));
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
    }
}

void unregisterGameSystems(RegistrationOwner owner) noexcept {
    auto &systems = getGameSystemRegisterer().systems;
    std::erase_if(systems, [owner](const GameSystemRegistration &system) {
        return system.owner == owner;
    });
}

std::size_t gameSystemRegistrationCount(RegistrationOwner owner) noexcept {
    return static_cast<std::size_t>(std::count_if(
        getGameSystemRegisterer().registeredSystems().begin(),
        getGameSystemRegisterer().registeredSystems().end(),
        [owner](const GameSystemRegistration &system) { return system.owner == owner; }));
}

} // namespace internal

} // namespace Pelican
