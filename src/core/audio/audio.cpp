#include "audio.hpp"

#include "../launchconfig.hpp"
#include "../loader/pathresolver.hpp"
#include "../log.hpp"

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Pelican {

namespace {

constexpr std::size_t busIndex(AudioBus bus) {
    return static_cast<std::size_t>(bus);
}

struct DecodedSound {
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::vector<float> samples;

    std::uint64_t frameCount() const {
        return channels == 0 ? 0 : static_cast<std::uint64_t>(samples.size() / channels);
    }
};

class WavReader {
    std::span<const std::byte> bytes;

  public:
    explicit WavReader(std::span<const std::byte> input) : bytes{input} {}

    bool has(std::size_t offset, std::size_t size) const {
        return offset <= bytes.size() && size <= bytes.size() - offset;
    }

    bool fourcc(std::size_t offset, const char expected[4]) const {
        if (!has(offset, 4)) {
            return false;
        }
        for (std::size_t i = 0; i < 4; ++i) {
            if (static_cast<unsigned char>(bytes[offset + i]) != static_cast<unsigned char>(expected[i])) {
                return false;
            }
        }
        return true;
    }

    std::uint16_t u16(std::size_t offset) const {
        if (!has(offset, 2)) {
            throw std::runtime_error("Invalid WAV: unexpected end of file");
        }
        return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset])) |
               static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset + 1]) << 8);
    }

    std::uint32_t u32(std::size_t offset) const {
        if (!has(offset, 4)) {
            throw std::runtime_error("Invalid WAV: unexpected end of file");
        }
        return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24);
    }

    const std::byte *data(std::size_t offset) const {
        if (!has(offset, 1)) {
            throw std::runtime_error("Invalid WAV: unexpected end of file");
        }
        return bytes.data() + offset;
    }
};

struct WavFormat {
    std::uint16_t audio_format = 0;
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t block_align = 0;
    std::uint16_t bits_per_sample = 0;
};

float decodePcmSample(const std::byte *sample, std::uint16_t bits_per_sample) {
    switch (bits_per_sample) {
    case 8: {
        const auto value = static_cast<int>(static_cast<unsigned char>(sample[0])) - 128;
        return static_cast<float>(value) / 128.0f;
    }
    case 16: {
        const auto raw = static_cast<std::uint16_t>(static_cast<unsigned char>(sample[0])) |
                         (static_cast<std::uint16_t>(static_cast<unsigned char>(sample[1])) << 8);
        return static_cast<float>(static_cast<std::int16_t>(raw)) / 32768.0f;
    }
    case 24: {
        std::int32_t raw = static_cast<std::int32_t>(static_cast<unsigned char>(sample[0])) |
                           (static_cast<std::int32_t>(static_cast<unsigned char>(sample[1])) << 8) |
                           (static_cast<std::int32_t>(static_cast<unsigned char>(sample[2])) << 16);
        if ((raw & 0x00800000) != 0) {
            raw |= static_cast<std::int32_t>(0xFF000000);
        }
        return static_cast<float>(raw) / 8388608.0f;
    }
    case 32: {
        const auto raw = static_cast<std::uint32_t>(static_cast<unsigned char>(sample[0])) |
                         (static_cast<std::uint32_t>(static_cast<unsigned char>(sample[1])) << 8) |
                         (static_cast<std::uint32_t>(static_cast<unsigned char>(sample[2])) << 16) |
                         (static_cast<std::uint32_t>(static_cast<unsigned char>(sample[3])) << 24);
        return static_cast<float>(static_cast<std::int32_t>(raw)) / 2147483648.0f;
    }
    default:
        throw std::runtime_error("Invalid WAV: unsupported PCM bit depth");
    }
}

float decodeFloatSample(const std::byte *sample, std::uint16_t bits_per_sample) {
    if (bits_per_sample != 32) {
        throw std::runtime_error("Invalid WAV: IEEE float WAV must be 32-bit");
    }

    float value = 0.0f;
    std::uint32_t raw = static_cast<std::uint32_t>(static_cast<unsigned char>(sample[0])) |
                        (static_cast<std::uint32_t>(static_cast<unsigned char>(sample[1])) << 8) |
                        (static_cast<std::uint32_t>(static_cast<unsigned char>(sample[2])) << 16) |
                        (static_cast<std::uint32_t>(static_cast<unsigned char>(sample[3])) << 24);
    std::memcpy(&value, &raw, sizeof(value));
    if (!std::isfinite(value)) {
        throw std::runtime_error("Invalid WAV: non-finite float sample");
    }
    return std::clamp(value, -1.0f, 1.0f);
}

