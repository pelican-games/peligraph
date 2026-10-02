#include "audioanalysisformat.hpp"

#include "assetsmanifest.hpp"
#include "graphcomposition.hpp"
#include "importmanifest.hpp"
#include "sha256stream.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <system_error>

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

[[noreturn]] void invalidSetting(std::string field, std::string reason, const Json *value = nullptr) {
    Json detail{{"field", std::move(field)}, {"reason", std::move(reason)}};
    if (value != nullptr) detail["value"] = *value;
    throw GraphRegistryError(std::string{audioAnalysisSettingsInvalidCode}, std::move(detail));
}

double settingNumber(const Json &object, const char *key, const std::string &field, double fallback, double min,
                     double max) {
    const auto found = object.find(key);
    if (found == object.end()) return fallback;
    if (!found->is_number() || !std::isfinite(found->get<double>()))
        invalidSetting(field, "must be a number", &*found);
    const auto value = found->get<double>();
    if (value < min || value > max)
        invalidSetting(field, "must be from " + Json(min).dump() + " to " + Json(max).dump(), &*found);
    return value;
}

void onlyKeys(const Json &object, std::initializer_list<std::string_view> allowed, const std::string &prefix) {
    for (const auto &[key, value] : object.items()) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            invalidSetting(prefix + key, "is not a setting");
    }
}

std::string relativeGeneric(const std::filesystem::path &path, const std::filesystem::path &base) {
    std::error_code error;
    auto relative = std::filesystem::relative(path, base, error);
    if (error || relative.empty()) return path.generic_string();
    return relative.generic_string();
}

// A file's sha256, hashed again only when its write time or size moved:
// get_frame_plan asks for every derived initial asset on every call, and a
// source may be a long song.
std::string fileSha256(const std::filesystem::path &path) {
    struct Entry {
        std::filesystem::file_time_type time;
        std::uintmax_t size = 0;
        std::string sha256;
    };
    static std::mutex mutex;
    static std::map<std::filesystem::path, Entry> cache;
    const auto time = std::filesystem::last_write_time(path);
    const auto size = std::filesystem::file_size(path);
    const auto key = path.lexically_normal();
    {
        std::lock_guard lock{mutex};
        if (const auto found = cache.find(key);
            found != cache.end() && found->second.time == time && found->second.size == size)
            return found->second.sha256;
    }
    auto sha256 = assetFileSha256(path);
    std::lock_guard lock{mutex};
    cache.insert_or_assign(key, Entry{time, size, sha256});
    return sha256;
}

std::filesystem::path besideManifest(const std::filesystem::path &manifest, const std::string &file) {
    const std::filesystem::path path{file};
    if (path.is_absolute()) return path.lexically_normal();
    return (manifest.parent_path() / path).lexically_normal();
}

} // namespace

