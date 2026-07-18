#include "registerer.hpp"

#include <algorithm>

namespace Pelican::internal {

RegistrationToken UserBehaviorRegistererTemplatePublic::__registerBehavior(
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
    registration.token = acquireRegistrationToken(
        RegistrationKind::behavior, registration.owner,
        registration.stable_name);
    const auto token = registration.token;
    try {
        behaviors.push_back(std::move(registration));
    } catch (...) {
        (void)releaseRegistrationToken(token, RegistrationKind::behavior);
        throw;
    }
    return token;
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

void unregisterBehavior(RegistrationToken token) {
    auto &behaviors = getBehaviorRegisterer().behaviors;
    const auto found = std::find_if(
        behaviors.begin(), behaviors.end(),
        [token](const BehaviorRegistration &registration) {
            return registration.token == token;
        });
    if (found == behaviors.end()) {
        throw std::runtime_error("stale behavior registration token");
    }
    behaviors.erase(found);
    (void)releaseRegistrationToken(token, RegistrationKind::behavior);
}

void unregisterBehaviors(RegistrationOwner owner) noexcept {
    auto &behaviors = getBehaviorRegisterer().behaviors;
    for (const auto token : registrationTokens(owner, RegistrationKind::behavior)) {
        const auto found = std::find_if(
            behaviors.begin(), behaviors.end(),
            [token](const BehaviorRegistration &registration) {
                return registration.token == token;
            });
        if (found != behaviors.end()) behaviors.erase(found);
        (void)releaseRegistrationToken(token, RegistrationKind::behavior);
    }
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

void validateBehaviorReload(RegistrationOwner active_owner,
                            RegistrationOwner candidate_owner,
                            const nlohmann::json *authoring_scenes) {
    const auto &registry = getBehaviorRegisterer();
    const auto &registrations = registry.registeredBehaviors();
    for (const auto &active : registrations) {
        if (active.owner != active_owner) continue;

        const auto *candidate = registry.findByNameAndOwner(
            active.stable_name, candidate_owner);
        if (candidate == nullptr) {
            throw std::runtime_error(
                "behavior_type_removed: stable_name='" + active.stable_name +
                "' version=" + std::to_string(active.schema_version));
        }
        if (candidate->schema_version == active.schema_version &&
            candidate->params_schema_fingerprint !=
                active.params_schema_fingerprint) {
            throw std::runtime_error(
                "schema_changed_without_version_bump: stable_name='" +
                active.stable_name + "' version=" +
                std::to_string(active.schema_version));
        }
        if (candidate->schema_version == active.schema_version) continue;

        if (authoring_scenes == nullptr) {
            throw std::runtime_error(
                "schema_incompatible: stable_name='" + active.stable_name +
                "' version=" + std::to_string(active.schema_version) + "->" +
                std::to_string(candidate->schema_version) +
                " field='params' authoring document is unavailable");
        }

        for (auto scene_it = authoring_scenes->begin();
             scene_it != authoring_scenes->end(); ++scene_it) {
            const auto &objects = scene_it.value().at("objects");
            for (std::size_t object_index = 0; object_index < objects.size();
                 ++object_index) {
                const auto &object = objects.at(object_index);
                const auto &components = object.at("components");
                for (std::size_t component_index = 0;
                     component_index < components.size(); ++component_index) {
                    const auto &component = components.at(component_index);
                    if (component.value("name", std::string{}) != "behavior" ||
                        component.value("type", std::string{}) !=
                            active.stable_name) {
                        continue;
                    }
                    const auto params_it = component.find("params");
                    const nlohmann::json empty_params = nlohmann::json::object();
                    const auto &params = params_it == component.end()
                                             ? empty_params
                                             : *params_it;
                    try {
                        (void)candidate->canonicalize_params(params);
                    } catch (const StructFieldValidationError &error) {
                        throw std::runtime_error(
                            "schema_incompatible: stable_name='" +
                            active.stable_name + "' version=" +
                            std::to_string(active.schema_version) + "->" +
                            std::to_string(candidate->schema_version) +
                            " scene='" + scene_it.key() + "' object_index=" +
                            std::to_string(object_index) +
                            " component_index=" +
                            std::to_string(component_index) + " field='" +
                            std::string{error.path()} + "': " + error.what());
                    } catch (const std::exception &error) {
                        throw std::runtime_error(
                            "schema_incompatible: stable_name='" +
                            active.stable_name + "' version=" +
                            std::to_string(active.schema_version) + "->" +
                            std::to_string(candidate->schema_version) +
                            " scene='" + scene_it.key() + "' object_index=" +
                            std::to_string(object_index) +
                            " component_index=" +
                            std::to_string(component_index) +
                            " field='params': " + error.what());
                    }
                }
            }
        }
    }
}

} // namespace Pelican::internal
