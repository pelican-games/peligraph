#include "registerer.hpp"
#include "../ecs/componentinfo.hpp"

#include <exception>
namespace Pelican {

namespace internal {

DECLARE_MODULE(UserComponentRegisterer) {
    UserComponentRegistererTemplatePublic sub;

  public:
    UserComponentRegistererTemplatePublic &getPublicSub() { return sub; }
};

RegistrationToken UserComponentRegistererTemplatePublic::__registerComponent(
    ComponentId id, size_t sz, ComponentLoaderInfo loader) {
    Pelican::ComponentInfo info;
    info.id = id;
    info.name = loader.name;
    info.size = sz;
    info.alignment = loader.alignment;
    info.cb_construct = loader.construct;
    info.cb_destroy = loader.destroy;
    info.cb_relocate = loader.relocate;
    info.cb_init = loader.init;
    info.cb_deinit = loader.deinit;
    info.cb_load_by_json2 = loader.json_loader;
    info.owner = currentRegistrationOwner();

    return GET_MODULE(ComponentInfoManager).registerComponent(std::move(info));
}

UserComponentRegistererTemplatePublic &getComponentRegisterer() {
    return GET_MODULE(UserComponentRegisterer).getPublicSub();
}

void unregisterComponent(RegistrationToken token) {
    GET_MODULE(ComponentInfoManager).unregisterComponent(token);
}

void unregisterComponents(RegistrationOwner owner) noexcept {
    auto *manager = FastModuleContainer::tryGet<ComponentInfoManager>();
    if (manager == nullptr) return;
    for (const auto token : registrationTokens(owner, RegistrationKind::component)) {
        try {
            manager->unregisterComponent(token);
        } catch (...) {
            // Continuing into FreeLibrary would leave component lifecycle
            // callbacks in a dependent ECS system pointing at unloaded code.
            std::terminate();
        }
    }
}

std::size_t componentRegistrationCount(RegistrationOwner owner) noexcept {
    const auto *manager = FastModuleContainer::tryGet<ComponentInfoManager>();
    return manager == nullptr ? 0 : manager->registrationCount(owner);
}

} // namespace internal

} // namespace Pelican