AudioAnalysisSettings parseAudioAnalysisSettings(const Json &settings) {
    AudioAnalysisSettings result;
    if (settings.is_null()) return result;
    if (!settings.is_object()) invalidSetting("settings", "must be an object", &settings);
    onlyKeys(settings, {"features", "bands", "hop_ms", "window_ms", "smoothing", "channels"}, "");
    if (const auto features = settings.find("features"); features != settings.end()) {
        if (!features->is_array() || features->empty())
            invalidSetting("features", "must be a non-empty array of \"rms\", \"peak\" and \"bands\"", &*features);
        result.rms = result.peak = result.bands_enabled = false;
        std::set<std::string> seen;
        for (const auto &feature : *features) {
            if (!feature.is_string()) invalidSetting("features", "names a feature with a string", &feature);
            const auto name = feature.get<std::string>();
            if (!seen.insert(name).second) invalidSetting("features", "names a feature twice", &feature);
            if (name == "rms") result.rms = true;
            else if (name == "peak") result.peak = true;
            else if (name == "bands") result.bands_enabled = true;
            else invalidSetting("features", "is one of \"rms\", \"peak\" and \"bands\"", &feature);
        }
    }
    if (const auto bands = settings.find("bands"); bands != settings.end()) {
        if (!bands->is_number_integer() || bands->get<std::int64_t>() < 1 || bands->get<std::int64_t>() > 128)
            invalidSetting("bands", "must be an integer from 1 to 128", &*bands);
        result.bands = bands->get<std::uint32_t>();
    }
    result.hop_ms = settingNumber(settings, "hop_ms", "hop_ms", 10.0, 1.0, 1000.0);
    result.window_ms = settingNumber(settings, "window_ms", "window_ms", 2.0 * result.hop_ms, 1.0, 4000.0);
    if (const auto smoothing = settings.find("smoothing"); smoothing != settings.end()) {
        if (!smoothing->is_object()) invalidSetting("smoothing", "must be an object", &*smoothing);
        onlyKeys(*smoothing, {"attack_ms", "release_ms"}, "smoothing.");
        result.attack_ms = settingNumber(*smoothing, "attack_ms", "smoothing.attack_ms", 0.0, 0.0, 60000.0);
        result.release_ms = settingNumber(*smoothing, "release_ms", "smoothing.release_ms", 0.0, 0.0, 60000.0);
    }
    if (const auto channels = settings.find("channels"); channels != settings.end()) {
        if (!channels->is_string() || (*channels != "mix" && *channels != "left" && *channels != "right"))
            invalidSetting("channels", "is \"mix\", \"left\" or \"right\"", &*channels);
        result.channels = channels->get<std::string>();
    }
    return result;
}

OrderedJson audioAnalysisSettingsJson(const AudioAnalysisSettings &settings) {
    auto features = OrderedJson::array();
    if (settings.rms) features.push_back("rms");
    if (settings.peak) features.push_back("peak");
    if (settings.bands_enabled) features.push_back("bands");
    return {{"features", std::move(features)},
            {"bands", settings.bands},
            {"hop_ms", settings.hop_ms},
            {"window_ms", settings.window_ms},
            {"smoothing", {{"attack_ms", settings.attack_ms}, {"release_ms", settings.release_ms}}},
            {"channels", settings.channels}};
}

std::string audioAnalysisSettingsSha256(const AudioAnalysisSettings &settings) {
    return sha256Hex(audioAnalysisSettingsJson(settings).dump());
}

std::vector<std::string> audioAnalysisFeatureRows(const AudioAnalysisSettings &settings) {
    std::vector<std::string> rows;
    if (settings.rms) rows.emplace_back("rms");
    if (settings.peak) rows.emplace_back("peak");
    if (settings.bands_enabled)
        for (std::uint32_t band = 0; band < settings.bands; ++band) rows.push_back("band:" + std::to_string(band));
    return rows;
}

OrderedJson sampleTableLayoutJson(const SampleTableLayout &layout) {
    return {{"width", layout.width},
            {"rows_per_feature", layout.rows_per_feature},
            {"sample_count", layout.sample_count},
            {"hop_seconds", layout.hop_seconds},
            {"features", layout.features}};
}

SampleTableLayout parseSampleTableLayout(const Json &layout) {
    if (!layout.is_object()) throw std::invalid_argument("sample table layout must be an object");
    const auto count = [&](const char *key, std::uint64_t min) {
        const auto found = layout.find(key);
        if (found == layout.end() || !found->is_number_integer() || found->get<std::int64_t>() < 0 ||
            found->get<std::uint64_t>() < min || found->get<std::uint64_t>() > 0xFFFFFFFFull)
            throw std::invalid_argument(std::string{"sample table layout "} + key + " must be an integer of at least " +
                                        std::to_string(min));
        return found->get<std::uint32_t>();
    };
    SampleTableLayout result;
    result.width = count("width", 1);
    result.rows_per_feature = count("rows_per_feature", 1);
    result.sample_count = count("sample_count", 1);
    const auto hop = layout.find("hop_seconds");
    if (hop == layout.end() || !hop->is_number() || !std::isfinite(hop->get<double>()) || hop->get<double>() <= 0.0)
        throw std::invalid_argument("sample table layout hop_seconds must be a positive number");
    result.hop_seconds = hop->get<double>();
    const auto features = layout.find("features");
    if (features == layout.end() || !features->is_array() || features->empty())
        throw std::invalid_argument("sample table layout features must be a non-empty array of names");
    for (const auto &feature : *features) {
        if (!feature.is_string()) throw std::invalid_argument("sample table layout features must be names");
        result.features.push_back(feature.get<std::string>());
    }
    if (static_cast<std::uint64_t>(result.rows_per_feature) * result.width < result.sample_count)
        throw std::invalid_argument("sample table layout rows_per_feature * width is less than sample_count");
    return result;
}

