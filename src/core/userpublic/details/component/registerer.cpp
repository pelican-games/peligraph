#include "registerer.hpp"
#include "../ecs/componentinfo.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <string_view>
namespace Pelican {

namespace internal {

namespace {

constexpr std::array<std::string_view, 7> grandfathered_engine_names{
    "eid",          "transform", "localtransform", "simplemodelview",
    "camera",       "animation", "sprite_view",
};

void enforceComponentRegistrationPolicy(std::string_view name,
                                        RegistrationOwner owner) {
    const bool engine_owned = owner == engineRegistrationOwner;
    const bool reserved = name.starts_with("pelican_");
    const bool grandfathered =
        std::ranges::find(grandfathered_engine_names, name) !=
        grandfathered_engine_names.end();
    if (engine_owned && !reserved && !grandfathered) {
        throw ComponentRegistrationPolicyError{
            ComponentRegistrationPolicyErrorCode::builtin_name_unprefixed,
            std::string{name}, owner};
    }
    if (!engine_owned && reserved) {
        throw ComponentRegistrationPolicyError{
            ComponentRegistrationPolicyErrorCode::reserved_component_namespace,
            std::string{name}, owner};
    }
}

} // namespace

std::string_view componentRegistrationPolicyErrorCodeName(
    ComponentRegistrationPolicyErrorCode code) noexcept {
    switch (code) {
    case ComponentRegistrationPolicyErrorCode::builtin_name_unprefixed:
        return "builtin_name_unprefixed@1";
    case ComponentRegistrationPolicyErrorCode::reserved_component_namespace:
        return "reserved_component_namespace@1";
    }
    return "component_registration_policy_unknown@1";
}

ComponentRegistrationPolicyError::ComponentRegistrationPolicyError(
    ComponentRegistrationPolicyErrorCode code, std::string name,
    RegistrationOwner owner)
    : std::runtime_error(
          std::string{componentRegistrationPolicyErrorCodeName(code)} +
          nlohmann::ordered_json{
              {"name", name},
              {"owner_kind", owner == engineRegistrationOwner ? "engine"
                                                                : "external"}}
              .dump()),
      code_{code},
      payload_{{"name", std::move(name)},
               {"owner_kind", owner == engineRegistrationOwner ? "engine"
                                                                 : "external"}} {}

DECLARE_MODULE(UserComponentRegisterer) {
    UserComponentRegistererTemplatePublic sub;

  public:
    UserComponentRegistererTemplatePublic &getPublicSub() { return sub; }
};

RegistrationToken UserComponentRegistererTemplatePublic::__registerComponent(
    ComponentId id, size_t sz, ComponentLoaderInfo loader,
    RegistrationOwner owner) {
    enforceComponentRegistrationPolicy(loader.name, owner);
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
    info.owner = owner;

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

std::vector<std::string> componentRegistrationNames(RegistrationOwner owner) {
    const auto *manager = FastModuleContainer::tryGet<ComponentInfoManager>();
    return manager == nullptr ? std::vector<std::string>{}
                              : manager->registrationNames(owner);
}

} // namespace internal

} // namespace Pelican
