#pragma once

#include <string>
#include <vector>

namespace PelicanStudio {

// What Studio starts the engine with: the arguments, and -- when the
// configured ones (PELICAN_STUDIO_PLAYER_ARGUMENTS) keep Studio from adding
// one it would -- the line that says so.
struct StudioPlayerLaunch {
    std::vector<std::string> arguments;
    // PM-3b: empty when the engine starts in edit (--start-state edit), or
    // when the configured arguments name a start state of their own. Else one
    // sentence naming the argument that runs the game from its first frame.
    std::string start_state_notice;
};

StudioPlayerLaunch studioPlayerLaunch(std::vector<std::string> configured,
                                      const std::string &project_root);

std::vector<std::string> studioPlayerArgumentStrings(
    std::vector<std::string> configured,
    const std::string &project_root);

} // namespace PelicanStudio
