#pragma once

#include "../container.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <unordered_map>

#include "component.hpp"
#include "entity.hpp"

namespace Pelican {

struct ComponentInfo {
    ComponentId id;
    uint32_t size;
    std::string name;
    void (*cb_init)(void *ptr);
    void (*cb_deinit)(void *ptr);

    void (*cb_load_by_json)(void *ptr, const nlohmann::json &json);
};

DECLARE_MODULE(ComponentInfoManager) {
    std::vector<ComponentInfo> infos;
    std::unordered_map<std::string, ComponentId> name_id_map;

  public:
    ComponentInfoManager();

    void registerComponent(ComponentInfo info);

    uint32_t getSizeFromComponentId(ComponentId id) const;
    ComponentId getComponentIdByName(const std::string &name) const;
    void loadByJson(void *ptr, const nlohmann::json &json) const;
};

template <class T> struct ComponentIdByType;

#define DECLARE_COMPONENT(_name, _id)                                                                                  \
    template <> struct ComponentIdByType<_name> {                                                                      \
        static constexpr ComponentId value = _id;                                                                      \
    };

DECLARE_COMPONENT(EntityId, 0);

} // namespace Pelican