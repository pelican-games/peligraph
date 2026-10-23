#pragma once

#include "../container.hpp"
#include "../handle.hpp"
#include "../userpublic/audio.hpp"
#include "./audiobackendkind.hpp"

#include <array>
#include <chrono>
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
        // AT-1 decision 4: the voice the game clock follows (an audio.playback
        // with `master`). One plays at a time; the graph refuses a second.
        bool master = false;
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
        // AT-1 decision 4: false for a master voice that plays on from the
        // frame before: its cursor is not brought to the position however
        // far apart the two stand (the game clock follows the voice, not the
        // voice the game clock). A voice that resumes from being held, or
        // ran to its end and restarts, is still sought.
        bool correct_drift = true;
    };
    // AT-1 decision 1: where the audio clock comes from. The device's own
    // (the frames the device took, fitted against the steady clock), the
    // device-less engine's (the frames it mixed), or none -- the null backend
    // -- when the game clock stands in for it.
    enum class ClockSource : std::uint8_t { device, offline, game_clock };
    // The audio clock: the second of the sound being heard.
    struct ClockReading {
        ClockSource source = ClockSource::game_clock;
        std::uint32_t sample_rate = 0;
        double audio_time = 0.0;
        // The frames heard by then (audio_time at the sample rate).
        std::uint64_t played_frames = 0;
        // What the device holds before it is heard: an estimate (the device's
        // buffer), 0 where there is no device.
        double output_latency_seconds = 0.0;
        bool latency_estimated = false;
        // Since the engine started: the audio clock's progress less the
        // steady clock's (the device's crystal against the CPU's). 0 where
        // the audio follows the game clock.
        double drift_seconds = 0.0;
    };
    // What the latency estimate of a device was read from (AT-1 decision 1).
    struct DeviceLatency {
        double seconds = 0.0;
        std::uint32_t period_frames = 0;    // internalPeriodSizeInFrames
        std::uint32_t periods = 0;          // internalPeriods
        std::uint32_t buffer_frames = 0;    // WASAPI's actualBufferSizeInFramesPlayback; 0 elsewhere
        std::uint32_t device_rate = 0;      // the device's own rate, the frames above are at it
        bool from_buffer = false;           // buffer_frames was used rather than period_frames * periods
    };
    // The frame's head, as updateFrameState hands it over (AT-1 decision 2).
    struct FrameClock {
        bool game_step = false;
        double now = 0.0;                   // the game clock after the step
        double dt = 0.0;                    // the step's length
        std::uint64_t time_set_revision = 0;
    };
    // AT-1 decision 6: where a graph voice is heard now -- its cursor less
    // the output latency, at the voice's rate (and, on the device-less
    // engine, less what the voice read ahead of its mix) -- and how far that
    // stands from the source second the graph asks for, taken modulo the
    // loop (or the source).
    struct HeardPosition {
        double heard_seconds = 0.0;
        double offset_seconds = 0.0;
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
        std::uint64_t loop_begin_frames = 0;
        std::uint32_t source_rate = 0;
        bool master = false;
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
    // AT-1 decision 2: the device-less engine's clock is the frames it mixed.
    // A game step mixes it up to the game clock from where the clock stood
    // when the mix last took it up (the base); a jump of the game clock
    // (set_time, a play session's start) or a stretch of held frames moves
    // the base rather than the mix.
    std::uint64_t offline_mixed_frames = 0;
    struct OfflineBase {
        std::uint64_t time_set_revision = 0;
        double time = 0.0;
        std::uint64_t frames = 0;
    };
    std::optional<OfflineBase> offline_base;
    // AT-1 decision 1: the device's clock as the frame's head read it.
    std::optional<ClockReading> device_reading;
    bool device_read_this_frame = false;

    float effectiveVolume(AudioBus bus) const;
    void updateVoiceVolumes();
    void followGameClock(const FrameClock &frame);

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

    // AT-1. The frame's head, after update(): a game step mixes the
    // device-less engine up to the game clock (decision 2: the frames up to
    // floor(now * rate), nothing while the whole mix is held), and a device
    // whose clock EngineTime::advance did not read this frame reads it now.
    void beginFrame(const FrameClock &frame);
    // AT-1 decision 1. The audio clock: the device's as this frame's head
    // read it; the device-less engine's, exact; on the null backend the game
    // clock, `game_time`, stands in for it.
    ClockReading clock(double game_time) const;
    // AT-1 decision 5, asked by EngineTime::advance at its tick: the device's
    // clock there (read once, kept as the frame's) and whether a master
    // voice plays. None where the audio follows the game clock instead
    // (offline, null): there is nothing to follow.
    struct FollowedClock {
        double audio_time = 0.0;
        bool master_playing = false;
    };
    std::optional<FollowedClock> followedClockAt(std::chrono::steady_clock::time_point tick);
    // A master voice is playing: started, not held, not at its end, and the
    // whole mix not held.
    bool masterPlaying() const;
    std::optional<HeardPosition> heardPosition(SoundHandle handle, double position_seconds) const;
    std::optional<DeviceLatency> deviceLatency() const;

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
    // Every frame the device-less engine mixed, whoever asked for it.
    std::uint64_t mixedFramesForTesting() const noexcept { return offline_mixed_frames; }
};

} // namespace Pelican
