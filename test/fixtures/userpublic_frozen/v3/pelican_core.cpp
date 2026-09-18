#include "pelican_core.hpp"
#include "../appflow/loop.hpp"
#include "../appflow/teardown.hpp"
#include "../terminalshutdown.hpp"
#include "../asset/model.hpp"
#include "../log.hpp"
#include "../material/projectmaterialasset.hpp"
#include "../vkcore/core.hpp"

#include "../ecs/predefined.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/projectsrc.hpp"
#include "../loader/scene.hpp"
#include "../gamelogic/gamelogicreload.hpp"
#include "../persistence/persistence.hpp"
#include "../startup.hpp"
#include "../launchconfig.hpp"
#include "../xractivation.hpp"
#include "../loader/pathresolver.hpp"
#include "../renderingpass/renderingpassconfigloader.hpp"
#include "../shader/raygrouplayout.hpp"
#include "../watch/reloadgate.hpp"
#include "../watch/reloadservice.hpp"
#include "../vkcore/renderer.hpp"
#include "../communication/renderconfigtransaction.hpp"
#include <components/spriteview.hpp>
#if PELICAN_WITH_AUDIO
#include "../audio/audio.hpp"
#endif
#if PELICAN_WITH_OPENXR
#include "../openxr/openxrdiscovery.hpp"
#endif

#include <utility>

namespace {

void configureRayLayoutModeBeforeComponentRegistration() {
#if PELICAN_RUNTIME_SHADER_COMPILER
    constexpr bool runtime_shader_compiler_enabled = true;
#else
    constexpr bool runtime_shader_compiler_enabled = false;
#endif
    auto &config = Pelican::GET_MODULE(Pelican::ProjectBasicConfig);
    auto &paths = Pelican::GET_MODULE(Pelican::PathResolver);
    const auto rendering = Pelican::loadRenderingPassConfigJsonFromString(
        config.renderingConfigJson(), "ProjectBasicConfig preflight");
    const auto ray_project = Pelican::loadRayTracingProjectConfig(
        rendering,
        [&paths](std::string_view reference) {
            return paths.loadText(reference);
        },
        runtime_shader_compiler_enabled);
    Pelican::GET_MODULE(Pelican::RayLayoutRuntimeState)
        .configure(Pelican::rayLayoutSemanticMode(ray_project.get()));
}

} // namespace

namespace Pelican {

namespace internal {

void runTerminalShutdown(RuntimeTeardownGuard &teardown) noexcept {
    teardown.run();
    shutdownConfiguredGameLogic();
}

} // namespace internal

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
    RuntimeTeardownGuard teardown{RuntimeTeardownMode::terminal_shutdown};
    bool succeeded = true;
    try {
        GET_MODULE(StartupMetrics).begin();
        auto &launch_config = GET_MODULE(EngineLaunchConfig);
#if PELICAN_WITH_OPENXR
        const auto xr_decision = resolveXrActivation(
            launch_config, true, XrDiscoveryHook{.query = OpenXr::queryDiscovery});
#else
        const auto xr_decision = resolveXrActivation(launch_config, false);
#endif
        launch_config.xr_requested_mode = xr_decision.requested_mode;
        launch_config.xr_mode = xr_decision.resolved_mode;
        launch_config.xr_active = xr_decision.active;
        if (!xr_decision.info.empty()) {
            LOG_INFO(logger, "{}", xr_decision.info);
        }
        GET_MODULE(ProjectSource).setSourceByData(settings_str);
        GET_MODULE(watch::ReloadGate).configureFromLaunch(launch_config);

        // A render-authoring document transaction may have crossed a process
        // crash. Resolve it before ProjectBasicConfig or Renderer can observe
        // any member of the document set.
        recoverRenderConfigDocumentTransaction(
            GET_MODULE(PathResolver).projectRoot());

        auto &persistence = GET_MODULE(Persistence);
        if (persistence.loadSettings()) {
#if PELICAN_WITH_AUDIO
            persistence.applyAudioSettings(GET_MODULE(Audio));
#endif
        }

        configureRayLayoutModeBeforeComponentRegistration();
        GET_MODULE(ECSPredefinedRegistration).reg();
        if (!initializeConfiguredGameLogic()) {
            const auto status = configuredGameLogicStatus();
            LOG_ERROR(logger,
                      "game logic startup initialization failed: error_code='{}' fields={}",
                      status.error_code, status.error_fields.dump());
        }
        // Validate and publish the render graph before a transactional scene
        // prepare can instantiate ModelAssetContainer and upload model data.
        // Invalid material routes must remain the first startup failure and
        // must not consume the vertex pool.
        (void)GET_MODULE(Renderer);
        {
            auto &config = GET_MODULE(ProjectBasicConfig);
            auto request = SceneLoadRequest::borrowed(
                config.defaultSceneId(), config.sceneProjectionState(),
                SceneLoadRoot::startup);
            const auto result =
                GET_MODULE(SceneLoader).load(std::move(request));
            (void)requireSceneLoadCommitted(result);
            if (const auto *failure =
                    std::get_if<CommittedWithEffectFailure>(&result)) {
                LOG_ERROR(logger,
                          "startup scene committed with effect failure: error_code='committed_with_effect_failure@1' diagnostics={}",
                          sceneLoadDiagnosticsJson(failure->diagnostics).dump());
            }
        }
        // Model CPU preparation is parallel, but its Vulkan/resource commit is
        // deliberately forced onto the startup thread before ECS systems run.
        // This also makes the permanent startup metric cover the whole phase.
        (void)GET_MODULE(ProjectMaterialAssetContainer);
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
    internal::runTerminalShutdown(teardown);
    return succeeded;
}

} // namespace Pelican
