#include "framephase.hpp"

#if PELICAN_WITH_AUDIO
#include "../audio/audio.hpp"
#endif
#include "../ecs/core.hpp"
#include "../ecs/predefined/animationsystem.hpp"
#include "../gamelogic/behaviorarena.hpp"
#include "../loader/runtimespawn.hpp"
#include "../loader/scene.hpp"
#include "../os/inputstate.hpp"
#include "../os/inputsequence.hpp"
#include "../os/window.hpp"
#include "../ui/module.hpp"
#include "../vkcore/rendertarget.hpp"
#include "../playback/camerabake.hpp"
#include "../playback/seqplayer.hpp"
#include "../userpublic/details/event/registerer.hpp"
#include "../userpublic/details/system/registerer.hpp"
#include "../userpublic/gamecontext.hpp"
#include "../userpublic/userinput.hpp"
#include "enginetime.hpp"
#include "../watch/reloadservice.hpp"
#if PELICAN_WITH_ANIMATION
#include "../animation/animationservice.hpp"
#include "../animation/posegraphruntime.hpp"
#include "../animation/vrmapplication.hpp"
#endif
#if PELICAN_WITH_IMGUI
#include "../imgui/imguiruntime.hpp"
#include "../imgui/imguisystem.hpp"
#include "../launchconfig.hpp"
#endif

#include <cassert>

