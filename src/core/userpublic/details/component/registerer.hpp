#pragma once

#include <details/ecs/component.hpp>
#include <details/ecs/componentdeclare.hpp>
#include <string>
#include <vector>

namespace Pelican {

namespace internal {

struct ComponentLoaderInfo {
    std::string name;
};

class UserComponentRegistererTemplatePublic {
    void __registerComponent(ComponentId id, size_t sz, ComponentLoaderInfo loader);

  public:
    template <class Component> void registerComponent(ComponentLoaderInfo loader) {
        __registerComponent(ComponentIdByType<Component>::value, sizeof(Component), loader);
    }
};

UserComponentRegistererTemplatePublic &getComponentRegisterer();

} // namespace internal

}; // namespace Pelican