std::optional<std::uint32_t> sampleTableRow(const SampleTableLayout &layout, const Json &row) {
    if (row.is_number_integer()) {
        if (row.get<std::int64_t>() < 0 || row.get<std::uint64_t>() >= layout.features.size()) return std::nullopt;
        return row.get<std::uint32_t>();
    }
    if (!row.is_string()) return std::nullopt;
    const auto found = std::find(layout.features.begin(), layout.features.end(), row.get<std::string>());
    if (found == layout.features.end()) return std::nullopt;
    return static_cast<std::uint32_t>(found - layout.features.begin());
}

double sampleTableValue(const SampleTableLayout &layout, const std::vector<float> &values, std::uint32_t feature,
                        double seconds) {
    if (layout.sample_count == 0 || feature >= layout.features.size()) return 0.0;
    const auto last = static_cast<double>(layout.sample_count - 1);
    const auto position = std::clamp(std::isfinite(seconds) ? seconds / layout.hop_seconds : 0.0, 0.0, last);
    const auto first = static_cast<std::uint32_t>(std::floor(position));
    const auto second = (std::min)(first + 1, layout.sample_count - 1);
    const auto at = [&](std::uint32_t sample) {
        const auto index = (static_cast<std::size_t>(feature) * layout.rows_per_feature + sample / layout.width) *
                               layout.width +
                           sample % layout.width;
        return index < values.size() ? static_cast<double>(values[index]) : 0.0;
    };
    const auto fraction = position - static_cast<double>(first);
    return at(first) + (at(second) - at(first)) * fraction;
}

std::filesystem::path derivedAssetManifestPath(const std::filesystem::path &file) {
    auto manifest = file;
    manifest += ".import.json";
    return manifest;
}

OrderedJson audioAnalysisManifest(const std::filesystem::path &source, const std::string &source_sha256,
                                  const AudioAnalysisSettings &settings, const SampleTableLayout &layout,
                                  const std::filesystem::path &table, const std::string &table_sha256) {
    const auto directory = derivedAssetManifestPath(table).parent_path();
    return {{"schema", "pelican.import"},
            {"version", 1},
            {"tool", {{"name", audioAnalysisToolName}, {"version", audioAnalysisToolVersion}}},
            {"source",
             {{"file", relativeGeneric(source, directory)},
              {"metadata",
               {{"sha256", source_sha256},
                {"settings", audioAnalysisSettingsJson(settings)},
                {"settings_sha256", audioAnalysisSettingsSha256(settings)},
                {"layout", sampleTableLayoutJson(layout)}}}}},
            {"outputs", OrderedJson::array({{{"file", table.filename().generic_string()},
                                             {"schema", audioAnalysisTableSchema},
                                             {"version", audioAnalysisTableSchemaVersion},
                                             {"sha256", table_sha256}}})}};
}

