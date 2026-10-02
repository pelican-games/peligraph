#include "audio.hpp"

#include "../launchconfig.hpp"
#include "../loader/pathresolver.hpp"
#include "../log.hpp"
#include "../../project/graphcomposition.hpp"

// AU-2: Ogg Vorbis through the stb_vorbis that ships with miniaudio. Its
// declarations come before miniaudio so miniaudio compiles its Vorbis
// decoding backend; its implementation is included at the end of this file,
// after miniaudio's, as miniaudio's own tools do.
#define STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

// The formats, the WAV rules and the decoders (AB-1 moved them out so the CLI
// decodes an asset the same way).
#include "sounddecode.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Pelican {

using namespace AudioDecode;

namespace {

constexpr std::size_t busIndex(AudioBus bus) {
    return static_cast<std::size_t>(bus);
}


// AU-2 decision 3: a sound longer than this is streamed rather than decoded.
constexpr double defaultStreamingThresholdSeconds = 10.0;

// The device-less engine's format. Fixed so what a test reads back does not
// depend on the machine it runs on.
constexpr std::uint32_t offlineSampleRate = 48000;
constexpr std::uint32_t offlineChannels = 2;
// Above this many decoded bytes the cache lets go of the sounds no voice holds.
constexpr std::size_t decodedCacheBudget = 256u * 1024u * 1024u;

std::uint64_t secondsToFrames(double seconds, std::uint32_t sample_rate, std::uint64_t length) {
    if (!std::isfinite(seconds) || seconds <= 0.0) return 0;
    const auto frames = std::llround(seconds * static_cast<double>(sample_rate));
    return (std::min)(static_cast<std::uint64_t>((std::max)(frames, 0LL)), length);
}

} // namespace

AudioBus audioBusFromName(std::string_view bus) {
    if (bus == "master") {
        return AudioBus::Master;
    }
    if (bus == "bgm") {
        return AudioBus::Bgm;
    }
    if (bus == "se") {
        return AudioBus::Se;
    }
    throw std::runtime_error("Unknown audio bus: " + std::string{bus});
}

AudioBus audioVoiceBusFromName(std::string_view bus) {
    if (bus == "bgm") return AudioBus::Bgm;
    if (bus == "se") return AudioBus::Se;
    throw std::runtime_error("Unknown audio bus: " + std::string{bus} + " (a voice plays on se or bgm)");
}

AudioBackendKind preferredAudioBackend(bool headless) noexcept {
    return headless ? AudioBackendKind::offline : AudioBackendKind::device;
}

class Audio::Backend {
  public:
    struct Setup {
        std::uint64_t start = 0;       // source frames
        bool loop = false;
        std::uint64_t loop_begin = 0;
        std::uint64_t loop_end = 0;    // exclusive; 0 = the end of the source
        std::uint64_t end = 0;         // exclusive; 0 = the end of the source
        bool paused = false;
    };
    virtual ~Backend() = default;
    virtual AudioBackendKind kind() const noexcept = 0;
    virtual std::uint64_t play(std::shared_ptr<const DecodedSound> sound, float gain, const Setup &setup) = 0;
    virtual void destroy(std::uint64_t voice_id) = 0;
    virtual bool isFinished(std::uint64_t voice_id) const = 0;
    virtual bool isPlaying(std::uint64_t voice_id) const = 0;
    // The bus and master volume. Applied at once.
    virtual void setGain(std::uint64_t voice_id, float gain) = 0;
    // The voice's own volume, reached linearly over `ramp_seconds` of mixed audio.
    virtual void setVolume(std::uint64_t voice_id, float volume, double ramp_seconds) = 0;
    virtual void setPitch(std::uint64_t voice_id, float pitch) = 0;
    // false starts a voice that is not playing, including one that ran to its end.
    virtual void setPaused(std::uint64_t voice_id, bool paused) = 0;
    // The source cursor in frames; none where there is no clock.
    virtual std::optional<std::uint64_t> cursor(std::uint64_t voice_id) const = 0;
    virtual void seek(std::uint64_t voice_id, std::uint64_t frame) = 0;
    virtual SoundPlaybackState state(std::uint64_t voice_id) const = 0;
    virtual std::size_t voiceCount() const noexcept = 0;
    virtual std::vector<float> mix(std::uint64_t frames) {
        (void)frames;
        throw std::logic_error("audio backend '" + std::string{audioBackendKindName(kind())} +
                               "' has no offline mix");
    }
    virtual std::uint32_t sampleRate() const noexcept { return 0; }
    virtual std::uint32_t channels() const noexcept { return 0; }
    // PM-1a: the whole mix held or resumed. Nothing to hold by default (null).
    virtual void setAllPaused(bool paused) { (void)paused; }
    // PM-2: every voice stopped and let go at once.
    virtual void stopAll() = 0;
};

