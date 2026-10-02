#include "audioanalysis.hpp"

#include "sounddecode.hpp"
#include "../loader/ktx2.hpp"
#include "../../project/graphcomposition.hpp"
#include "../../project/sha256stream.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <fstream>
#include <iterator>
#include <numbers>
#include <stdexcept>
#include <string>
#include <system_error>

namespace Pelican {
namespace {

// In place, radix 2; `twiddles[k]` = exp(-2 pi i k / size), size / 2 of them.
void fft(std::vector<std::complex<double>> &values, const std::vector<std::complex<double>> &twiddles) {
    const auto size = values.size();
    for (std::size_t i = 1, j = 0; i < size; ++i) {
        auto bit = size >> 1;
        for (; (j & bit) != 0; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(values[i], values[j]);
    }
    for (std::size_t length = 2; length <= size; length <<= 1) {
        const auto step = size / length;
        const auto half = length / 2;
        for (std::size_t start = 0; start < size; start += length)
            for (std::size_t k = 0; k < half; ++k) {
                const auto odd = values[start + k + half] * twiddles[k * step];
                values[start + k + half] = values[start + k] - odd;
                values[start + k] += odd;
            }
    }
}

std::vector<float> channelSamples(const AudioDecode::DecodedSound &sound, const std::string &channels) {
    const auto frames = static_cast<std::size_t>(sound.frameCount());
    std::vector<float> result(frames);
    const auto count = sound.channels;
    if (count == 0) return result;
    const std::uint32_t pick = channels == "left" ? 0u : (std::min)(1u, count - 1);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto *samples = sound.samples.data() + frame * count;
        if (channels == "mix") {
            double sum = 0.0;
            for (std::uint32_t channel = 0; channel < count; ++channel) sum += samples[channel];
            result[frame] = static_cast<float>(sum / count);
        } else {
            result[frame] = samples[pick];
        }
    }
    return result;
}

std::vector<std::byte> readBytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error("cannot read " + path.generic_string());
    std::vector<char> chars{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes(chars.size());
    if (!chars.empty()) std::memcpy(bytes.data(), chars.data(), chars.size());
    return bytes;
}

// Through a temporary file beside it, renamed over the old one, so a reader
// never sees half a file.
void writeReplacing(const std::filesystem::path &path, const void *data, std::size_t size) {
    std::error_code error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        if (!output) throw std::runtime_error("cannot write " + temporary.generic_string());
        output.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
        if (!output) throw std::runtime_error("cannot write " + temporary.generic_string());
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        throw std::runtime_error("cannot replace " + path.generic_string());
    }
}

} // namespace

