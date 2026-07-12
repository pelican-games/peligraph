#include "pelican_core.hpp"
#include "../appflow/loop.hpp"
#include "../appflow/teardown.hpp"
#include "../asset/model.hpp"
#include "../log.hpp"
#include "../vkcore/core.hpp"

#include "../ecs/predefined.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/projectsrc.hpp"
#include "../loader/scene.hpp"
#include "../gamelogic/gamelogicreload.hpp"
#include "../persistence/persistence.hpp"
#include "../startup.hpp"
#include "../launchconfig.hpp"
#include "../loader/pathresolver.hpp"
#include "../watch/reloadgate.hpp"
#include "../watch/reloadservice.hpp"
#if PELICAN_WITH_AUDIO
#include "../audio/audio.hpp"
#endif

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
        GET_MODULE(StartupMetrics).begin();
        GET_MODULE(ProjectSource).setSourceByData(settings_str);
        GET_MODULE(watch::ReloadGate).configureFromLaunch(GET_MODULE(EngineLaunchConfig));

        auto &persistence = GET_MODULE(Persistence);
        if (persistence.loadSettings()) {
#if PELICAN_WITH_AUDIO
            persistence.applyAudioSettings(GET_MODULE(Audio));
#endif
        }

        GET_MODULE(ECSPredefinedRegistration).reg();
        (void)initializeConfiguredGameLogic();
        GET_MODULE(SceneLoader).load(GET_MODULE(ProjectBasicConfig).defaultSceneId());
        // Model CPU preparation is parallel, but its Vulkan/resource commit is
        // deliberately forced onto the startup thread before ECS systems run.
        // This also makes the permanent startup metric cover the whole phase.
        (void)GET_MODULE(ModelAssetContainer);

        // Runtime resources now represent the initial disk contents, so this
        // is the point at which the watcher may seed its live inventory.
        GET_MODULE(watch::ReloadService).setup(GET_MODULE(PathResolver));

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
    shutdownConfiguredGameLogic();
    return succeeded;
}

} // namespace Pelican