class NullAudioBackend final : public Audio::Backend {
    std::uint64_t next_voice = 1;
    std::unordered_map<std::uint64_t, SoundPlaybackState> playing;

  public:
    AudioBackendKind kind() const noexcept override { return AudioBackendKind::null; }

    std::uint64_t play(std::shared_ptr<const DecodedSound> sound, float gain, const Setup &setup) override {
        (void)sound;
        (void)gain;
        const auto id = next_voice++;
        playing.emplace(id, SoundPlaybackState{.playing = !setup.paused, .paused = setup.paused});
        return id;
    }

    void destroy(std::uint64_t voice_id) override {
        playing.erase(voice_id);
    }

    bool isFinished(std::uint64_t voice_id) const override {
        return !playing.contains(voice_id);
    }

    bool isPlaying(std::uint64_t voice_id) const override {
        const auto it = playing.find(voice_id);
        return it != playing.end() && it->second.playing;
    }

    void setGain(std::uint64_t voice_id, float gain) override {
        (void)playing.at(voice_id);
        (void)gain;
    }
    void setVolume(std::uint64_t voice_id, float volume, double ramp_seconds) override {
        (void)playing.at(voice_id);
        (void)volume;
        (void)ramp_seconds;
    }

    void setPitch(std::uint64_t voice_id, float pitch) override {
        playing.at(voice_id).pitch = pitch;
    }
    void setPaused(std::uint64_t voice_id, bool paused) override {
        auto &state = playing.at(voice_id);
        state.paused = paused;
        state.playing = !paused;
    }
    std::optional<std::uint64_t> cursor(std::uint64_t voice_id) const override {
        (void)playing.at(voice_id);
        return std::nullopt;
    }
    void seek(std::uint64_t voice_id, std::uint64_t frame) override {
        (void)playing.at(voice_id);
        (void)frame;
    }
    SoundPlaybackState state(std::uint64_t voice_id) const override {
        return playing.at(voice_id);
    }

    std::size_t voiceCount() const noexcept override {
        return playing.size();
    }

    void stopAll() override { playing.clear(); }
};

class MiniaudioBackend final : public Audio::Backend {
    struct MiniaudioVoice {
        std::shared_ptr<const DecodedSound> decoded;
        ma_audio_buffer_ref buffer{};
        // A streamed sound (AU-2): this voice's own decoder over the shared
        // encoded bytes, read by the mixer as it plays.
        ma_decoder decoder{};
        ma_sound sound{};
        bool buffer_initialized = false;
        bool decoder_initialized = false;
        bool sound_initialized = false;
        bool paused = false;

        MiniaudioVoice() = default;
        MiniaudioVoice(const MiniaudioVoice &) = delete;
        MiniaudioVoice &operator=(const MiniaudioVoice &) = delete;

        ~MiniaudioVoice() {
            if (sound_initialized) {
                ma_sound_stop(&sound);
                ma_sound_uninit(&sound);
            }
            if (decoder_initialized) {
                ma_decoder_uninit(&decoder);
            }
            if (buffer_initialized) {
                ma_audio_buffer_ref_uninit(&buffer);
            }
        }

        ma_data_source *source() {
            return decoder_initialized ? static_cast<ma_data_source *>(&decoder)
                                       : static_cast<ma_data_source *>(&buffer);
        }
    };

    AudioBackendKind kind_;
    ma_engine engine{};
    bool engine_initialized = false;
    std::uint64_t next_voice = 1;
    std::unordered_map<std::uint64_t, std::unique_ptr<MiniaudioVoice>> voices;
    bool all_paused_ = false;

    MiniaudioVoice &voice(std::uint64_t voice_id) const {
        return *voices.at(voice_id);
    }

  public:
    explicit MiniaudioBackend(AudioBackendKind kind) : kind_{kind} {
        auto config = ma_engine_config_init();
        if (kind == AudioBackendKind::offline) {
            config.noDevice = MA_TRUE;
            config.channels = offlineChannels;
            config.sampleRate = offlineSampleRate;
        }
        const auto result = ma_engine_init(&config, &engine);
        if (result != MA_SUCCESS) {
            throw std::runtime_error(maErrorMessage("ma_engine_init", result));
        }
        engine_initialized = true;
    }

