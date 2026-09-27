#include "playmode.hpp"

#if PELICAN_WITH_AUDIO
#include "../audio/audio.hpp"
#endif
#include "../container.hpp"
#include "../gamelogic/behaviorarena.hpp"
#include "../renderer/camera.hpp"
#include "../userpublic/details/event/registerer.hpp"
#include "../userpublic/userinput.hpp"
#include "../../project/inputactionoverlay.hpp"

#include <string>

namespace Pelican::internal {

namespace {

using PlayState = EngineTime::PlayState;

std::string_view freeCameraOverlay(EngineLaunchFreeCameraPreset preset) {
    return preset == EngineLaunchFreeCameraPreset::Unity
               ? freeCameraUnityInputActionOverlayReference
               : freeCameraBlenderInputActionOverlayReference;
}

// What follows the play state once it has changed.
void applyPlayStateSurroundings(PlayState state) {
#if PELICAN_WITH_AUDIO
    if (auto *audio = FastModuleContainer::tryGet<Audio>())
        audio->setAllPaused(state != PlayState::playing);
#endif
    if (!launchedInEditState()) return;
    // The editor's keys reach the game while it plays, and the editor's
    // again otherwise.
    setEditorInputOverlaysStacked(state != PlayState::playing);
    if (auto *camera = FastModuleContainer::tryGet<Camera>();
        camera != nullptr && !camera->editorCameraName().empty()) {
        camera->setViewCamera(state == PlayState::edit ? Camera::ViewCamera::editor
                                                       : Camera::ViewCamera::game);
    }
}

} // namespace

void completeEditStartLaunch(EngineLaunchConfig &config) {
    if (config.start_state != EngineStartState::edit || config.free_camera) return;
    config.free_camera = EngineLaunchFreeCamera{};
    config.input_action_overlays.emplace_back(freeCameraOverlay(config.free_camera->preset));
}

bool launchedInEditState() noexcept {
    const auto *config = FastModuleContainer::tryGet<EngineLaunchConfig>();
    return config != nullptr && config->start_state == EngineStartState::edit;
}

void applyLaunchStartState() {
    if (!launchedInEditState()) return;
    GET_MODULE(EngineTime).setPlayState(PlayState::edit);
    GET_MODULE(BehaviorAttachmentArena).setDormant(true);
}

PlayStateChange changePlayState(PlayState target,
                                const std::function<void()> &rebuild_runtime) {
    auto &time = GET_MODULE(EngineTime);
    const auto previous = time.playState();
    if (target == previous) return {previous, target};
    const bool session_starts = previous == PlayState::edit;
    const bool session_ends = target == PlayState::edit;
    if (!session_starts && !session_ends) {
        time.setPlayState(target);
        applyPlayStateSurroundings(target);
        return {previous, target};
    }

    auto &arena = GET_MODULE(BehaviorAttachmentArena);
    const auto clock = time.gameClock();
    clearPendingEvents();
    time.setPlayState(target);
    if (session_starts) {
        time.beginGameSession();
    } else {
        time.endGameSession();
    }
    arena.setDormant(session_ends);
    try {
        if (rebuild_runtime) rebuild_runtime();
    } catch (...) {
        arena.setDormant(!session_ends);
        time.setPlayState(previous);
        time.restoreGameClock(clock);
        throw;
    }
    applyPlayStateSurroundings(target);
    return {previous, target};
}

} // namespace Pelican::internal
