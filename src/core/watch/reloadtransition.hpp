#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Pelican::watch {

// The reload facts an accepted editor edit must not straddle: the watcher's
// state and epoch, and each runtime participant's reload outcome.
//
// This is deliberately not ReloadService::statusJson(). A participant's
// `details` are its self-description, not transitions: re-lowering the render
// pipeline for a resized window republishes its generation there, and hashing
// that JSON once made every resize drop accepted edits. Only fields that move
// when a reload is requested, attempted, applied or fails belong here.
struct ReloadTransitionFacts {
    struct Participant {
        std::string name;
        bool requested = false;
        std::uint64_t attempted = 0;
        std::uint64_t applied = 0;
        std::uint64_t failed = 0;
        std::string error_code;
        std::string last_error;

        bool operator==(const Participant &) const = default;
    };

    std::string state;
    std::uint64_t epoch = 0;
    std::vector<Participant> participants;

    bool operator==(const ReloadTransitionFacts &) const = default;
};

} // namespace Pelican::watch
