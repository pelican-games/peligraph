#include "framephase.hpp"

#include "../ecs/core.hpp"
#include "../gamelogic/behaviorarena.hpp"
#include "../loader/scene.hpp"
#include "../os/inputstate.hpp"
#include "../os/inputsequence.hpp"
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
#include "../animation/animationservice.hpp"
#include "../animation/vrmapplication.hpp"
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
};

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
    // Reload publication is a frame-boundary operation and happens before any
    // phase can observe game/runtime state.
    if (modules.reload_service != nullptr) modules.reload_service->applyFrame();
    invokeEditorCommitQueueHook();
    GameContext game_context;
    forEachFramePhase([&](FramePhase phase) {
        switch (phase) {
        case FramePhase::freeze_events:
            internal::freezePendingEventsForFrame();
            break;
        case FramePhase::freeze_input:
            modules.input_sequence.prepareFrame(modules.input_state);
            modules.input_state.beginFrame();
            modules.input_sequence.recordFrame(modules.input_state.currentFrameInput());
            break;
        case FramePhase::freeze_actions:
#if PELICAN_WITH_IMGUI
            if (modules.imgui_system != nullptr)
                modules.imgui_system->routeInputAndBeginFrame(modules.input_state);
#endif
            if (modules.ui_module != nullptr) {
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
            internal::dispatchFrozenEvents(game_context);
            break;
        case FramePhase::update_game:
            modules.ecs.update();
            internal::updateRegisteredGameSystems(game_context);
            if (const auto status =
                    Vrm::applicationServiceRuntime()
                        .ensureStandardPhasesRegistered();
                status != Animation::Status::ok) {
                throw std::runtime_error(
                    "VRM application phase registration failed");
            }
            if (const auto status = Animation::animationServiceRuntime().runAllPhases(
                    modules.engine_time.frameIndex());
                status != Animation::Status::ok) {
                throw std::runtime_error("animation phase evaluation failed");
            }
            modules.seq_player.update(modules.engine_time.now());
            modules.scene_loader.applyPendingLoad();
            break;
        default:
            assert(false && "unknown frame phase");
            break;
        }
    });
    if (auto *camera_bake = FastModuleContainer::tryGet<CameraBakeRecorder>())
        camera_bake->recordFrame();
}

} // namespace Pelican
