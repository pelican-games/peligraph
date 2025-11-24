#include "pelican_core.hpp"
#include "appflow/loop.hpp"
#include "log.hpp"
#include "vkcore/core.hpp"

#include "ecs/predefined.hpp"
#include "loader/basicconfig.hpp"
#include "loader/projectsrc.hpp"
#include "loader/scene.hpp"

#include "battery/embed.hpp"

namespace Pelican {

void PelicanCore::run() {
    try {
        setupLogger();

        FastModuleContainer container;
        GET_MODULE(ProjectSource).setSourceByData(b::embed<"default_config.json">().str());

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