    ~MiniaudioBackend() override {
        voices.clear();
        if (engine_initialized) {
            ma_engine_uninit(&engine);
        }
    }

    AudioBackendKind kind() const noexcept override { return kind_; }

    std::uint64_t play(std::shared_ptr<const DecodedSound> sound, float gain, const Setup &setup) override {
        auto voice = std::make_unique<MiniaudioVoice>();
        voice->decoded = std::move(sound);
        const auto length = voice->decoded->frameCount();

        ma_result result = MA_SUCCESS;
        if (voice->decoded->streamed()) {
            // AU-2 decision 3: a decoder per voice, so each has its own cursor
            // over the one copy of the encoded bytes.
            const auto &bytes = *voice->decoded->encoded;
            const auto config = decoderConfig(voice->decoded->format);
            result = ma_decoder_init_memory(bytes.data(), bytes.size(), &config, &voice->decoder);
            if (result != MA_SUCCESS) {
                throw std::runtime_error(maErrorMessage("ma_decoder_init_memory", result));
            }
            voice->decoder_initialized = true;
        } else {
            // The decoded samples are shared between voices; each voice reads them
            // through its own reference with its own cursor, range and loop point.
            result = ma_audio_buffer_ref_init(ma_format_f32, voice->decoded->channels,
                                              voice->decoded->samples.data(), length, &voice->buffer);
            if (result != MA_SUCCESS) {
                throw std::runtime_error(maErrorMessage("ma_audio_buffer_ref_init", result));
            }
            voice->buffer.sampleRate = voice->decoded->sample_rate;
            voice->buffer_initialized = true;
        }
        // Range, loop point and start are the data source's, streamed or not:
        // the same frames mean the same place in either.
        auto *source = voice->source();

        if (setup.end != 0 && setup.end < length) {
            result = ma_data_source_set_range_in_pcm_frames(source, 0, setup.end);
            if (result != MA_SUCCESS)
                throw std::runtime_error(maErrorMessage("ma_data_source_set_range_in_pcm_frames", result));
        }
        if (setup.loop) {
            // AU-1 decision 2: the loop is the data source's own loop point, so
            // the frame after loop_end - 1 is loop_begin with no seek between.
            const auto loop_end = setup.loop_end == 0 ? length : (std::min)(setup.loop_end, length);
            result = ma_data_source_set_loop_point_in_pcm_frames(source, setup.loop_begin, loop_end);
            if (result != MA_SUCCESS)
                throw std::runtime_error(maErrorMessage("ma_data_source_set_loop_point_in_pcm_frames", result));
            result = ma_data_source_set_looping(source, MA_TRUE);
            if (result != MA_SUCCESS)
                throw std::runtime_error(maErrorMessage("ma_data_source_set_looping", result));
        }
        if (setup.start != 0) {
            result = ma_data_source_seek_to_pcm_frame(source, (std::min)(setup.start, length));
            if (result != MA_SUCCESS)
                throw std::runtime_error(maErrorMessage("ma_data_source_seek_to_pcm_frame", result));
        }

        // The sound applies its own looping flag to the data source, so the
        // loop is asked for here as well as on the buffer.
        const ma_uint32 flags = MA_SOUND_FLAG_NO_SPATIALIZATION | (setup.loop ? MA_SOUND_FLAG_LOOPING : 0u);
        result = ma_sound_init_from_data_source(&engine,
                                                source,
                                                flags,
                                                nullptr,
                                                &voice->sound);
        if (result != MA_SUCCESS) {
            throw std::runtime_error(maErrorMessage("ma_sound_init_from_data_source", result));
        }
        voice->sound_initialized = true;

        ma_sound_set_volume(&voice->sound, gain);
        voice->paused = setup.paused;
        if (!setup.paused) {
            result = ma_sound_start(&voice->sound);
            if (result != MA_SUCCESS) {
                throw std::runtime_error(maErrorMessage("ma_sound_start", result));
            }
        }

        const auto id = next_voice++;
        voices.emplace(id, std::move(voice));
        return id;
    }

    void destroy(std::uint64_t voice_id) override {
        voices.erase(voice_id);
    }

