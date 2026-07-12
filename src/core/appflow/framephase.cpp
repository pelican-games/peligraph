#include "framephase.hpp"

#include "../ecs/core.hpp"
#include "../loader/scene.hpp"
#include "../os/inputstate.hpp"
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
            GET_MODULE(InputState).beginFrame();
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
}

} // namespace Pelican