DerivedAssetStatus readDerivedAssetStatus(const std::filesystem::path &manifest_path) {
    std::ifstream input{manifest_path, std::ios::binary};
    if (!input) throw std::runtime_error("cannot read " + manifest_path.generic_string());
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    const auto document = Json::parse(text);
    const auto manifest = parseImportManifestJson(document);

    DerivedAssetStatus status;
    status.manifest = manifest_path.lexically_normal();
    status.output = besideManifest(manifest_path, manifest.outputs.front().file);
    status.source = besideManifest(manifest_path, manifest.source.file);
    status.tool_name = manifest.tool.name;
    status.tool_version = manifest.tool.version;
    const auto reason = [&](const char *why) { status.stale_reasons.emplace_back(why); };

    const auto &metadata = manifest.source.metadata.contains("metadata") ? manifest.source.metadata.at("metadata")
                                                                          : Json::object();
    std::error_code error;
    if (!manifest.source.file.empty()) {
        if (!std::filesystem::is_regular_file(status.source, error)) reason("source_missing");
        else if (const auto sha = metadata.find("sha256"); sha != metadata.end() && sha->is_string() &&
                                                             fileSha256(status.source) != sha->get<std::string>())
            reason("source_changed");
    }
    for (const auto &output : manifest.outputs) {
        const auto file = besideManifest(manifest_path, output.file);
        if (!std::filesystem::is_regular_file(file, error)) {
            reason("output_missing");
            break;
        }
        if (fileSha256(file) != output.sha256) {
            reason("output_changed");
            break;
        }
    }
    if (manifest.tool.name == audioAnalysisToolName) {
        if (manifest.tool.version != std::string{audioAnalysisToolVersion}) reason("tool_version");
        bool settings_match = false;
        try {
            const auto recorded = metadata.find("settings_sha256");
            settings_match = recorded != metadata.end() && recorded->is_string() &&
                             audioAnalysisSettingsSha256(parseAudioAnalysisSettings(metadata.value("settings", Json{}))) ==
                                 recorded->get<std::string>();
        } catch (const std::exception &) {
        }
        if (!settings_match) reason("settings_changed");
    }
    status.stale = !status.stale_reasons.empty();
    return status;
}

OrderedJson derivedAssetJson(const DerivedAssetStatus &status, const std::filesystem::path &project_root) {
    const auto spell = [&](const std::filesystem::path &path) {
        if (!project_root.empty()) {
            std::error_code error;
            const auto relative = std::filesystem::relative(path, project_root, error);
            if (!error && !relative.empty() && *relative.begin() != "..") return relative.generic_string();
        }
        return path.generic_string();
    };
    OrderedJson tool{{"name", status.tool_name}};
    tool["version"] = status.tool_version ? OrderedJson(*status.tool_version) : OrderedJson(nullptr);
    return {{"asset", spell(status.output)},
            {"manifest", spell(status.manifest)},
            {"tool", tool},
            {"source", spell(status.source)},
            {"derived_from", {{"source", spell(status.source)}, {"tool", tool}}},
            {"stale", status.stale},
            {"stale_reasons", status.stale_reasons}};
}

std::optional<DerivedAssetStatus> derivedAssetStatusOf(const std::filesystem::path &file) {
    const auto manifest = derivedAssetManifestPath(file);
    std::error_code error;
    if (!std::filesystem::is_regular_file(manifest, error)) return std::nullopt;
    try {
        return readDerivedAssetStatus(manifest);
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

std::vector<DerivedAssetStatus> findDerivedAssets(const std::vector<std::filesystem::path> &roots) {
    std::set<std::filesystem::path> manifests;
    for (const auto &root : roots) {
        std::error_code error;
        if (!std::filesystem::is_directory(root, error)) continue;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 root, std::filesystem::directory_options::skip_permission_denied, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            const auto name = it->path().filename().string();
            constexpr std::string_view suffix = ".import.json";
            if (name.size() > suffix.size() && name.ends_with(suffix) && it->is_regular_file(error))
                manifests.insert(it->path().lexically_normal());
        }
    }
    std::vector<DerivedAssetStatus> result;
    for (const auto &manifest : manifests) {
        try {
            result.push_back(readDerivedAssetStatus(manifest));
        } catch (const std::exception &) {
        }
    }
    return result;
}

} // namespace Pelican
