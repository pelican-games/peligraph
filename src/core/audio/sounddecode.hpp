#pragma once

// AU-2's sound decoding, apart from the Audio module so a tool that is not the
// engine (pelican_cli audio analyze, AB-1) decodes an asset the same way the
// voices do: the same formats, the same refusals, the same samples. miniaudio's
// implementation is compiled once per executable elsewhere (audio.cpp in the
// engine, the CLI's own unit).

#include <miniaudio.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican::AudioDecode {

// AU-2 decision 1: the two formats an asset can be.
enum class SoundFormat : std::uint8_t { wav, ogg };

// A sound as the voices read it. A short one is decoded once and its samples
// are shared by every voice; a long one (AU-2 decision 3) keeps only its
// encoded bytes and what its header says, and each voice decodes it as it
// plays through a decoder of its own.
struct DecodedSound {
    SoundFormat format = SoundFormat::wav;
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::vector<float> samples;
    std::shared_ptr<const std::vector<std::byte>> encoded; // set when streamed
    std::uint64_t streamed_frames = 0;

    bool streamed() const { return encoded != nullptr; }
    std::uint64_t frameCount() const {
        if (streamed()) return streamed_frames;
        return channels == 0 ? 0 : static_cast<std::uint64_t>(samples.size() / channels);
    }
    // What the sound keeps in memory: its samples, or its encoded bytes.
    std::size_t residentBytes() const {
        return streamed() ? encoded->size() : samples.size() * sizeof(float);
    }
};

std::string maErrorMessage(const char *operation, ma_result result);
// AU-2 decision 1: the extension and the first bytes must both name the same
// supported format; anything else is audio.format_unsupported@1.
SoundFormat detectSoundFormat(std::string_view path, std::span<const std::byte> bytes);
ma_decoder_config decoderConfig(SoundFormat format);
// A sound's format, channels, rate and length from its header, decoding no
// sample.
DecodedSound readSoundHeader(SoundFormat format, std::span<const std::byte> bytes);
DecodedSound decodeWav(std::span<const std::byte> bytes);
// The whole sound, from the header readSoundHeader already read.
DecodedSound decodeOgg(std::span<const std::byte> bytes, DecodedSound decoded);
// AB-1: the whole sound whatever its length (never streamed), float samples
// interleaved at the source's channels and rate. `path` names the asset (its
// extension is checked against its first bytes, as a voice's asset is).
DecodedSound decodeWholeSound(std::string_view path, std::span<const std::byte> bytes);

} // namespace Pelican::AudioDecode
