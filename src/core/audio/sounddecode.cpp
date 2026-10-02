#include "sounddecode.hpp"

#include "../../project/graphcomposition.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace Pelican::AudioDecode {

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

struct WavLayout {
    WavFormat format;
    std::size_t data_offset = 0;
    std::size_t data_size = 0;

    std::uint64_t frameCount() const { return data_size / format.block_align; }
};

// Everything decodeWav checks before it reads a sample. A streamed WAV is held
// to the same rules through this, so the length of a sound never decides which
// WAV files are accepted.
WavLayout parseWav(std::span<const std::byte> bytes) {
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
    return WavLayout{format, data_offset, data_size};
}

DecodedSound decodeWav(std::span<const std::byte> bytes) {
    const auto layout = parseWav(bytes);
    const auto &format = layout.format;
    const auto bytes_per_sample = static_cast<std::uint16_t>(format.bits_per_sample / 8);
    const auto frame_count = static_cast<std::size_t>(layout.frameCount());
    DecodedSound decoded;
    decoded.format = SoundFormat::wav;
    decoded.channels = format.channels;
    decoded.sample_rate = format.sample_rate;
    decoded.samples.reserve(frame_count * format.channels);

    const auto *data = WavReader{bytes}.data(layout.data_offset);
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

bool bytesAt(std::span<const std::byte> bytes, std::size_t offset, std::string_view expected) {
    if (offset > bytes.size() || expected.size() > bytes.size() - offset) return false;
    for (std::size_t i = 0; i < expected.size(); ++i)
        if (static_cast<unsigned char>(bytes[offset + i]) != static_cast<unsigned char>(expected[i])) return false;
    return true;
}

// What the first bytes say the file is: RIFF/WAVE, or an Ogg stream whose
// first packet is a Vorbis identification header. Ogg carrying anything else
// (Opus, FLAC) is not Ogg Vorbis.
std::string_view soundSignature(std::span<const std::byte> bytes) {
    if (bytesAt(bytes, 0, "RIFF") && bytesAt(bytes, 8, "WAVE")) return "wav";
    if (bytesAt(bytes, 0, "OggS") && bytes.size() > 26) {
        const auto first_packet = std::size_t{27} + static_cast<unsigned char>(bytes[26]);
        if (bytesAt(bytes, first_packet, std::string_view{"\x01vorbis", 7})) return "ogg";
    }
    return "unknown";
}

std::string soundExtension(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    const auto name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const auto dot = name.rfind('.');
    std::string extension{dot == std::string_view::npos ? std::string_view{} : name.substr(dot)};
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

// AU-2 decision 1: the extension and the first bytes must both name the same
// supported format. Anything else is refused by name before any decoding.
SoundFormat detectSoundFormat(std::string_view path, std::span<const std::byte> bytes) {
    const auto extension = soundExtension(path);
    const auto signature = soundSignature(bytes);
    if (extension == ".wav" && signature == "wav") return SoundFormat::wav;
    if (extension == ".ogg" && signature == "ogg") return SoundFormat::ogg;
    throw GraphRegistryError("audio.format_unsupported@1",
        {{"asset", std::string{path}}, {"extension", extension}, {"signature", std::string{signature}},
         {"supported", nlohmann::json::array({".wav (RIFF/WAVE)", ".ogg (Ogg Vorbis)"})}});
}

ma_decoder_config decoderConfig(SoundFormat format) {
    // Float samples at the source's own channels and rate: the cursor stays in
    // source frames, and the engine converts the rest as it does for a buffer.
    auto config = ma_decoder_config_init(ma_format_f32, 0, 0);
    config.encodingFormat = format == SoundFormat::ogg ? ma_encoding_format_vorbis : ma_encoding_format_wav;
    return config;
}

class ScopedDecoder {
    ma_decoder decoder_{};
    bool initialized_ = false;

  public:
    ScopedDecoder(SoundFormat format, std::span<const std::byte> bytes) {
        const auto config = decoderConfig(format);
        const auto result = ma_decoder_init_memory(bytes.data(), bytes.size(), &config, &decoder_);
        if (result != MA_SUCCESS)
            throw std::runtime_error(std::string{format == SoundFormat::ogg ? "Invalid Ogg Vorbis: " : "Invalid WAV: "} +
                                     maErrorMessage("ma_decoder_init_memory", result));
        initialized_ = true;
    }
    ScopedDecoder(const ScopedDecoder &) = delete;
    ScopedDecoder &operator=(const ScopedDecoder &) = delete;
    ~ScopedDecoder() {
        if (initialized_) ma_decoder_uninit(&decoder_);
    }
    ma_decoder *get() { return &decoder_; }
};

// A sound's format, channels, rate and length from its header, decoding no
// sample: the WAV rules of parseWav, or the Vorbis headers and the last
// page's position.
DecodedSound readSoundHeader(SoundFormat format, std::span<const std::byte> bytes) {
    DecodedSound header;
    header.format = format;
    if (format == SoundFormat::wav) {
        const auto layout = parseWav(bytes);
        header.channels = layout.format.channels;
        header.sample_rate = layout.format.sample_rate;
        header.streamed_frames = layout.frameCount();
        return header;
    }
    ScopedDecoder decoder{format, bytes};
    ma_format sample_format{};
    ma_uint32 channels = 0;
    ma_uint32 sample_rate = 0;
    auto result = ma_decoder_get_data_format(decoder.get(), &sample_format, &channels, &sample_rate, nullptr, 0);
    if (result != MA_SUCCESS)
        throw std::runtime_error("Invalid Ogg Vorbis: " + maErrorMessage("ma_decoder_get_data_format", result));
    ma_uint64 length = 0;
    result = ma_decoder_get_length_in_pcm_frames(decoder.get(), &length);
    if (result != MA_SUCCESS || length == 0 || channels == 0 || sample_rate == 0)
        throw std::runtime_error("Invalid Ogg Vorbis: the stream has no length, channels or sample rate");
    header.channels = channels;
    header.sample_rate = sample_rate;
    header.streamed_frames = length;
    return header;
}

// The whole sound, from the header readSoundHeader already read.
DecodedSound decodeOgg(std::span<const std::byte> bytes, DecodedSound decoded) {
    ScopedDecoder decoder{SoundFormat::ogg, bytes};
    decoded.samples.resize(static_cast<std::size_t>(decoded.streamed_frames) * decoded.channels);
    ma_uint64 read = 0;
    const auto result = ma_decoder_read_pcm_frames(decoder.get(), decoded.samples.data(), decoded.streamed_frames, &read);
    if ((result != MA_SUCCESS && result != MA_AT_END) || read == 0)
        throw std::runtime_error("Invalid Ogg Vorbis: " + maErrorMessage("ma_decoder_read_pcm_frames", result));
    decoded.samples.resize(static_cast<std::size_t>(read) * decoded.channels);
    decoded.streamed_frames = 0;
    return decoded;
}

DecodedSound decodeWholeSound(std::string_view path, std::span<const std::byte> bytes) {
    const auto format = detectSoundFormat(path, bytes);
    if (format == SoundFormat::wav) return decodeWav(bytes);
    return decodeOgg(bytes, readSoundHeader(format, bytes));
}

} // namespace Pelican::AudioDecode
