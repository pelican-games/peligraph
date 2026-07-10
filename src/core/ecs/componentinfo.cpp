#include "componentinfo.hpp"

#include <stdexcept>

namespace Pelican {

ComponentInfoManager::ComponentInfoManager() {}

void ComponentInfoManager::registerComponent(ComponentInfo info) {
    if (info.size == 0 || info.alignment == 0 || info.cb_construct == nullptr || info.cb_destroy == nullptr ||
        info.cb_relocate == nullptr) {
        throw std::invalid_argument("Component registration requires typed lifecycle metadata");
    }
    if (infos.size() < info.id + 1)
        infos.resize(info.id + 1);
    infos[info.id] = info;
    name_id_map.insert_or_assign(info.name, info.id);
}

size_t ComponentInfoManager::getIndexFromComponentId(ComponentId id) const { return static_cast<size_t>(id); }
const ComponentInfo &ComponentInfoManager::getFromIndex(size_t index) const { return infos.at(index); }
const ComponentInfo &ComponentInfoManager::get(ComponentId id) const { return infos.at(static_cast<size_t>(id)); }
ComponentId ComponentInfoManager::getComponentIdByName(const std::string &name) const { return name_id_map.at(name); }

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
