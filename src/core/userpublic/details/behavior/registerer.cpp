#include "registerer.hpp"

#include <algorithm>

namespace Pelican::internal {

void UserBehaviorRegistererTemplatePublic::__registerBehavior(
    BehaviorRegistration registration) {
    registration.owner = currentRegistrationOwner();
    const auto duplicate_name = std::find_if(
        behaviors.begin(), behaviors.end(), [&](const BehaviorRegistration &existing) {
            return existing.owner == registration.owner &&
                   existing.stable_name == registration.stable_name;
        });
    if (duplicate_name != behaviors.end()) {
        throw std::runtime_error("behavior stable name is registered more than once by one owner: " +
                                 registration.stable_name);
    }
    if (registration.owner == engineRegistrationOwner) {
        const auto collides_with_dll = std::find_if(
            behaviors.begin(), behaviors.end(), [&](const BehaviorRegistration &existing) {
                return existing.stable_name == registration.stable_name;
            });
        if (collides_with_dll != behaviors.end()) {
            throw std::runtime_error("engine behavior stable name collides with another owner: " +
                                     registration.stable_name);
        }
    } else {
        const auto collides_with_engine = std::find_if(
            behaviors.begin(), behaviors.end(), [&](const BehaviorRegistration &existing) {
                return existing.owner == engineRegistrationOwner &&
                       existing.stable_name == registration.stable_name;
            });
        if (collides_with_engine != behaviors.end()) {
            throw std::runtime_error("game behavior stable name collides with an engine behavior: " +
                                     registration.stable_name);
        }
    }
    behaviors.push_back(std::move(registration));
}

const BehaviorRegistration *UserBehaviorRegistererTemplatePublic::findByName(
    std::string_view stable_name) const noexcept {
    const auto found = std::find_if(
        behaviors.begin(), behaviors.end(), [&](const BehaviorRegistration &registration) {
            return registration.stable_name == stable_name;
        });
    return found == behaviors.end() ? nullptr : &*found;
}

const BehaviorRegistration *UserBehaviorRegistererTemplatePublic::findByNameAndOwner(
    std::string_view stable_name, RegistrationOwner owner) const noexcept {
    const auto found = std::find_if(
        behaviors.begin(), behaviors.end(), [&](const BehaviorRegistration &registration) {
            return registration.owner == owner && registration.stable_name == stable_name;
        });
    return found == behaviors.end() ? nullptr : &*found;
}

UserBehaviorRegistererTemplatePublic &getBehaviorRegisterer() {
    static UserBehaviorRegistererTemplatePublic registerer;
    return registerer;
}

void unregisterBehaviors(RegistrationOwner owner) noexcept {
    auto &behaviors = getBehaviorRegisterer().behaviors;
    std::erase_if(behaviors, [owner](const BehaviorRegistration &registration) {
        return registration.owner == owner;
    });
}

std::size_t behaviorRegistrationCount(RegistrationOwner owner) noexcept {
    const auto &behaviors = getBehaviorRegisterer().registeredBehaviors();
    return static_cast<std::size_t>(std::count_if(
        behaviors.begin(), behaviors.end(), [owner](const BehaviorRegistration &registration) {
            return registration.owner == owner;
        }));
}

std::string canonicalizeBehaviorParams(std::string_view stable_name,
                                       const nlohmann::json &params) {
    const auto *registration = getBehaviorRegisterer().findByName(stable_name);
    if (registration == nullptr) {
        throw std::runtime_error("unknown behavior type: " + std::string{stable_name});
    }
    return registration->canonicalize_params(params);
}

} // namespace Pelican::internal
