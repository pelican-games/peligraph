#pragma once

#include "../../project/audioanalysisformat.hpp"

#include <atomic>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <vector>

// AB-1: the analysis bake. One function, called by bake_audio_analysis's
// worker and by `pelican_cli audio analyze`: the source decoded by AU-2's
// decoders (AudioDecode::decodeWholeSound), its rms, peak and log-spaced band
// levels every hop, written as the table and manifest audioanalysisformat.hpp
// describes. Deterministic: the same bytes and settings give the same files.
namespace Pelican {

namespace AudioDecode {
struct DecodedSound; // sounddecode.hpp
}

struct AudioAnalysisTable {
    SampleTableLayout layout;
    // width * rows_per_feature * features.size() values, row-major.
    std::vector<float> values;
};

// Sample i is the window of window_ms centred on i * hop: its rms and peak
// (of the samples as they are), and each band's level, the root of the
// summed squared amplitudes (2|X|/sum(w), Hann window, FFT of the next power
// of two) of the bins in [20 * (nyquist/20)^(b/N), 20 * (nyquist/20)^((b+1)/N)).
// A full-scale sine reads about 0.71 rms, 1 peak and about 1.2 in its band.
// Then each row is smoothed (one pole, attack while rising, release while
// falling; 0 ms leaves it as it is). sample_count = floor(frames / hop) + 1.
AudioAnalysisTable analyzeAudio(const AudioDecode::DecodedSound &sound, const AudioAnalysisSettings &settings);

struct AudioAnalysisBakeResult {
    std::filesystem::path out;
    std::filesystem::path manifest;
    nlohmann::ordered_json layout;
};

// Reads `source`, analyzes it, writes `out` (R32_SFLOAT KTX2, writeKtx2) and
// `<out>.import.json` (audioAnalysisManifest), each through a temporary file
// renamed over the old one (writeFileReplacing, retried while a reader holds
// the old one). Throws GraphRegistryError: audio.format_unsupported@1 from the
// decoder, audio.analysis_failed@1 for anything else; {reason: "output_locked",
// path} when a reader held `out` or the manifest past the retries.
AudioAnalysisBakeResult bakeAudioAnalysis(const std::filesystem::path &source, const std::filesystem::path &out,
                                          const AudioAnalysisSettings &settings);

} // namespace Pelican
