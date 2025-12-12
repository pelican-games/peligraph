#include "componentinfo.hpp"

namespace Pelican {

ComponentInfoManager::ComponentInfoManager() {}

void ComponentInfoManager::registerComponent(ComponentInfo info) {
    if (infos.size() < info.id + 1)
        infos.resize(info.id + 1);
    infos[info.id] = info;
    name_id_map.insert({info.name, info.id});
}

size_t ComponentInfoManager::getIndexFromComponentId(ComponentId id) const { return static_cast<size_t>(id); }
size_t ComponentInfoManager::getSizeFromIndex(size_t index) const { return infos[index].size; }
ComponentId ComponentInfoManager::getComponentIdByName(const std::string &name) const { return name_id_map.at(name); }

void ComponentInfoManager::loadByJson(void *dst_ptr, const nlohmann::json &hint) const {
    const auto id = getComponentIdByName(hint.at("name"));
    infos[id].cb_load_by_json(dst_ptr, hint);
}

} // namespace Pelican