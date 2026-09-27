#pragma once

#include "../launchconfig.hpp"
#include "enginetime.hpp"

#include <functional>

namespace Pelican::internal {

// PM-1b. The edit state and the moves between it and the game
// (docs/design_play_mode.md sections 2, 4.4, 5 and 6).

// What the command line adds to a --start-state edit launch: the editor
// camera, the way --free-camera prepares one (its default preset unless the
// launch named one), with that camera's input overlay. Any other launch is
// left as it is.
void completeEditStartLaunch(EngineLaunchConfig &config);

bool launchedInEditState() noexcept;

// Startup, before the first scene load: an engine launched with
// --start-state edit starts in edit, its behaviors dormant.
void applyLaunchStartState();

struct PlayStateChange {
    EngineTime::PlayState previous = EngineTime::PlayState::playing;
    EngineTime::PlayState state = EngineTime::PlayState::playing;
};

// set_play_state's engine half. The same state again changes nothing.
//  - playing <-> paused: the game clock stops or goes on (PM-1a).
//  - edit -> playing / paused: a play session starts. The game's pending
//    events are dropped, the game clock and the game frame go back to 0,
//    behaviors wake, and `rebuild_runtime` makes the runtime again from the
//    current authoring (a scene load keeping the authoring ids), which makes
//    the instances and runs onInit. Paused, the game stands at frame 0.
//  - playing / paused -> edit: the session ends. The game's pending events
//    are dropped, behaviors go dormant, the game clock goes back to the time
//    it had when the session began, and `rebuild_runtime` makes the edit
//    runtime from the current authoring: the old instances get onDestroy and
//    what was spawned is gone. The authoring is not put back (PM-2).
// Then the whole audio mix is held unless playing and, in a --start-state edit
// launch, the editor's input overlays leave the action set stack while
// playing and the view camera is the editor's in edit and the game's
// otherwise. When `rebuild_runtime` throws, the state, the clock and the
// behaviors' dormancy are put back and the exception goes on.
PlayStateChange changePlayState(EngineTime::PlayState target,
                                const std::function<void()> &rebuild_runtime);

} // namespace Pelican::internal
