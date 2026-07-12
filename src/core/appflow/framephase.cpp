#include "framephase.hpp"

#include "../ecs/core.hpp"
#include "../loader/scene.hpp"
#include "../os/inputstate.hpp"
#include "../os/inputsequence.hpp"
#include "../playback/camerabake.hpp"
#include "../playback/seqplayer.hpp"
#include "../userpublic/details/event/registerer.hpp"
#include "../userpublic/details/system/registerer.hpp"
#include "../userpublic/gamecontext.hpp"
#include "../userpublic/userinput.hpp"
#include "enginetime.hpp"
#if PELICAN_WITH_IMGUI
#include "../imgui/imguiruntime.hpp"
#include "../imgui/imguisystem.hpp"
#include "../launchconfig.hpp"
#endif

#include <cassert>

namespace Pelican {

void updateFrameState() {
    GameContext game_context;
    forEachFramePhase([&](FramePhase phase) {
        switch (phase) {
        case FramePhase::freeze_events:
            internal::freezePendingEventsForFrame();
            break;
        case FramePhase::freeze_input:
            GET_MODULE(InputSequenceRuntime).prepareFrame(GET_MODULE(InputState));
            GET_MODULE(InputState).beginFrame();
            GET_MODULE(InputSequenceRuntime).recordFrame(GET_MODULE(InputState).currentFrameInput());
            break;
        case FramePhase::freeze_actions:
#if PELICAN_WITH_IMGUI
            invokeImGuiRuntimeCallback(GET_MODULE(EngineLaunchConfig), [&] {
                GET_MODULE(ImGuiSystem).routeInputAndBeginFrame(GET_MODULE(InputState));
            });
#endif
            internal::freezeInputActionsFrame();
            break;
        case FramePhase::deliver_events:
            internal::dispatchFrozenEvents(game_context);
            break;
        case FramePhase::update_game:
            GET_MODULE(ECSCore).update();
            internal::updateRegisteredGameSystems(game_context);
            GET_MODULE(SeqPlayer).update(GET_MODULE(EngineTime).now());
            GET_MODULE(SceneLoader).applyPendingLoad();
            break;
        default:
            assert(false && "unknown frame phase");
            break;
        }
    });
    if (FastModuleContainer::isInitialized<CameraBakeRecorder>()) {
        GET_MODULE(CameraBakeRecorder).recordFrame();
    }
}

} // namespace Pelican
