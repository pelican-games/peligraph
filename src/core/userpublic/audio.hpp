#pragma once

#include <optional>

namespace Pelican {

struct SoundPlaybackState {
    float volume = 1.0f;
    float pitch = 1.0f;
    float effective_volume = 1.0f;
    bool playing = false;
    bool paused = false;
    // Source cursor in seconds, not device presentation time. A headless run
    // reports the frames its device-less engine actually mixed; the null
    // backend (no engine could open) has no clock and reports none.
    std::optional<double> position_seconds;
};

} // namespace Pelican
