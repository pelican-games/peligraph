#pragma once

#include <nlohmann/json.hpp>

namespace Pelican::internal {

// AG-3a. `get_status.animation`: who is driving a pose right now, what clip
// each one stands on, and where a state machine is between two states.
//
// A build without the animation module answers `{"available": false}` and
// nothing else -- not an empty sink list, which would read as "the module is
// here and nobody is animating". The two answers are different facts, and a
// tool deciding whether to show a timeline at all needs to tell them apart.
//
// Read at a frame boundary on the main thread, from what the frame that just
// ran recorded. It evaluates nothing: asking what an animation is doing must
// not be the thing that advances it.
nlohmann::json animationStatusJson();

} // namespace Pelican::internal
