#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// AB-1: a sound's analysis baked into a table (bake_audio_analysis, `pelican_cli
// audio analyze`) and read back by value.sample_table and
// pelican_sample_table.glsl. The table is one R32_SFLOAT 2D KTX2, 4096 texels
// wide: each feature row (rms, peak, band:0 .. band:N-1, in that order, the
// ones the settings leave out absent) takes rows_per_feature =
// ceil(sample_count / 4096) image rows, sample i of feature f at
// (x = i % 4096, y = f * rows_per_feature + i / 4096), the rest 0. Sample i is
// the sound at i * hop_seconds. Beside it, `<table>.import.json` is a
// pelican.import manifest (importmanifest.hpp) whose source.metadata carries
// {sha256, settings, settings_sha256, layout}. The format is the same for a
// future real-time `audio.analysis {voice|bus, features}`, which would give the
// same rows for the frame now.
namespace Pelican {

inline constexpr std::string_view audioAnalysisToolName = "pelican.audio_analysis";
inline constexpr std::string_view audioAnalysisToolVersion = "1";
inline constexpr std::string_view audioAnalysisTableSchema = "pelican.audio_analysis_table";
inline constexpr int audioAnalysisTableSchemaVersion = 1;
inline constexpr std::uint32_t sampleTableWidth = 4096;

// A settings value the bake does not take: {field, reason, value?}.
inline constexpr std::string_view audioAnalysisSettingsInvalidCode = "audio.analysis_settings_invalid@1";
// The bake failed after its settings were read (the source could not be read
// or decoded, or the files not written): {asset, out, message}. A source
// whose extension or first bytes are not WAV / Ogg Vorbis keeps AU-2's
// audio.format_unsupported@1.
inline constexpr std::string_view audioAnalysisFailedCode = "audio.analysis_failed@1";

struct AudioAnalysisSettings {
    bool rms = true;
    bool peak = true;
    bool bands_enabled = true;
    std::uint32_t bands = 8;
    double hop_ms = 10.0;
    // 0 while parsing: twice hop_ms.
    double window_ms = 20.0;
    double attack_ms = 0.0;
    double release_ms = 0.0;
    std::string channels = "mix"; // "mix" | "left" | "right"
};

// {features?: ["rms", "peak", "bands"] (all), bands?: N (8; 1..128),
//  hop_ms?: (10; 1..1000), window_ms?: (2 * hop_ms; 1..4000),
//  smoothing?: {attack_ms?: (0), release_ms?: (0)} (0..60000),
//  channels?: "mix" | "left" | "right" ("mix")}. null or absent: the
// defaults. Anything else throws GraphRegistryError
// audio.analysis_settings_invalid@1.
AudioAnalysisSettings parseAudioAnalysisSettings(const nlohmann::json &settings);
// Every field, defaults filled in: the form the manifest records.
nlohmann::ordered_json audioAnalysisSettingsJson(const AudioAnalysisSettings &settings);
// The feature rows the settings give, in table order: "rms", "peak",
// "band:0" .. "band:<bands-1>".
std::vector<std::string> audioAnalysisFeatureRows(const AudioAnalysisSettings &settings);

struct SampleTableLayout {
    std::uint32_t width = sampleTableWidth;
    std::uint32_t rows_per_feature = 1;
    std::uint32_t sample_count = 0;
    double hop_seconds = 0.01;
    std::vector<std::string> features;
};

// {width, rows_per_feature, sample_count, hop_seconds, features}.
nlohmann::ordered_json sampleTableLayoutJson(const SampleTableLayout &layout);
// Throws std::invalid_argument naming the field.
SampleTableLayout parseSampleTableLayout(const nlohmann::json &layout);
// The feature a row names: its index (an integer) or its name ("rms",
// "peak", "band:3"). nullopt when the table has no such row.
std::optional<std::uint32_t> sampleTableRow(const SampleTableLayout &layout, const nlohmann::json &row);
// The value of feature `feature` at `seconds`: the two samples either side of
// seconds / hop_seconds, linearly; before the first or after the last, the
// end's value. `values` is the whole table, row-major.
double sampleTableValue(const SampleTableLayout &layout, const std::vector<float> &values, std::uint32_t feature,
                        double seconds);

// `<file>.import.json`, beside the file a manifest describes.
std::filesystem::path derivedAssetManifestPath(const std::filesystem::path &file);

// The pelican.import manifest of a bake: tool {pelican.audio_analysis, "1"},
// source {file: the source relative to the manifest's directory, metadata:
// {sha256, settings, settings_sha256, layout}}, outputs [{file: the table's
// file name, schema pelican.audio_analysis_table, version 1, sha256}]. No
// `created`: the same bake of the same bytes writes the same manifest.
nlohmann::ordered_json audioAnalysisManifest(const std::filesystem::path &source, const std::string &source_sha256,
                                             const AudioAnalysisSettings &settings, const SampleTableLayout &layout,
                                             const std::filesystem::path &table, const std::string &table_sha256);

// The sha256 of the settings as the manifest records them
// (audioAnalysisSettingsJson(...).dump()).
std::string audioAnalysisSettingsSha256(const AudioAnalysisSettings &settings);

// A file some tool derived from a source, as its `<file>.import.json` says,
// and whether it is stale: the source is missing or its bytes are not the
// recorded sha256 ("source_missing", "source_changed"), the file itself is
// missing or not its recorded sha256 ("output_missing", "output_changed"),
// and for pelican.audio_analysis the recorded settings are not the ones baked
// (settings_sha256; "settings_changed") or another tool version wrote it
// ("tool_version").
struct DerivedAssetStatus {
    std::filesystem::path manifest;
    std::filesystem::path output;
    std::filesystem::path source;
    std::string tool_name;
    std::optional<std::string> tool_version;
    bool stale = false;
    std::vector<std::string> stale_reasons;
};

// {asset, manifest, tool: {name, version}, source, derived_from: {source,
// tool}, stale, stale_reasons}: list_derived_assets' entry, and what
// get_frame_plan's initial_asset adds (derived_from, stale, stale_reasons).
// Paths are relative to `project_root` with forward slashes when inside it,
// absolute otherwise (and always when project_root is empty).
nlohmann::ordered_json derivedAssetJson(const DerivedAssetStatus &status,
                                        const std::filesystem::path &project_root);

// The manifest at `manifest` (throws std::runtime_error when it cannot be
// read or is not a pelican.import manifest). Its first output is the asset.
DerivedAssetStatus readDerivedAssetStatus(const std::filesystem::path &manifest);
// The status of `file` when `<file>.import.json` is there; nullopt when it is
// not, or cannot be read as a manifest.
std::optional<DerivedAssetStatus> derivedAssetStatusOf(const std::filesystem::path &file);
// Every readable `*.import.json` under the roots (recursively), in path order.
std::vector<DerivedAssetStatus> findDerivedAssets(const std::vector<std::filesystem::path> &roots);

} // namespace Pelican