namespace Pelican {

namespace {

void *editor_commit_queue_context = nullptr;
EditorCommitQueueHook editor_commit_queue_hook = nullptr;

struct FrameStateModules {
    watch::ReloadService *reload_service;
#if PELICAN_WITH_AUDIO
    Audio *audio;
#endif
    InputSequenceRuntime &input_sequence;
    InputState &input_state;
    ui::UiModule *ui_module;
    RenderTarget *render_target;
#if PELICAN_WITH_IMGUI
    ImGuiSystem *imgui_system;
#endif
    ECSCore &ecs;
    BehaviorAttachmentArena &behavior_arena;
    EngineTime &engine_time;
    SeqPlayer &seq_player;
    SceneLoader &scene_loader;
    RuntimeSpawner &runtime_spawner;
};

// GP-4: this machine's window-to-framebuffer conversion. With a window, its
// framebuffer over its window size; without one (headless), cursor events
// have no window to be measured in and are taken as the output's pixels.
PointerTransform livePointerTransform() {
    if (const auto *window = FastModuleContainer::tryGet<Window>()) {
        return window->pointerTransform();
    }
    PointerTransform transform;
    if (const auto *target = FastModuleContainer::tryGet<RenderTarget>()) {
        const auto extent = target->getExtent();
        transform.framebuffer_width = extent.width;
        transform.framebuffer_height = extent.height;
    }
    return transform;
}

FrameStateModules resolveFrameStateModules() {
    auto *ui_module = FastModuleContainer::tryGet<ui::UiModule>();
    auto *render_target = FastModuleContainer::tryGet<RenderTarget>();
    if (ui_module == nullptr || render_target == nullptr) {
        ui_module = nullptr;
        render_target = nullptr;
    }
#if PELICAN_WITH_IMGUI
    ImGuiSystem *imgui_system = nullptr;
    const auto &launch_config = GET_MODULE(EngineLaunchConfig);
    if (!isImGuiRuntimeEnabled(launch_config)) {
        // The gate can close at a logical frame boundary (notably when XR
        // activation wins). Never carry an already-begun ImGui frame across
        // that transition into a graph which has no ImGui render pass.
        if (auto *existing = FastModuleContainer::tryGet<ImGuiSystem>()) {
            existing->endFrameIfStarted();
        }
    }
    invokeImGuiRuntimeCallback(launch_config, [&] {
        imgui_system = &GET_MODULE(ImGuiSystem);
    });
#endif
    return {
        FastModuleContainer::tryGet<watch::ReloadService>(),
#if PELICAN_WITH_AUDIO
        FastModuleContainer::tryGet<Audio>(),
#endif
        GET_MODULE(InputSequenceRuntime),
        GET_MODULE(InputState),
        ui_module,
        render_target,
#if PELICAN_WITH_IMGUI
        imgui_system,
#endif
        GET_MODULE(ECSCore),
        GET_MODULE(BehaviorAttachmentArena),
        GET_MODULE(EngineTime),
        GET_MODULE(SeqPlayer),
        GET_MODULE(SceneLoader),
        GET_MODULE(RuntimeSpawner),
    };
}

} // namespace

void prepareFrameStateModules() {
    (void)resolveFrameStateModules();
}

bool installEditorCommitQueueHook(void *context,
                                  EditorCommitQueueHook hook) noexcept {
    if (context == nullptr || hook == nullptr ||
        editor_commit_queue_hook != nullptr) {
        return false;
    }
    editor_commit_queue_context = context;
    editor_commit_queue_hook = hook;
    return true;
}

void removeEditorCommitQueueHook(void *context) noexcept {
    if (context != editor_commit_queue_context) return;
    editor_commit_queue_hook = nullptr;
    editor_commit_queue_context = nullptr;
}

void invokeEditorCommitQueueHook() noexcept {
    if (editor_commit_queue_hook != nullptr) {
        editor_commit_queue_hook(editor_commit_queue_context);
    }
}

void updateFrameState() {
    auto modules = resolveFrameStateModules();
    // Audio completion happens asynchronously. Reap it at the common frame
    // boundary before game code can observe this frame's state.
#if PELICAN_WITH_AUDIO
    if (modules.audio != nullptr) modules.audio->update();
#endif
    // Reload publication is a frame-boundary operation and happens before any
    // phase can observe game/runtime state.
    if (modules.reload_service != nullptr) modules.reload_service->applyFrame();
    invokeEditorCommitQueueHook();
    // PM-1a: whether this frame is a game step (playing, or one of the steps
    // advance_frames asked for while paused). A frame that is not one still
    // takes input, runs the editor systems, evaluates the animation, seq and
    // graph at the stopped game time and draws; what it leaves out is game
    // code and what only game code causes (docs/design_play_mode.md 4.1).
    const bool game_step = modules.engine_time.gameStepping();
    // GP-2: the boundary where GameContext::spawn / destroy land -- after the
    // other boundary operations above, which may replace the scene run, and
    // before any game code of this frame, so a request made anywhere during
    // the previous frame is in place for all of this one.
    if (game_step) modules.runtime_spawner.applyBoundary();
    GameContext game_context;
    forEachFramePhase([&](FramePhase phase) {
        switch (phase) {
        case FramePhase::freeze_events:
            // Frozen and delivered together or not at all: freezing without
            // delivering trips the event registerer's frame check.
            if (game_step) internal::freezePendingEventsForFrame();
            break;
        case FramePhase::freeze_input:
            modules.input_sequence.prepareFrame(modules.input_state, livePointerTransform());
            modules.input_state.beginFrame();
            modules.input_sequence.recordFrame(modules.input_state.currentFrameInput());
            break;
        case FramePhase::freeze_actions:
#if PELICAN_WITH_IMGUI
            if (modules.imgui_system != nullptr)
                modules.imgui_system->routeInputAndBeginFrame(modules.input_state);
#endif
            if (modules.ui_module != nullptr) {
                // Paused, the game UI still follows the pointer but queues
                // no event for game code.
                modules.ui_module->setEventsSuppressed(!game_step);
                const auto &mask = modules.input_state.consumptionMask();
                const bool tool_owns_pointer = mask.pointer_motion ||
                    mask.consumesControl(KeyCode::MouseLeft) ||
                    mask.consumesControl(KeyCode::MouseRight) ||
                    mask.consumesControl(KeyCode::MouseMiddle);
                if (!tool_owns_pointer)
                    modules.ui_module->routeFrameInput(modules.input_state,
                                                       modules.render_target->getExtent());
            }
            internal::freezeInputActionsFrame();
            break;
        case FramePhase::deliver_events:
            if (game_step) internal::dispatchFrozenEvents(game_context);
            break;
        case FramePhase::update_game:
            modules.ecs.update();
            internal::updateRegisteredGameSystems(
                game_context, game_step ? internal::GameSystemScope::all
                                        : internal::GameSystemScope::editor_only);
#if PELICAN_WITH_ANIMATION
            // Retire animation sinks whose scene entity or renderer instance
            // was replaced before invoking any registered pose callbacks.
            Animation::animationServiceRuntime().synchronizeSceneObjects();
            GET_MODULE(AnimationSystem).updateGraphs();
            if (const auto status =
                    Vrm::applicationServiceRuntime()
                        .ensureStandardPhasesRegistered();
                status != Animation::Status::ok) {
                throw std::runtime_error(
                    "VRM application phase registration failed");
            }
            if (const auto status = Animation::animationServiceRuntime().runAllPhases(
                    modules.engine_time.evaluationIndex(),
                    game_step ? Animation::AnimationServiceRuntime::PhaseOwners::all
                              : Animation::AnimationServiceRuntime::PhaseOwners::engine_only);
                status != Animation::Status::ok) {
                // A phase callback answers with a status because an exception
                // cannot cross the C boundary. The pose graph kept the
                // exception it caught, so ask for it before falling back to a
                // message that says only that some phase failed: what reaches
                // the author has to name the port, not the frame.
                Animation::poseGraphRuntime().rethrowPendingFailure();
                throw std::runtime_error("animation phase evaluation failed");
            }
#endif
            modules.seq_player.update(modules.engine_time.now());
            // A ctx.loadScene request is game code's; paused, it waits.
            if (game_step) modules.scene_loader.applyPendingLoad();
            break;
        default:
            assert(false && "unknown frame phase");
            break;
        }
    });
    // GP-3: what game code wrote through ctx.ui() this frame lands here, once,
    // after every phase that runs game code and before the frame is drawn.
    // The UI module is read directly rather than through modules.ui_module,
    // which drops it when there is no render target to route input against.
    if (auto *ui_module = FastModuleContainer::tryGet<ui::UiModule>())
        ui_module->commitFrameWrites();
    modules.scene_loader.updateGraphRuntime();
    if (auto *camera_bake = FastModuleContainer::tryGet<CameraBakeRecorder>())
        camera_bake->recordFrame();
}

} // namespace Pelican