    bool isFinished(std::uint64_t voice_id) const override {
        const auto it = voices.find(voice_id);
        return it == voices.end() ||
               ma_sound_at_end(&it->second->sound) == MA_TRUE;
    }

    bool isPlaying(std::uint64_t voice_id) const override {
        const auto it = voices.find(voice_id);
        return it != voices.end() && ma_sound_is_playing(&it->second->sound) == MA_TRUE;
    }

    void setGain(std::uint64_t voice_id, float gain) override {
        const auto it = voices.find(voice_id);
        if (it != voices.end()) {
            ma_sound_set_volume(&it->second->sound, gain);
        }
    }

    void setVolume(std::uint64_t voice_id, float volume, double ramp_seconds) override {
        auto &target = voice(voice_id);
        // The fader runs at the engine's rate. A ramp of zero frames lands on
        // the value at once; a positive one starts from wherever the fader is.
        const auto frames = ramp_seconds > 0.0
            ? static_cast<ma_uint64>(std::llround(ramp_seconds * ma_engine_get_sample_rate(&engine)))
            : ma_uint64{0};
        ma_sound_set_fade_in_pcm_frames(&target.sound, frames == 0 ? volume : -1.0f, volume, frames);
    }

    void setPitch(std::uint64_t voice_id, float pitch) override {
        ma_sound_set_pitch(&voice(voice_id).sound, pitch);
    }
    void setPaused(std::uint64_t voice_id, bool paused) override {
        auto &target = voice(voice_id);
        if (paused) {
            if (!target.paused || ma_sound_is_playing(&target.sound) == MA_TRUE) {
                const auto result = ma_sound_stop(&target.sound);
                if (result != MA_SUCCESS) throw std::runtime_error(maErrorMessage("ma_sound_stop", result));
            }
        } else if (ma_sound_is_playing(&target.sound) != MA_TRUE) {
            const auto result = ma_sound_start(&target.sound);
            if (result != MA_SUCCESS) throw std::runtime_error(maErrorMessage("ma_sound_start", result));
        }
        target.paused = paused;
    }

    std::optional<std::uint64_t> cursor(std::uint64_t voice_id) const override {
        ma_uint64 frames{};
        if (ma_sound_get_cursor_in_pcm_frames(&voice(voice_id).sound, &frames) != MA_SUCCESS)
            return std::nullopt;
        return frames;
    }

    void seek(std::uint64_t voice_id, std::uint64_t frame) override {
        auto &target = voice(voice_id);
        const auto result = ma_sound_seek_to_pcm_frame(&target.sound, (std::min)(frame, target.decoded->frameCount()));
        if (result != MA_SUCCESS) throw std::runtime_error(maErrorMessage("ma_sound_seek_to_pcm_frame", result));
    }

    SoundPlaybackState state(std::uint64_t voice_id) const override {
        auto &target = voice(voice_id);
        SoundPlaybackState result;
        result.pitch = ma_sound_get_pitch(&target.sound);
        result.playing = ma_sound_is_playing(&target.sound) == MA_TRUE;
        result.paused = target.paused;
        if (const auto frames = cursor(voice_id))
            result.position_seconds = static_cast<double>(*frames) / target.decoded->sample_rate;
        return result;
    }

    std::size_t voiceCount() const noexcept override {
        return voices.size();
    }

    // Each voice stops and uninitializes as it goes (~MiniaudioVoice). The
    // engine itself, and whether it is held, stay as they are.
    void stopAll() override { voices.clear(); }

    void setAllPaused(bool paused) override {
        if (paused == all_paused_) return;
        all_paused_ = paused;
        // Offline, the mix itself is the clock: mix() answers silence and
        // reads nothing while held. On a device the device is stopped.
        if (kind_ == AudioBackendKind::offline) return;
        const auto result = paused ? ma_engine_stop(&engine) : ma_engine_start(&engine);
        if (result != MA_SUCCESS)
            LOG_WARNING(logger, "audio: {}",
                        maErrorMessage(paused ? "ma_engine_stop" : "ma_engine_start", result));
    }

    std::vector<float> mix(std::uint64_t frames) override {
        if (kind_ != AudioBackendKind::offline) return Backend::mix(frames);
        std::vector<float> samples(static_cast<std::size_t>(frames) * offlineChannels);
        // Held: silence, and no voice moves on.
        if (all_paused_) return samples;
        ma_uint64 read = 0;
        const auto result = ma_engine_read_pcm_frames(&engine, samples.data(), frames, &read);
        // Nothing playing reads as nothing: the frames are silence, not missing.
        if (result != MA_SUCCESS && result != MA_AT_END)
            throw std::runtime_error(maErrorMessage("ma_engine_read_pcm_frames", result));
        return samples;
    }
    std::uint32_t sampleRate() const noexcept override { return ma_engine_get_sample_rate(&engine); }
    std::uint32_t channels() const noexcept override { return ma_engine_get_channels(&engine); }
};

