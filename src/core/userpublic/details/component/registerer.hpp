#pragma once

#include <details/ecs/component.hpp>
#include <details/ecs/componentdeclare.hpp>
#include <serialize/jsonarchive.hpp>
#include <serialize/serialize.hpp>
#include <string>
#include <vector>

namespace Pelican {

namespace internal {

class UserComponentRegistererTemplatePublic {
    struct ComponentLoaderInfo {
        std::string name;
        void (*json_loader)(void *component, JsonArchiveLoader &ar);
    };

    void __registerComponent(ComponentId id, size_t sz, ComponentLoaderInfo loader);

  public:
    template <class Component>
        requires ISerializable<Component, JsonArchiveLoader>
    void registerComponent(std::string name) {
        __registerComponent(ComponentIdByType<Component>::value, sizeof(Component),
                            ComponentLoaderInfo{
                                .name = name,
                                .json_loader = [](void *c, JsonArchiveLoader &ar) { static_cast<Component *>(c)->ref(ar); },
                            });
    }
};

UserComponentRegistererTemplatePublic &getComponentRegisterer();

} // namespace internal

}; // namespace Pelican
