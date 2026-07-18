#pragma once

#include "../container.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <unordered_map>

#include "component.hpp"
#include "entity.hpp"
#include <details/ecs/componentdeclare.hpp>
#include <details/reload/registrationowner.hpp>
#include <serialize/jsonarchive.hpp>

namespace Pelican {

namespace internal {
class UserComponentRegistererTemplatePublic;
}

struct ComponentInfo {
    ComponentId id;
    size_t size;
    size_t alignment;
    std::string name;
    void (*cb_construct)(void *ptr) = nullptr;
    void (*cb_destroy)(void *ptr) noexcept = nullptr;
    void (*cb_relocate)(void *dst, void *src) noexcept = nullptr;
    void (*cb_init)(void *ptr) = nullptr;
    void (*cb_deinit)(void *ptr) noexcept = nullptr;

    void (*cb_load_by_json2)(void *ptr, JsonArchiveLoader &json) = nullptr;
    internal::RegistrationOwner owner = internal::engineRegistrationOwner;
    internal::RegistrationToken token;
};

DECLARE_MODULE(ComponentInfoManager) {
    friend class internal::UserComponentRegistererTemplatePublic;
    std::vector<ComponentInfo> infos;
    std::unordered_map<std::string, ComponentId> name_id_map;

    internal::RegistrationToken registerComponent(ComponentInfo info);

  public:
    ComponentInfoManager();
    ~ComponentInfoManager();

    size_t getIndexFromComponentId(ComponentId id) const;
    const ComponentInfo &getFromIndex(size_t index) const;
    const ComponentInfo &get(ComponentId id) const;
    ComponentId getComponentIdByName(const std::string &name) const;
    void loadByJson(void *ptr, const nlohmann::json &json) const;
    void unregisterComponent(internal::RegistrationToken token);
    std::size_t registrationCount(internal::RegistrationOwner owner) const noexcept;
};

} // namespace Pelican
