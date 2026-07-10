#include "pelican_core.hpp"
#include "../appflow/loop.hpp"
#include "../appflow/teardown.hpp"
#include "../log.hpp"
#include "../vkcore/core.hpp"

#include "../ecs/predefined.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/projectsrc.hpp"
#include "../loader/scene.hpp"

#include <utility>

namespace Pelican {

PelicanCore::PelicanCore() {
    setupLogger();
    settings_str = "{}";
}

PelicanCore::PelicanCore(std::string _settings_str) : PelicanCore(std::move(_settings_str), false) {}

PelicanCore::PelicanCore(std::string _settings_str, bool reserve_stdout_for_protocol) {
    setupLogger(reserve_stdout_for_protocol);
    settings_str = _settings_str;
}

bool PelicanCore::run() {
    FastModuleContainer container;
    RuntimeTeardownGuard teardown;
    bool succeeded = true;
    try {
        GET_MODULE(ProjectSource).setSourceByData(settings_str);

        GET_MODULE(ECSPredefinedRegistration).reg();
        GET_MODULE(SceneLoader).load(GET_MODULE(ProjectBasicConfig).defaultSceneId());

        auto &loop = GET_MODULE(Loop);
        loop.run();

        // wait ongoing tasks
        GET_MODULE(VulkanManageCore).waitIdle();

    } catch (std::exception &e) {
        LOG_ERROR(logger, "Pelican fatal error : {}", e.what());
        succeeded = false;
    } catch (...) {
        LOG_ERROR(logger, "Pelican fatal error: non-standard exception");
        succeeded = false;
    }
    teardown.run();
    return succeeded;
}

} // namespace Pelican
