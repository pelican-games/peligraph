#include "pelican_core.hpp"
#include "../appflow/loop.hpp"
#include "../log.hpp"
#include "../vkcore/core.hpp"

#include "../ecs/predefined.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/projectsrc.hpp"
#include "../loader/scene.hpp"

namespace Pelican {

PelicanCore::PelicanCore() {
    setupLogger();
    settings_str = "{}";
}

PelicanCore::PelicanCore(std::string _settings_str) { settings_str = _settings_str; }

void PelicanCore::run() {
    try {
        FastModuleContainer container;
        GET_MODULE(ProjectSource).setSourceByData(settings_str);

        GET_MODULE(ECSPredefinedRegistration).reg();
        GET_MODULE(SceneLoader).load(GET_MODULE(ProjectBasicConfig).defaultSceneId());

        auto &loop = GET_MODULE(Loop);
        loop.run();

        // wait ongoing tasks
        GET_MODULE(VulkanManageCore).waitIdle();

    } catch (std::exception &e) {
        LOG_ERROR(logger, "Pelican fatal error : {}", e.what());
        return;
    }
}

} // namespace Pelican
