#pragma once

#include "../container.hpp"
#include "../handle.hpp"
#include "../userpublic/audio.hpp"
#include "./audiobackendkind.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Pelican {

enum class AudioBus : std::uint8_t {
    Master = 0,
    Bgm = 1,
    Se = 2,
};

AudioBus audioBusFromName(std::string_view bus);
// The bus a voice plays on: "se" or "bgm". Master scales both and carries no
// voice of its own.
AudioBus audioVoiceBusFromName(std::string_view bus);

// AudioBackendKind and its names live in audiobackendkind.hpp, which every
// build has: the player reads a launch's --audio-backend with or without audio.
// The backend a launch that names none gets: the window decides.
AudioBackendKind preferredAudioBackend(bool headless) noexcept;

DECLARE_MODULE(Audio) {
  public:
    class Backend;
    struct PreparedSound {
        std::shared_ptr<const void> decoded;
        std::string source;
        AudioBus bus = AudioBus::Se;
    };
    // How a graph voice starts (AU-1 decision 2): where in the source, on
    // which bus, and whether the source loops at a sample-accurate loop point.
    struct VoiceStart {
        AudioBus bus = AudioBus::Se;
        double start = 0.0;              // source seconds
        bool loop = false;
        double loop_begin = 0.0;         // source seconds
        std::optional<double> loop_end;  // source seconds; the end of the source when absent
        // With loop == false, where the voice ends. The end of the source when absent.
        std::optional<double> end;
        float rate = 1.0f;               // multiplied into the voice's pitch
        bool paused = false;
    };
    // What a graph voice does this frame (AU-1 decision 3). With a position
    // the voice follows it: its cursor is compared with the position modulo
    // the source length (modulo the loop, for a voice on its own loop point)
    // and sought when the two are further apart than `tolerance`. Without one
    // the voice runs on its own clock.
    struct VoiceDrive {
        bool paused = false;
        std::optional<double> position;  // source seconds
        double tolerance = 0.0;          // seconds
        float rate = 1.0f;
        // A followed voice that ran to its end is sought back and restarted.
        // A voice on its own loop point that ended is left ended unless the
        // position is further than `tolerance` from it.
        bool restart_ended = true;
    };

  private:
    struct Voice {
        AudioBus bus = AudioBus::Se;
        std::uint64_t backend_id = 0;
        float volume = 1.0f;
        float pitch = 1.0f;
        float rate = 1.0f;
        // A graph voice belongs to the graph runtime that started it: it stays
        // when its source ends, because the graph decides when it goes.
        bool graph = false;
        bool held = false;
        bool volume_set = false;
        std::size_t seeks = 0;
        std::uint64_t length_frames = 0;
        std::uint64_t loop_frames = 0; // the loop's length when the source loops on its own
        std::uint32_t source_rate = 0;
    };
    struct DecodedEntry {
        std::optional<std::pair<std::int64_t, std::uintmax_t>> stamp;
        std::shared_ptr<const void> decoded;
        std::size_t bytes = 0;
    };

    std::unique_ptr<Backend> backend;
    // PM-1a: the whole mix held while the game is paused.
    bool all_paused = false;
    // Why the preferred backend did not open, when the null one stands in.
    std::string fallback_reason;
    std::unordered_map<SoundHandle, Voice, SoundHandle::Hash> voices;
    std::array<float, 3> bus_volumes{1.0f, 1.0f, 1.0f};
    bool voice_volumes_dirty = false;
    std::uint64_t next_handle = 1;
    // AU-1 decision 7: one decode per asset, shared by every voice that plays
    // it and by the rendering config's check that the asset decodes. A sound
    // longer than the streaming threshold (AU-2) is never decoded whole: its
    // entry holds the encoded bytes and the header, and each voice decodes as
    // it plays.
    mutable std::mutex decode_mutex;
    mutable std::map<std::string, DecodedEntry, std::less<>> decoded_sounds;
    mutable std::size_t decode_count = 0;
    std::size_t seek_count = 0;
    // AU-2 decision 3: longer sounds than this stream, a decoder per voice.
    double streaming_threshold_seconds;
    std::size_t streamed_voice_count = 0;

    float effectiveVolume(AudioBus bus) const;
    void updateVoiceVolumes();

  public:
    Audio();
    ~Audio();

    SoundHandle playSound(std::string_view path);
    SoundHandle playSound(std::string_view path, AudioBus bus);
    void stopSound(SoundHandle handle);
    void setBusVolume(std::string_view bus, float volume);
    float busVolume(std::string_view bus) const;
    bool isPlaying(SoundHandle handle) const;
    bool setSoundVolume(SoundHandle handle, float volume);
    // The same volume reached linearly over `seconds` of mixed audio rather
    // than at once (AU-1 decision 6: a graph writes one target per frame).
    bool rampSoundVolume(SoundHandle handle, float volume, double seconds);
    bool setSoundPitch(SoundHandle handle, float pitch);
    bool setSoundPaused(SoundHandle handle, bool paused);
    std::optional<SoundPlaybackState> soundState(SoundHandle handle) const;

    // Effect-transaction seam. Decoding and all engine-side capacity are
    // prepared before commit; backend calls remain in the fallible flush.
    PreparedSound prepareSound(std::string_view path, AudioBus bus = AudioBus::Se) const;
    static double lengthSeconds(const PreparedSound &sound);
    SoundHandle reserveSoundHandle();
    void reserveEffectVoices(std::size_t count);
    void prepareVoiceSlot(SoundHandle handle);
    void cancelPreparedVoice(SoundHandle handle) noexcept;
    void commitPreparedPlay(SoundHandle handle, const PreparedSound &sound);
    void commitPreparedStop(SoundHandle handle,
                            bool inject_backend_failure = false);
    void publishBusVolume(std::string_view bus, float volume) noexcept;
    void applyBusVolume();
    void commitBusVolume(std::string_view bus, float volume);
    std::uint64_t nextHandleForTesting() const noexcept { return next_handle; }

    // Graph voices (AU-1). The graph runtime starts one per audio.playback
    // node and drives it once per frame.
    SoundHandle playVoice(const PreparedSound &sound, const VoiceStart &start);
    void driveVoice(SoundHandle handle, const VoiceDrive &drive);
    // The source length of a graph voice in seconds; 0 when there is none.
    double voiceLengthSeconds(SoundHandle handle) const noexcept;

    // PM-1a. Holds (true) or resumes (false) the whole mix at once, graph
    // voices included: the device backend stops and starts its device, the
    // offline one mixes silence without moving any voice on, the null one has
    // nothing to hold. Each voice keeps its own paused state underneath.
    void setAllPaused(bool paused);
    bool allPaused() const noexcept { return all_paused; }
    // PM-2. Stops every voice at once, graph voices included, and forgets
    // their handles: a handle held by game code or a graph afterwards names
    // nothing (stopping it does nothing, it is not playing). The whole-mix
    // hold, the bus volumes and the decode cache stay.
    void stopAll();

    // Called at the shared logical-frame boundary. Completed backend voices
    // are destroyed here rather than accumulating until Audio teardown.
    void update();

    AudioBackendKind backendKind() const noexcept;
    const std::string &backendFallbackReason() const noexcept { return fallback_reason; }
    // The device-less engine only: mixes `frames` frames and returns them
    // interleaved at outputChannels() channels. Throws on any other backend.
    std::vector<float> mixOffline(std::uint64_t frames);
    std::uint32_t outputSampleRate() const noexcept;
    std::uint32_t outputChannels() const noexcept;

    std::size_t voiceCountForTesting() const noexcept;
    std::size_t backendVoiceCountForTesting() const noexcept;
    // Whole-sound decodes: a streamed sound is never counted here.
    std::size_t decodeCountForTesting() const noexcept;
    // Voices started on a decoder of their own (streamed sounds).
    std::size_t streamedVoiceCountForTesting() const noexcept { return streamed_voice_count; }
    // Sounds longer than `seconds` stream from then on; what the cache holds
    // is let go so each asset is read again under the new threshold.
    void setStreamingThresholdForTesting(double seconds);
    std::size_t seekCountForTesting() const noexcept { return seek_count; }
    std::size_t seekCountForTesting(SoundHandle handle) const noexcept;
};

} // namespace Pelican