Audio::Audio() : streaming_threshold_seconds{defaultStreamingThresholdSeconds} {
    // AU-3: the backend the launch named, else the one the window decides.
    const auto &launch = GET_MODULE(EngineLaunchConfig);
    const auto preferred = launch.audio_backend.value_or(preferredAudioBackend(launch.headless));
    if (preferred == AudioBackendKind::null) {
        // Named, not fallen back to: nothing opens, so there is no reason to
        // report beyond the backend itself.
        backend = std::make_unique<NullAudioBackend>();
        LOG_INFO(logger, "audio backend: null (named at launch)");
        return;
    }
    try {
        backend = std::make_unique<MiniaudioBackend>(preferred);
        LOG_INFO(logger, "audio backend: {}", audioBackendKindName(preferred));
    } catch (const std::exception &error) {
        LOG_WARNING(logger, "audio {} backend unavailable; falling back to null: {}",
                    audioBackendKindName(preferred), error.what());
        fallback_reason = std::string{audioBackendKindName(preferred)} + ": " + error.what();
        backend = std::make_unique<NullAudioBackend>();
    }
}

Audio::~Audio() = default;

float Audio::effectiveVolume(AudioBus bus) const {
    const auto master = bus_volumes[busIndex(AudioBus::Master)];
    if (bus == AudioBus::Master) {
        return master;
    }
    return master * bus_volumes[busIndex(bus)];
}

void Audio::updateVoiceVolumes() {
    for (const auto &[handle, voice] : voices) {
        (void)handle;
        if (voice.backend_id != 0)
            backend->setGain(voice.backend_id, effectiveVolume(voice.bus));
    }
}

SoundHandle Audio::playSound(std::string_view path) {
    return playSound(path, AudioBus::Se);
}

SoundHandle Audio::playSound(std::string_view path, AudioBus bus) {
    auto prepared = prepareSound(path, bus);
    const auto handle = reserveSoundHandle();
    prepareVoiceSlot(handle);
    try {
        commitPreparedPlay(handle, prepared);
    } catch (...) {
        cancelPreparedVoice(handle);
        throw;
    }
    return handle;
}

Audio::PreparedSound Audio::prepareSound(std::string_view path, AudioBus bus) const {
    if (bus == AudioBus::Master) throw std::runtime_error("Unknown audio bus: master (a voice plays on se or bgm)");
    auto &resolver = GET_MODULE(PathResolver);
    // The key is the asset's normalized reference; a file on disk also carries
    // its write time and size, so an asset edited in place decodes again.
    std::string key{path};
    std::optional<std::pair<std::int64_t, std::uintmax_t>> stamp;
    try {
        key = resolver.normalizedReference(path);
        const auto file = resolver.resolveExistingFile(path);
        stamp = std::pair{static_cast<std::int64_t>(std::filesystem::last_write_time(file).time_since_epoch().count()),
                          std::filesystem::file_size(file)};
    } catch (const std::exception &) {
        // Embedded or missing: loadBytes below reads it or says why it cannot.
    }
    {
        std::lock_guard lock{decode_mutex};
        const auto found = decoded_sounds.find(key);
        if (found != decoded_sounds.end() && found->second.stamp == stamp)
            return PreparedSound{found->second.decoded, std::string{path}, bus};
    }
    auto bytes = resolver.loadBytes(path);
    const std::span<const std::byte> view{bytes.data(), bytes.size()};
    const auto format = detectSoundFormat(path, view);
    // AU-2 decision 3. The header says how long the sound is; a long one is
    // not decoded here -- not for the rendering config's check, not for a
    // voice -- and keeps only its encoded bytes, which each voice's decoder
    // reads as it plays.
    auto header = readSoundHeader(format, view);
    const bool streamed = static_cast<double>(header.streamed_frames) >
                          streaming_threshold_seconds * static_cast<double>(header.sample_rate);
    std::shared_ptr<const DecodedSound> decoded;
    if (streamed) {
        header.encoded = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
        decoded = std::make_shared<const DecodedSound>(std::move(header));
    } else {
        decoded = std::make_shared<const DecodedSound>(format == SoundFormat::ogg ? decodeOgg(view, std::move(header)) : decodeWav(view));
    }
    const auto size = decoded->residentBytes();
    std::lock_guard lock{decode_mutex};
    if (!streamed) ++decode_count;
    std::size_t total = size;
    for (const auto &[name, entry] : decoded_sounds)
        if (name != key) total += entry.bytes;
    // Over budget, let go of what no voice holds; a later play decodes again.
    for (auto it = decoded_sounds.begin(); total > decodedCacheBudget && it != decoded_sounds.end();) {
        if (it->first != key && it->second.decoded.use_count() == 1) {
            total -= it->second.bytes;
            it = decoded_sounds.erase(it);
        } else ++it;
    }
    decoded_sounds.insert_or_assign(key, DecodedEntry{stamp, decoded, size});
    return PreparedSound{std::move(decoded), std::string{path}, bus};
}