DecodedSound decodeWav(std::span<const std::byte> bytes) {
    WavReader reader{bytes};
    if (!reader.fourcc(0, "RIFF") || !reader.fourcc(8, "WAVE")) {
        throw std::runtime_error("Invalid WAV: missing RIFF/WAVE header");
    }

    bool found_format = false;
    bool found_data = false;
    WavFormat format;
    std::size_t data_offset = 0;
    std::size_t data_size = 0;

    std::size_t offset = 12;
    while (reader.has(offset, 8)) {
        const auto chunk_size = static_cast<std::size_t>(reader.u32(offset + 4));
        const auto chunk_data = offset + 8;
        if (!reader.has(chunk_data, chunk_size)) {
            throw std::runtime_error("Invalid WAV: chunk extends past end of file");
        }

        if (reader.fourcc(offset, "fmt ")) {
            if (chunk_size < 16) {
                throw std::runtime_error("Invalid WAV: fmt chunk is too small");
            }
            format.audio_format = reader.u16(chunk_data);
            format.channels = reader.u16(chunk_data + 2);
            format.sample_rate = reader.u32(chunk_data + 4);
            format.block_align = reader.u16(chunk_data + 12);
            format.bits_per_sample = reader.u16(chunk_data + 14);
            found_format = true;
        } else if (reader.fourcc(offset, "data")) {
            data_offset = chunk_data;
            data_size = chunk_size;
            found_data = true;
        }

        offset = chunk_data + chunk_size + (chunk_size % 2);
    }

    if (!found_format) {
        throw std::runtime_error("Invalid WAV: missing fmt chunk");
    }
    if (!found_data) {
        throw std::runtime_error("Invalid WAV: missing data chunk");
    }
    if (format.audio_format != 1 && format.audio_format != 3) {
        throw std::runtime_error("Invalid WAV: only PCM and IEEE float WAV are supported");
    }
    if (format.channels == 0 || format.channels > MA_MAX_CHANNELS) {
        throw std::runtime_error("Invalid WAV: unsupported channel count");
    }
    if (format.sample_rate == 0) {
        throw std::runtime_error("Invalid WAV: sample rate must be non-zero");
    }
    if ((format.bits_per_sample % 8) != 0) {
        throw std::runtime_error("Invalid WAV: bit depth must be byte aligned");
    }

    const auto bytes_per_sample = static_cast<std::uint16_t>(format.bits_per_sample / 8);
    if (bytes_per_sample == 0) {
        throw std::runtime_error("Invalid WAV: bit depth must be non-zero");
    }
    const auto expected_block_align = static_cast<std::uint16_t>(format.channels * bytes_per_sample);
    if (format.block_align != expected_block_align) {
        throw std::runtime_error("Invalid WAV: block align does not match format");
    }
    if (data_size == 0 || (data_size % format.block_align) != 0) {
        throw std::runtime_error("Invalid WAV: data chunk size does not match format");
    }

    const auto frame_count = data_size / format.block_align;
    DecodedSound decoded;
    decoded.channels = format.channels;
    decoded.sample_rate = format.sample_rate;
    decoded.samples.reserve(frame_count * format.channels);

    const auto *data = reader.data(data_offset);
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto frame_offset = frame * format.block_align;
        for (std::uint16_t channel = 0; channel < format.channels; ++channel) {
            const auto *sample = data + frame_offset + channel * bytes_per_sample;
            const auto value = format.audio_format == 3
                                   ? decodeFloatSample(sample, format.bits_per_sample)
                                   : decodePcmSample(sample, format.bits_per_sample);
            decoded.samples.push_back(value);
        }
    }

    return decoded;
}

