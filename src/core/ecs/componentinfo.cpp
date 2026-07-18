#include "componentinfo.hpp"
#include "../userpublic/details/ecs/coretemplate.hpp"

#include <algorithm>
#include <stdexcept>

namespace Pelican {

ComponentInfoManager::ComponentInfoManager() {}

ComponentInfoManager::~ComponentInfoManager() {
    for (const auto &info : infos) {
        if (info.token) {
            (void)internal::releaseRegistrationToken(
                info.token, internal::RegistrationKind::component);
        }
    }
}

internal::RegistrationToken ComponentInfoManager::registerComponent(ComponentInfo info) {
    if (info.name.empty()) {
        throw std::invalid_argument("component registration name must not be empty");
    }
    if (static_cast<std::size_t>(info.id) >= MAX_COMPONENTS) {
        throw std::runtime_error("component '" + info.name + "' id " +
                                 std::to_string(info.id) +
                                 " exceeds the registration limit (<64)");
    }
    const auto index = static_cast<std::size_t>(info.id);
    if (index < infos.size() && infos[index].token) {
        throw std::runtime_error("component '" + info.name + "' duplicates id " +
                                 std::to_string(info.id) + " already registered by '" +
                                 infos[index].name + "'");
    }
    if (const auto duplicate_name = name_id_map.find(info.name);
        duplicate_name != name_id_map.end()) {
        throw std::runtime_error("component name '" + info.name +
                                 "' duplicates registered id " +
                                 std::to_string(duplicate_name->second) +
                                 " while incoming id is " +
                                 std::to_string(info.id));
    }
    if (info.size == 0 || info.alignment == 0 || info.cb_construct == nullptr || info.cb_destroy == nullptr ||
        info.cb_relocate == nullptr) {
        throw std::invalid_argument("component '" + info.name +
                                    "' registration requires typed lifecycle metadata");
    }
    info.token = internal::acquireRegistrationToken(
        internal::RegistrationKind::component, info.owner, info.name);
    const auto token = info.token;
    if (infos.size() < index + 1) infos.resize(index + 1);
    try {
        infos[index] = std::move(info);
        name_id_map.emplace(infos[index].name, infos[index].id);
    } catch (...) {
        infos[index] = {};
        (void)internal::releaseRegistrationToken(
            token, internal::RegistrationKind::component);
        throw;
    }
    return token;
}

void ComponentInfoManager::unregisterComponent(
    internal::RegistrationToken token) {
    const auto found = std::find_if(infos.begin(), infos.end(),
                                    [token](const ComponentInfo &info) {
                                        return info.token == token;
                                    });
    if (found == infos.end()) {
        throw std::runtime_error("stale component registration token");
    }
    const auto index = static_cast<std::size_t>(found->id);
    const auto dependents = internal::ecsComponentDependentNames(index);
    if (!dependents.empty()) {
        throw std::runtime_error("cannot unregister component '" + found->name +
                                 "': dependent ECS system '" + dependents.front() +
                                 "' remains");
    }
    name_id_map.erase(found->name);
    *found = {};
    (void)internal::releaseRegistrationToken(
        token, internal::RegistrationKind::component);
}

size_t ComponentInfoManager::getIndexFromComponentId(ComponentId id) const {
    return static_cast<std::size_t>(get(id).id);
}
const ComponentInfo &ComponentInfoManager::getFromIndex(size_t index) const {
    if (index >= infos.size() || !infos[index].token) {
        throw std::out_of_range("component index " + std::to_string(index) +
                                " is not registered");
    }
    return infos[index];
}
const ComponentInfo &ComponentInfoManager::get(ComponentId id) const {
    return getFromIndex(static_cast<size_t>(id));
}
ComponentId ComponentInfoManager::getComponentIdByName(const std::string &name) const { return name_id_map.at(name); }

std::size_t ComponentInfoManager::registrationCount(
    internal::RegistrationOwner owner) const noexcept {
    return static_cast<std::size_t>(std::count_if(
        infos.begin(), infos.end(), [owner](const ComponentInfo &info) {
            return info.token && info.owner == owner;
        }));
}

void ComponentInfoManager::loadByJson(void *dst_ptr, const nlohmann::json &hint) const {
    const auto id = getComponentIdByName(hint.at("name"));

    if (infos[id].cb_load_by_json2) {
        JsonArchiveLoader ar{static_cast<const void *>(&hint)};
        infos[id].cb_load_by_json2(dst_ptr, ar);
    } else {
        throw std::runtime_error("Component has no JSON serializer: " + infos[id].name);
    }
}

} // namespace Pelican