void Audio::setStreamingThresholdForTesting(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0)
        throw std::invalid_argument("audio streaming threshold must be finite and non-negative");
    std::lock_guard lock{decode_mutex};
    streaming_threshold_seconds = seconds;
    // Voices keep what they play; the cache forgets, so the next prepare reads
    // each asset again and decides by the new threshold.
    decoded_sounds.clear();
}

double Audio::lengthSeconds(const PreparedSound &sound) {
    const auto decoded = std::static_pointer_cast<const DecodedSound>(sound.decoded);
    if (!decoded || decoded->sample_rate == 0) return 0.0;
    return static_cast<double>(decoded->frameCount()) / decoded->sample_rate;
}

SoundHandle Audio::reserveSoundHandle() {
    if (next_handle == 0 ||
        next_handle == (std::numeric_limits<std::uint64_t>::max)()) {
        throw std::overflow_error("Audio sound handle space exhausted");
    }
    return SoundHandle{next_handle++};
}

void Audio::reserveEffectVoices(std::size_t count) {
    voices.reserve(voices.size() + count);
}

void Audio::prepareVoiceSlot(SoundHandle handle) {
    const auto [it, inserted] =
        voices.emplace(handle, Voice{AudioBus::Se, 0});
    (void)it;
    if (!inserted) {
        throw std::runtime_error("Audio sound handle space exhausted");
    }
}

void Audio::cancelPreparedVoice(SoundHandle handle) noexcept {
    const auto found = voices.find(handle);
    if (found != voices.end() && found->second.backend_id == 0) {
        voices.erase(found);
    }
}

void Audio::commitPreparedPlay(SoundHandle handle,
                               const PreparedSound &sound) {
    auto decoded = std::static_pointer_cast<const DecodedSound>(
        sound.decoded);
    const auto backend_id = backend->play(decoded, effectiveVolume(sound.bus), {});
    const auto slot = voices.find(handle);
    if (slot == voices.end() || slot->second.backend_id != 0) {
        backend->destroy(backend_id);
        throw std::logic_error("prepared audio voice slot is missing");
    }
    // The unordered-map node was allocated while staging. Ownership transfer
    // after the backend call is only this non-throwing scalar publication.
    slot->second.backend_id = backend_id;
    slot->second.bus = sound.bus;
    if (decoded->streamed()) ++streamed_voice_count;
}

void Audio::commitPreparedStop(SoundHandle handle,
                               bool inject_backend_failure) {
    const auto it = voices.find(handle);
    if (it == voices.end()) return;
    const auto backend_id = it->second.backend_id;
    voices.erase(it);
    if (inject_backend_failure) {
        LOG_ERROR(logger,
                  "audio_stop_failed@1: backend voice {} may remain after handle {} was erased",
                  backend_id, handle.value);
        throw std::runtime_error("injected audio stop failure");
    }
    try {
        backend->destroy(backend_id);
    } catch (...) {
        LOG_ERROR(logger,
                  "audio_stop_failed@1: backend voice {} may remain after handle {} was erased",
                  backend_id, handle.value);
        throw;
    }
}

void Audio::commitBusVolume(std::string_view bus, float volume) {
    (void)audioBusFromName(bus);
    publishBusVolume(bus, volume);
    applyBusVolume();
}

void Audio::publishBusVolume(std::string_view bus, float volume) noexcept {
    const auto parsed = bus == "master" ? AudioBus::Master
                      : bus == "bgm" ? AudioBus::Bgm
                                     : AudioBus::Se;
    bus_volumes[busIndex(parsed)] = volume;
    voice_volumes_dirty = true;
}