std::string maErrorMessage(const char *operation, ma_result result) {
    return std::string{operation} + " failed with miniaudio result " + std::to_string(result);
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

class Audio::Backend {
  public:
    virtual ~Backend() = default;
    virtual std::uint64_t play(std::shared_ptr<const DecodedSound> sound, float volume) = 0;
    virtual void destroy(std::uint64_t voice_id) = 0;
    virtual bool isFinished(std::uint64_t voice_id) const = 0;
    virtual bool isPlaying(std::uint64_t voice_id) const = 0;
    virtual void setVolume(std::uint64_t voice_id, float volume) = 0;
    virtual std::size_t voiceCount() const noexcept = 0;
};

class NullAudioBackend final : public Audio::Backend {
    std::uint64_t next_voice = 1;
    std::unordered_map<std::uint64_t, bool> playing;

  public:
    std::uint64_t play(std::shared_ptr<const DecodedSound> sound, float volume) override {
        (void)sound;
        (void)volume;
        const auto id = next_voice++;
        playing.emplace(id, true);
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
        return it != playing.end() && it->second;
    }

    void setVolume(std::uint64_t voice_id, float volume) override {
        (void)voice_id;
        (void)volume;
    }

    std::size_t voiceCount() const noexcept override {
        return playing.size();
    }
};

class MiniaudioBackend final : public Audio::Backend {
    struct MiniaudioVoice {
        std::shared_ptr<const DecodedSound> decoded;
        ma_audio_buffer buffer{};
        ma_sound sound{};
        bool buffer_initialized = false;
        bool sound_initialized = false;

        MiniaudioVoice() = default;
        MiniaudioVoice(const MiniaudioVoice &) = delete;
        MiniaudioVoice &operator=(const MiniaudioVoice &) = delete;

        ~MiniaudioVoice() {
            if (sound_initialized) {
                ma_sound_stop(&sound);
                ma_sound_uninit(&sound);
            }
            if (buffer_initialized) {
                ma_audio_buffer_uninit(&buffer);
            }
        }
    };

    ma_engine engine{};
    bool engine_initialized = false;
    std::uint64_t next_voice = 1;
    std::unordered_map<std::uint64_t, std::unique_ptr<MiniaudioVoice>> voices;

  public:
    MiniaudioBackend() {
        const auto result = ma_engine_init(nullptr, &engine);
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

    std::uint64_t play(std::shared_ptr<const DecodedSound> sound, float volume) override {
        auto voice = std::make_unique<MiniaudioVoice>();
        voice->decoded = std::move(sound);

        auto config = ma_audio_buffer_config_init(ma_format_f32,
                                                  voice->decoded->channels,
                                                  voice->decoded->frameCount(),
                                                  voice->decoded->samples.data(),
                                                  nullptr);
        config.sampleRate = voice->decoded->sample_rate;

        auto result = ma_audio_buffer_init(&config, &voice->buffer);
        if (result != MA_SUCCESS) {
            throw std::runtime_error(maErrorMessage("ma_audio_buffer_init", result));
        }
        voice->buffer_initialized = true;

        result = ma_sound_init_from_data_source(&engine,
                                                &voice->buffer,
                                                MA_SOUND_FLAG_NO_SPATIALIZATION,
                                                nullptr,
                                                &voice->sound);
        if (result != MA_SUCCESS) {
            throw std::runtime_error(maErrorMessage("ma_sound_init_from_data_source", result));
        }
        voice->sound_initialized = true;

        ma_sound_set_volume(&voice->sound, volume);
        result = ma_sound_start(&voice->sound);
        if (result != MA_SUCCESS) {
            throw std::runtime_error(maErrorMessage("ma_sound_start", result));
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

    void setVolume(std::uint64_t voice_id, float volume) override {
        const auto it = voices.find(voice_id);
        if (it != voices.end()) {
            ma_sound_set_volume(&it->second->sound, volume);
        }
    }

    std::size_t voiceCount() const noexcept override {
        return voices.size();
    }
};

bool shouldUseNullBackend() {
    const auto &launch_config = GET_MODULE(EngineLaunchConfig);
    return launch_config.headless || launch_config.rpc;
}

Audio::Audio() {
    if (shouldUseNullBackend()) {
        LOG_INFO(logger, "audio backend: null");
        backend = std::make_unique<NullAudioBackend>();
        return;
    }

    try {
        backend = std::make_unique<MiniaudioBackend>();
        LOG_INFO(logger, "audio backend: miniaudio");
    } catch (const std::exception &error) {
        LOG_WARNING(logger, "audio device backend unavailable; falling back to null: {}", error.what());
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
        backend->setVolume(voice.backend_id, effectiveVolume(voice.bus));
    }
}

SoundHandle Audio::playSound(std::string_view path) {
    const auto bytes = GET_MODULE(PathResolver).loadBytes(path);
    auto decoded = std::make_shared<DecodedSound>(decodeWav(std::span<const std::byte>{bytes.data(), bytes.size()}));

    const auto handle = SoundHandle{next_handle++};
    const auto backend_id = backend->play(decoded, effectiveVolume(AudioBus::Se));
    try {
        const auto [it, inserted] =
            voices.emplace(handle, Voice{AudioBus::Se, backend_id});
        (void)it;
        if (!inserted) {
            throw std::runtime_error("Audio sound handle space exhausted");
        }
    } catch (...) {
        // Once backend playback has started, publishing the public handle is
        // the ownership transfer. Roll it back if map allocation/publication
        // fails so the backend cannot retain an unreachable voice.
        backend->destroy(backend_id);
        throw;
    }
    return handle;
}

void Audio::stopSound(SoundHandle handle) {
    const auto it = voices.find(handle);
    if (it == voices.end()) {
        return;
    }
    backend->destroy(it->second.backend_id);
    voices.erase(it);
}

void Audio::setBusVolume(std::string_view bus, float volume) {
    if (!std::isfinite(volume) || volume < 0.0f) {
        throw std::runtime_error("Audio bus volume must be a finite non-negative number");
    }
    bus_volumes[busIndex(audioBusFromName(bus))] = volume;
    updateVoiceVolumes();
}

float Audio::busVolume(std::string_view bus) const {
    return bus_volumes[busIndex(audioBusFromName(bus))];
}

bool Audio::isPlaying(SoundHandle handle) const {
    const auto it = voices.find(handle);
    return it != voices.end() && backend->isPlaying(it->second.backend_id);
}

void Audio::update() {
    for (auto it = voices.begin(); it != voices.end();) {
        if (!backend->isFinished(it->second.backend_id)) {
            ++it;
            continue;
        }

        backend->destroy(it->second.backend_id);
        it = voices.erase(it);
    }
}

std::size_t Audio::voiceCountForTesting() const noexcept {
    return voices.size();
}

std::size_t Audio::backendVoiceCountForTesting() const noexcept {
    return backend->voiceCount();
}

} // namespace Pelican
