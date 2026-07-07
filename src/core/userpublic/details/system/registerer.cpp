#include "registerer.hpp"

#include <algorithm>

namespace Pelican {

namespace internal {

void UserGameSystemRegistererTemplatePublic::__registerSystem(GameSystemRegistration registration) {
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
            system.update(ctx);
        }
    }
}

} // namespace internal

} // namespace Pelican