void Audio::applyBusVolume() {
    updateVoiceVolumes();
    voice_volumes_dirty = false;
}

void Audio::stopSound(SoundHandle handle) {
    commitPreparedStop(handle);
}

void Audio::setBusVolume(std::string_view bus, float volume) {
    if (!std::isfinite(volume) || volume < 0.0f) {
        throw std::runtime_error("Audio bus volume must be a finite non-negative number");
    }
    commitBusVolume(bus, volume);
}

float Audio::busVolume(std::string_view bus) const {
    return bus_volumes[busIndex(audioBusFromName(bus))];
}

bool Audio::isPlaying(SoundHandle handle) const {
    const auto it = voices.find(handle);
    return it != voices.end() && it->second.backend_id != 0 &&
           backend->isPlaying(it->second.backend_id);
}

bool Audio::setSoundVolume(SoundHandle handle, float volume) {
    return rampSoundVolume(handle, volume, 0.0);
}

bool Audio::rampSoundVolume(SoundHandle handle, float volume, double seconds) {
    if (!std::isfinite(volume) || volume < 0.0f)
        throw std::invalid_argument("Audio voice volume must be finite and non-negative");
    const auto found = voices.find(handle);
    if (found == voices.end() || found->second.backend_id == 0) return false;
    // A voice's first volume is where it starts, not a ramp from full.
    const auto ramp = found->second.volume_set && std::isfinite(seconds) ? seconds : 0.0;
    backend->setVolume(found->second.backend_id, volume, ramp);
    found->second.volume = volume;
    found->second.volume_set = true;
    return true;
}

bool Audio::setSoundPitch(SoundHandle handle, float pitch) {
    if (!std::isfinite(pitch) || pitch <= 0.0f)
        throw std::invalid_argument("Audio voice pitch must be finite and positive");
    const auto found = voices.find(handle);
    if (found == voices.end() || found->second.backend_id == 0) return false;
    backend->setPitch(found->second.backend_id, pitch * found->second.rate);
    found->second.pitch = pitch;
    return true;
}

std::optional<SoundPlaybackState> Audio::soundState(SoundHandle handle) const {
    const auto found = voices.find(handle);
    if (found == voices.end() || found->second.backend_id == 0) return std::nullopt;
    auto result = backend->state(found->second.backend_id);
    result.volume = found->second.volume;
    result.pitch = found->second.pitch;
    result.effective_volume = found->second.volume * effectiveVolume(found->second.bus);
    return result;
}

bool Audio::setSoundPaused(SoundHandle handle, bool paused) {
    const auto found = voices.find(handle);
    if (found == voices.end() || found->second.backend_id == 0 || backend->isFinished(found->second.backend_id))
        return false;
    backend->setPaused(found->second.backend_id, paused);
    return true;
}

SoundHandle Audio::playVoice(const PreparedSound &sound, const VoiceStart &start) {
    auto decoded = std::static_pointer_cast<const DecodedSound>(sound.decoded);
    if (!decoded) throw std::invalid_argument("graph voice requires a decoded sound");
    if (start.bus == AudioBus::Master) throw std::runtime_error("Unknown audio bus: master (a voice plays on se or bgm)");
    if (!std::isfinite(start.rate) || start.rate <= 0.0f)
        throw std::invalid_argument("Audio voice rate must be finite and positive");
    const auto length = decoded->frameCount();
    const auto rate = decoded->sample_rate;
    Backend::Setup setup;
    setup.start = secondsToFrames(start.start, rate, length);
    setup.loop = start.loop;
    setup.loop_begin = secondsToFrames(start.loop_begin, rate, length);
    setup.loop_end = start.loop_end ? secondsToFrames(*start.loop_end, rate, length) : length;
    if (setup.loop && setup.loop_end <= setup.loop_begin)
        throw std::invalid_argument("audio voice loop is empty");
    setup.end = start.end ? secondsToFrames(*start.end, rate, length) : 0;
    setup.paused = start.paused;
    const auto handle = reserveSoundHandle();
    prepareVoiceSlot(handle);
    std::uint64_t backend_id = 0;
    try {
        backend_id = backend->play(decoded, effectiveVolume(start.bus), setup);
        if (start.rate != 1.0f) backend->setPitch(backend_id, start.rate);
    } catch (...) {
        if (backend_id != 0) backend->destroy(backend_id);
        cancelPreparedVoice(handle);
        throw;
    }
    auto &voice = voices.at(handle);
    voice.backend_id = backend_id;
    voice.bus = start.bus;
    voice.rate = start.rate;
    voice.graph = true;
    voice.length_frames = length;
    voice.loop_frames = setup.loop ? setup.loop_end - setup.loop_begin : 0;
    voice.source_rate = rate;
    voice.held = start.paused;
    if (decoded->streamed()) ++streamed_voice_count;
    return handle;
}