AudioAnalysisTable analyzeAudio(const AudioDecode::DecodedSound &sound, const AudioAnalysisSettings &settings) {
    if (sound.sample_rate == 0 || sound.channels == 0)
        throw std::invalid_argument("the sound has no sample rate or channels");
    const auto rate = static_cast<double>(sound.sample_rate);
    const auto samples = channelSamples(sound, settings.channels);
    const auto frames = samples.size();
    const auto hop = settings.hop_ms * rate / 1000.0;
    if (hop < 1.0) throw std::invalid_argument("hop_ms is shorter than one sample of the sound");
    const auto window = (std::max)(std::size_t{1}, static_cast<std::size_t>(std::llround(settings.window_ms * rate / 1000.0)));
    std::size_t fft_size = 2;
    while (fft_size < window) fft_size <<= 1;

    const auto rows = audioAnalysisFeatureRows(settings);
    AudioAnalysisTable table;
    auto &layout = table.layout;
    layout.width = sampleTableWidth;
    layout.sample_count = static_cast<std::uint32_t>(std::floor(static_cast<double>(frames) / hop)) + 1;
    layout.rows_per_feature = (layout.sample_count + layout.width - 1) / layout.width;
    layout.hop_seconds = settings.hop_ms / 1000.0;
    layout.features = rows;
    table.values.assign(static_cast<std::size_t>(layout.width) * layout.rows_per_feature * rows.size(), 0.0f);

    std::vector<double> hann(window, 1.0);
    double window_sum = 0.0;
    for (std::size_t n = 0; n < window; ++n) {
        if (window > 1) hann[n] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) /
                                                        static_cast<double>(window - 1));
        window_sum += hann[n];
    }
    std::vector<std::complex<double>> twiddles(fft_size / 2);
    for (std::size_t k = 0; k < twiddles.size(); ++k)
        twiddles[k] = std::polar(1.0, -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(fft_size));
    // The bins of each band: [first, last).
    const auto nyquist = rate / 2.0;
    std::vector<std::pair<std::size_t, std::size_t>> band_bins;
    if (settings.bands_enabled) {
        const auto bin_of = [&](double frequency) {
            return static_cast<std::size_t>(std::ceil(frequency * static_cast<double>(fft_size) / rate));
        };
        for (std::uint32_t band = 0; band < settings.bands; ++band) {
            if (nyquist <= 20.0) {
                band_bins.emplace_back(0, 0);
                continue;
            }
            const auto low = 20.0 * std::pow(nyquist / 20.0, static_cast<double>(band) / settings.bands);
            const auto high = 20.0 * std::pow(nyquist / 20.0, static_cast<double>(band + 1) / settings.bands);
            auto first = (std::max)(std::size_t{1}, bin_of(low));
            auto last = band + 1 == settings.bands ? fft_size / 2 + 1 : bin_of(high);
            last = (std::min)(last, fft_size / 2 + 1);
            band_bins.emplace_back(first, (std::max)(first, last));
        }
    }

    const auto put = [&](std::size_t row, std::uint32_t sample, double value) {
        table.values[(row * layout.rows_per_feature + sample / layout.width) * layout.width + sample % layout.width] =
            static_cast<float>(value);
    };
    std::vector<std::complex<double>> spectrum(fft_size);
    for (std::uint32_t sample = 0; sample < layout.sample_count; ++sample) {
        const auto centre = static_cast<std::int64_t>(std::llround(static_cast<double>(sample) * hop));
        const auto begin = centre - static_cast<std::int64_t>(window / 2);
        double square_sum = 0.0;
        double peak = 0.0;
        std::fill(spectrum.begin(), spectrum.end(), std::complex<double>{});
        for (std::size_t n = 0; n < window; ++n) {
            const auto at = begin + static_cast<std::int64_t>(n);
            const double value = at >= 0 && static_cast<std::size_t>(at) < frames ? samples[static_cast<std::size_t>(at)]
                                                                                   : 0.0;
            square_sum += value * value;
            peak = (std::max)(peak, std::abs(value));
            spectrum[n] = value * hann[n];
        }
        std::size_t row = 0;
        if (settings.rms) put(row++, sample, std::sqrt(square_sum / static_cast<double>(window)));
        if (settings.peak) put(row++, sample, peak);
        if (settings.bands_enabled) {
            fft(spectrum, twiddles);
            const auto scale = window_sum > 0.0 ? 2.0 / window_sum : 0.0;
            for (const auto &[first, last] : band_bins) {
                double power = 0.0;
                for (auto bin = first; bin < last; ++bin) {
                    const auto amplitude = std::abs(spectrum[bin]) * scale;
                    power += amplitude * amplitude;
                }
                put(row++, sample, std::sqrt(power));
            }
        }
    }

    if (settings.attack_ms > 0.0 || settings.release_ms > 0.0) {
        const auto coefficient = [&](double ms) { return ms > 0.0 ? std::exp(-settings.hop_ms / ms) : 0.0; };
        const auto attack = coefficient(settings.attack_ms);
        const auto release = coefficient(settings.release_ms);
        for (std::size_t row = 0; row < rows.size(); ++row) {
            double level = 0.0;
            for (std::uint32_t sample = 0; sample < layout.sample_count; ++sample) {
                const auto index =
                    (row * layout.rows_per_feature + sample / layout.width) * layout.width + sample % layout.width;
                const double value = table.values[index];
                const auto pole = value > level ? attack : release;
                level = pole * level + (1.0 - pole) * value;
                table.values[index] = static_cast<float>(level);
            }
        }
    }
    return table;
}

AudioAnalysisBakeResult bakeAudioAnalysis(const std::filesystem::path &source, const std::filesystem::path &out,
                                          const AudioAnalysisSettings &settings) {
    const auto failed = [&](const std::string &message) {
        return GraphRegistryError(std::string{audioAnalysisFailedCode},
                                  {{"asset", source.generic_string()}, {"out", out.generic_string()},
                                   {"message", message}});
    };
    AudioAnalysisTable table;
    std::string source_sha256;
    try {
        const auto bytes = readBytes(source);
        source_sha256 = sha256Hex(std::string_view{reinterpret_cast<const char *>(bytes.data()), bytes.size()});
        const auto sound = AudioDecode::decodeWholeSound(source.filename().generic_string(), bytes);
        table = analyzeAudio(sound, settings);
    } catch (const GraphRegistryError &) {
        throw;
    } catch (const std::exception &error) {
        throw failed(error.what());
    }
    try {
        const auto height = table.layout.rows_per_feature * static_cast<std::uint32_t>(table.layout.features.size());
        const auto file = writeKtx2(Ktx2Format::R32Sfloat, table.layout.width, height,
                                    std::as_bytes(std::span{table.values}));
        writeReplacing(out, file.data(), file.size());
        const auto table_sha256 =
            sha256Hex(std::string_view{reinterpret_cast<const char *>(file.data()), file.size()});
        const auto manifest_path = derivedAssetManifestPath(out);
        const auto manifest =
            audioAnalysisManifest(source, source_sha256, settings, table.layout, out, table_sha256).dump(2) + "\n";
        writeReplacing(manifest_path, manifest.data(), manifest.size());
        return {out, manifest_path, sampleTableLayoutJson(table.layout)};
    } catch (const std::exception &error) {
        throw failed(error.what());
    }
}

} // namespace Pelican
