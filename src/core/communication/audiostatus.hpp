#pragma once

#include <nlohmann/json.hpp>

namespace Pelican::internal {

// AU-1 decision 9: `get_status.audio` -- which mixer this player's voices
// reach: the audio device (a player with a window, the Studio's embedded one
// among them), the device-less engine of a headless run, or the null
// fallback. The player creates Audio before the module graph freezes; a host
// that never did reports no backend rather than opening one to answer.
// AU-3: a backend named at launch (--audio-backend / PELICAN_AUDIO_BACKEND)
// reports the same way, with no field of its own: `backend` is what took
// effect, and a named backend that did not is the only other case, which
// fallback_reason already names ("device: ..."). A named null never falls back.
//
// CT-1: `voices`, one entry per audio.playback node of the graph:
// {node, state: "idle" | "playing" | "paused", position_seconds}. `node` is
// the graph node id (an instance's node under its instance path); the
// position is the source second the graph last put the voice at, 0 when idle.
// Read from what the last frame asked of each voice, not from the mixer, so a
// null backend reports the same values. Outside a play session (the edit
// state) nothing plays: every voice is idle at 0. A paused game holds the
// whole mix, so a voice that would play is paused.
//
// AT-1 decision 6: `clock`, the audio clock the frame read --
// {source: "device" | "offline" | "game_clock", sample_rate, audio_time,
// played_frames, output_latency_seconds, latency_source: "estimated" |
// "none", drift_seconds} -- and `master`, the master voice with a voice --
// {node, position_seconds, heard_position_seconds, offset_seconds} -- or
// null. The null backend has no clock of its own: the game clock stands in
// for it, and a master is heard where the graph put it.
//
// A build without audio answers {"available": false} and nothing else.
nlohmann::json audioStatusJson();

} // namespace Pelican::internal
