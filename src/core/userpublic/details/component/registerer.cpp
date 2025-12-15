#include "registerer.hpp"
#include "../ecs/componentinfo.hpp"

namespace Pelican {

namespace internal {

DECLARE_MODULE(UserComponentRegisterer) {
    UserComponentRegistererTemplatePublic sub;

  public:
    UserComponentRegistererTemplatePublic &getPublicSub() { return sub; }
};

void UserComponentRegistererTemplatePublic::__registerComponent(ComponentId id, size_t sz, ComponentLoaderInfo loader) {
    Pelican::ComponentInfo info;
    info.id = id;
    info.name = loader.name;
    info.size = sz;
    info.cb_init = [](void *ptr) {};
    info.cb_deinit = [](void *ptr) {};
    info.cb_load_by_json = [](void *ptr, const nlohmann::json &json) {};

    GET_MODULE(ComponentInfoManager).registerComponent(info);
}

UserComponentRegistererTemplatePublic &getComponentRegisterer() {
    return GET_MODULE(UserComponentRegisterer).getPublicSub();
}

} // namespace internal

} // namespace Pelican
