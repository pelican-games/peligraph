#pragma once

#include <optional>

namespace Pelican {

struct SoundPlaybackState {
    float volume = 1.0f;
    float pitch = 1.0f;
    float effective_volume = 1.0f;
    bool playing = false;
    bool paused = false;
    // Source cursor in seconds, not device presentation time. No clock is
    // available for the headless/null backend.
    std::optional<double> position_seconds;
};

} // namespace Pelican
