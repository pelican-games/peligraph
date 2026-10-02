#pragma once

// AB-1s: the tables a sound's analysis is baked into (engine AB-1:
// bake_audio_analysis, value.sample_table, pelican_sample_table.glsl,
// list_derived_assets), as Studio reads them from the project -- the KTX2
// and the pelican.import manifest beside it -- and what it says of them.

#include "../../project/audioanalysisformat.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {

// One baked table of the project: `<asset>.import.json` whose tool is
// pelican.audio_analysis. Paths are relative to the project root with
// forward slashes ("assets/analysis/tone.ktx2"); `source` is the sound it
// was baked from, `settings` what the manifest records (every field).
struct GraphCanvasAnalysisTable {
    std::string asset;
    std::string manifest;
    std::string source;
    nlohmann::json settings = nlohmann::json::object();
    Pelican::SampleTableLayout layout;
};

// A reference without its project:// ("project://assets/a.ktx2" ->
// "assets/a.ktx2"); anything else as it is.
[[nodiscard]] std::string projectRelativeReference(std::string_view reference);
// The table `asset` names (with or without project://), when its manifest is
// a pelican.audio_analysis one Studio can read; nullopt otherwise.
[[nodiscard]] std::optional<GraphCanvasAnalysisTable>
readAnalysisTable(const std::filesystem::path &project_root, std::string_view asset);
// Every such table under the project, in path order (a folder whose name
// starts with "." is not entered).
[[nodiscard]] std::vector<GraphCanvasAnalysisTable>
readProjectAnalysisTables(const std::filesystem::path &project_root);

// Decision 1: where a bake of `sound` goes unless changed --
// "assets/analysis/<the sound's file name without extension>.ktx2".
[[nodiscard]] std::string defaultAnalysisOut(std::string_view sound);

// What the Bake dialog sends as `settings`. `features` is any of "rms",
// "peak", "bands"; window_ms is left to the engine (twice the hop).
struct AudioBakeChoice {
    bool rms = true;
    bool peak = true;
    bool bands = true;
    int band_count = 8;
    double hop_ms = 10.0;
    double attack_ms = 0.0;
    double release_ms = 0.0;
    std::string channels = "mix";
};
[[nodiscard]] nlohmann::json audioBakeSettings(const AudioBakeChoice &choice);

// The committed line: "Baked <out>: <features> × <sample_count> samples", the
// bands as "band:0..band:N-1" when there are more than two.
[[nodiscard]] std::string bakedLine(const nlohmann::json &committed);
// The feature rows as that line names them.
[[nodiscard]] std::string analysisFeaturesText(const std::vector<std::string> &features);

// Decision 4: one entry of list_derived_assets.
struct GraphCanvasDerivedAsset {
    std::string asset;
    std::string manifest;
    std::string source;
    std::string tool;
    bool stale = false;
    std::vector<std::string> stale_reasons;
};
// The list's entries Studio can read; the rest are skipped.
[[nodiscard]] std::vector<GraphCanvasDerivedAsset> parseDerivedAssets(const nlohmann::json &list);
// "stale: <reason>[, <reason>...]".
[[nodiscard]] std::string staleBandText(const GraphCanvasDerivedAsset &asset);

// Decision 4: Re-bake -- bake_audio_analysis's params from the table's own
// manifest (its sound, the same out, the settings it records). Null when the
// manifest cannot be read.
[[nodiscard]] nlohmann::json rebakeParams(const std::filesystem::path &project_root,
                                          std::string_view asset);

// Decision 3: the shader's side, one line to copy: the include and the call
// that reads row `feature` of the image `image` with the layout `layout`
// (a vec4 buffer port: rows_per_feature, sample_count, hop_seconds,
// scene_time).
[[nodiscard]] std::string sampleTableShaderLine(const std::string &image, const std::string &layout,
                                                std::size_t feature);

} // namespace PelicanStudio