void Audio::driveVoice(SoundHandle handle, const VoiceDrive &drive) {
    const auto found = voices.find(handle);
    if (found == voices.end() || found->second.backend_id == 0) return;
    auto &voice = found->second;
    const auto id = voice.backend_id;
    if (std::isfinite(drive.rate) && drive.rate > 0.0f && drive.rate != voice.rate) {
        voice.rate = drive.rate;
        backend->setPitch(id, voice.pitch * voice.rate);
    }
    if (drive.paused) {
        if (!voice.held) backend->setPaused(id, true);
        voice.held = true;
        return;
    }
    bool start = voice.held;
    if (drive.position && voice.length_frames > 0 && voice.source_rate > 0) {
        if (const auto cursor = backend->cursor(id)) {
            // Compared modulo the source length: a position that wrapped and a
            // cursor that looped are the same place.
            const auto length = static_cast<double>(voice.length_frames);
            auto target = std::fmod(*drive.position * voice.source_rate, length);
            if (target < 0.0) target += length;
            // A voice on its own loop point is compared modulo the loop: the
            // loop's end and its beginning are the same place.
            const auto period = voice.loop_frames != 0 ? static_cast<double>(voice.loop_frames) : length;
            auto difference = std::fmod(target - static_cast<double>(*cursor), period);
            if (difference > period / 2.0) difference -= period;
            else if (difference < -period / 2.0) difference += period;
            const bool ended = backend->isFinished(id);
            const bool resume = ended && drive.restart_ended;
            const bool apart = std::abs(difference) > drive.tolerance * voice.source_rate;
            if (resume || apart) {
                backend->seek(id, (std::min)(static_cast<std::uint64_t>(target), voice.length_frames - 1));
                ++voice.seeks;
                ++seek_count;
            }
            if (resume || apart || (!ended && !backend->isPlaying(id))) start = true;
        }
    }
    if (start) backend->setPaused(id, false);
    voice.held = false;
}

double Audio::voiceLengthSeconds(SoundHandle handle) const noexcept {
    const auto found = voices.find(handle);
    if (found == voices.end() || found->second.source_rate == 0) return 0.0;
    return static_cast<double>(found->second.length_frames) / found->second.source_rate;
}

void Audio::update() {
    if (voice_volumes_dirty) applyBusVolume();
    for (auto it = voices.begin(); it != voices.end();) {
        if (it->second.graph || !backend->isFinished(it->second.backend_id)) {
            ++it;
            continue;
        }

        backend->destroy(it->second.backend_id);
        it = voices.erase(it);
    }
}

AudioBackendKind Audio::backendKind() const noexcept {
    return backend->kind();
}

void Audio::setAllPaused(bool paused) {
    if (paused == all_paused) return;
    backend->setAllPaused(paused);
    all_paused = paused;
}

void Audio::stopAll() {
    backend->stopAll();
    voices.clear();
}

std::vector<float> Audio::mixOffline(std::uint64_t frames) {
    return backend->mix(frames);
}

std::uint32_t Audio::outputSampleRate() const noexcept {
    return backend->sampleRate();
}

std::uint32_t Audio::outputChannels() const noexcept {
    return backend->channels();
}

std::size_t Audio::voiceCountForTesting() const noexcept {
    return voices.size();
}

std::size_t Audio::backendVoiceCountForTesting() const noexcept {
    return backend->voiceCount();
}

std::size_t Audio::decodeCountForTesting() const noexcept {
    std::lock_guard lock{decode_mutex};
    return decode_count;
}

std::size_t Audio::seekCountForTesting(SoundHandle handle) const noexcept {
    const auto found = voices.find(handle);
    return found == voices.end() ? 0 : found->second.seeks;
}

} // namespace Pelican

// stb_vorbis's implementation, after miniaudio's (see the top of the file).
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4100 4244 4245 4456 4457 4701 4996)
#endif
#undef STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif
