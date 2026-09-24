#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace Pelican {

// AU-1 decision 9. A player with a window mixes into the audio device (the
// Studio's embedded player among them). A headless run mixes into a
// device-less engine that nothing reads unless a caller asks for its PCM, so
// its cursors are the frames actually mixed, not a clock made up for them.
// The null backend is what either falls back to when its engine cannot open.
//
// AU-3: a launch can also name one (the player's --audio-backend, else the
// PELICAN_AUDIO_BACKEND environment variable). The names are known to every
// build, so a launch is read the same way whether or not the build has audio.
enum class AudioBackendKind : std::uint8_t { device, offline, null };

constexpr std::string_view audioBackendKindName(AudioBackendKind kind) noexcept {
    switch (kind) {
    case AudioBackendKind::device: return "device";
    case AudioBackendKind::offline: return "offline";
    case AudioBackendKind::null: return "null";
    }
    return "null";
}

// The backend a launch names, exactly as get_status spells it; none for any
// other text.
constexpr std::optional<AudioBackendKind> audioBackendKindFromName(std::string_view name) noexcept {
    if (name == "device") return AudioBackendKind::device;
    if (name == "offline") return AudioBackendKind::offline;
    if (name == "null") return AudioBackendKind::null;
    return std::nullopt;
}

// AU-3: a launch that names a backend none of the three is stopped by this name.
inline constexpr std::string_view audioBackendUnknownCode = "audio.backend_unknown@1";

} // namespace Pelican
