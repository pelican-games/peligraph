// AB-1s: the baked analysis tables, read from the project as the engine
// writes them (audioanalysisformat.hpp), and the lines Studio says of them.

#include "audioanalysistables.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

namespace PelicanStudio {
namespace {
using Json = nlohmann::json;

std::string genericOf(const std::filesystem::path &path) {
    const auto text = path.generic_u8string();
    return std::string{text.begin(), text.end()};
}

std::filesystem::path pathOf(std::string_view text) {
    return std::filesystem::path{
        std::u8string{reinterpret_cast<const char8_t *>(text.data()), text.size()}};
}

std::optional<Json> readJson(const std::filesystem::path &file) {
    std::ifstream input{file, std::ios::binary};
    if (!input) return std::nullopt;
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto parsed = Json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return std::nullopt;
    return parsed;
}

// The table a manifest describes, when it is a bake's.
std::optional<GraphCanvasAnalysisTable> tableOf(const std::filesystem::path &project_root,
                                                const std::filesystem::path &manifest_path) {
    const auto manifest = readJson(manifest_path);
    if (!manifest) return std::nullopt;
    const auto tool = manifest->value("tool", Json::object());
    if (!tool.is_object() || tool.value("name", std::string{}) != Pelican::audioAnalysisToolName)
        return std::nullopt;
    const auto source = manifest->value("source", Json::object());
    if (!source.is_object()) return std::nullopt;
    const auto metadata = source.value("metadata", Json::object());
    if (!metadata.is_object()) return std::nullopt;
    GraphCanvasAnalysisTable table;
    try {
        table.layout = Pelican::parseSampleTableLayout(metadata.value("layout", Json{}));
    } catch (const std::exception &) {
        return std::nullopt;
    }
    const auto settings = metadata.find("settings");
    if (settings != metadata.end() && settings->is_object()) table.settings = *settings;
    const auto directory = manifest_path.parent_path();
    const auto relative = [&](const std::filesystem::path &file) {
        const auto inside = file.lexically_normal().lexically_relative(project_root.lexically_normal());
        const auto spelled = genericOf(inside);
        return spelled.empty() || spelled.starts_with("..") ? genericOf(file.lexically_normal()) : spelled;
    };
    const auto outputs = manifest->value("outputs", Json::array());
    std::string output;
    if (outputs.is_array() && !outputs.empty() && outputs.front().is_object())
        output = outputs.front().value("file", std::string{});
    // The table is the manifest's name without ".import.json" when the
    // outputs do not say.
    if (output.empty()) {
        const auto name = genericOf(manifest_path.filename());
        output = name.substr(0, name.size() - std::string_view{".import.json"}.size());
    }
    const auto table_file = directory / pathOf(output);
    table.asset = relative(table_file);
    table.manifest = relative(manifest_path);
    const auto source_file = source.value("file", std::string{});
    if (!source_file.empty()) table.source = relative(directory / pathOf(source_file));
    return table;
}

} // namespace

std::string projectRelativeReference(std::string_view reference) {
    constexpr std::string_view scheme = "project://";
    if (reference.starts_with(scheme)) reference.remove_prefix(scheme.size());
    return std::string{reference};
}

std::optional<GraphCanvasAnalysisTable> readAnalysisTable(const std::filesystem::path &project_root,
                                                          std::string_view asset) {
    if (project_root.empty() || asset.empty()) return std::nullopt;
    const auto relative = projectRelativeReference(asset);
    return tableOf(project_root, project_root / pathOf(relative + ".import.json"));
}

std::vector<GraphCanvasAnalysisTable> readProjectAnalysisTables(const std::filesystem::path &project_root) {
    std::vector<GraphCanvasAnalysisTable> tables;
    if (project_root.empty()) return tables;
    std::error_code error;
    std::filesystem::recursive_directory_iterator walk{
        project_root, std::filesystem::directory_options::skip_permission_denied, error};
    if (error) return tables;
    try {
        for (const std::filesystem::recursive_directory_iterator end; walk != end; walk.increment(error)) {
            if (error) break;
            const auto &entry = *walk;
            std::error_code kind;
            if (entry.is_directory(kind)) {
                const auto name = genericOf(entry.path().filename());
                if (!name.empty() && name.front() == '.') walk.disable_recursion_pending();
                continue;
            }
            const auto name = genericOf(entry.path().filename());
            if (!name.ends_with(".import.json")) continue;
            if (auto table = tableOf(project_root, entry.path())) tables.push_back(std::move(*table));
        }
    } catch (const std::exception &) {
    }
    std::ranges::sort(tables, {}, &GraphCanvasAnalysisTable::asset);
    return tables;
}

std::string defaultAnalysisOut(std::string_view sound) {
    const auto relative = projectRelativeReference(sound);
    auto stem = genericOf(pathOf(relative).stem());
    if (stem.empty()) stem = "analysis";
    return "assets/analysis/" + stem + ".ktx2";
}

Json audioBakeSettings(const AudioBakeChoice &choice) {
    auto features = Json::array();
    if (choice.rms) features.push_back("rms");
    if (choice.peak) features.push_back("peak");
    if (choice.bands) features.push_back("bands");
    Json settings{{"features", std::move(features)},
                  {"hop_ms", choice.hop_ms},
                  {"smoothing", {{"attack_ms", choice.attack_ms}, {"release_ms", choice.release_ms}}},
                  {"channels", choice.channels}};
    if (choice.bands) settings["bands"] = choice.band_count;
    return settings;
}

std::string analysisFeaturesText(const std::vector<std::string> &features) {
    std::vector<std::string> parts;
    std::vector<std::string> bands;
    for (const auto &feature : features)
        if (feature.starts_with("band:")) bands.push_back(feature);
        else parts.push_back(feature);
    if (bands.size() > 2) parts.push_back(bands.front() + ".." + bands.back());
    else parts.insert(parts.end(), bands.begin(), bands.end());
    std::string text;
    for (const auto &part : parts) text += (text.empty() ? "" : ", ") + part;
    return text;
}

std::string bakedLine(const Json &committed) {
    const auto out = committed.is_object() ? committed.value("out", std::string{}) : std::string{};
    const auto layout = committed.is_object() ? committed.value("layout", Json::object()) : Json::object();
    std::vector<std::string> features;
    std::uint64_t samples = 0;
    if (layout.is_object()) {
        for (const auto &feature : layout.value("features", Json::array()))
            if (feature.is_string()) features.push_back(feature.get<std::string>());
        if (const auto count = layout.find("sample_count"); count != layout.end() && count->is_number_unsigned())
            samples = count->get<std::uint64_t>();
    }
    return "Baked " + projectRelativeReference(out) + ": " + analysisFeaturesText(features) +
           " \xC3\x97 " + std::to_string(samples) + " samples";
}

std::vector<GraphCanvasDerivedAsset> parseDerivedAssets(const Json &list) {
    std::vector<GraphCanvasDerivedAsset> assets;
    if (!list.is_array()) return assets;
    for (const auto &entry : list) {
        if (!entry.is_object()) continue;
        GraphCanvasDerivedAsset asset;
        asset.asset = projectRelativeReference(entry.value("asset", std::string{}));
        if (asset.asset.empty()) continue;
        asset.manifest = entry.value("manifest", std::string{});
        asset.source = entry.value("source", std::string{});
        if (const auto tool = entry.find("tool"); tool != entry.end() && tool->is_object())
            asset.tool = tool->value("name", std::string{});
        asset.stale = entry.value("stale", false);
        for (const auto &reason : entry.value("stale_reasons", Json::array()))
            if (reason.is_string()) asset.stale_reasons.push_back(reason.get<std::string>());
        assets.push_back(std::move(asset));
    }
    return assets;
}

std::string staleBandText(const GraphCanvasDerivedAsset &asset) {
    std::string reasons;
    for (const auto &reason : asset.stale_reasons) reasons += (reasons.empty() ? "" : ", ") + reason;
    return "stale: " + (reasons.empty() ? std::string{"yes"} : reasons);
}

Json rebakeParams(const std::filesystem::path &project_root, std::string_view asset) {
    const auto table = readAnalysisTable(project_root, asset);
    if (!table || table->source.empty()) return nullptr;
    Json params{{"asset", "project://" + table->source}, {"out", table->asset}};
    if (table->settings.is_object() && !table->settings.empty()) params["settings"] = table->settings;
    return params;
}

std::string sampleTableShaderLine(const std::string &image, const std::string &layout, std::size_t feature) {
    const auto variable = "pelican_resource_" + layout + ".values[0]";
    return "#include \"pelican_sample_table.glsl\"  // then: vec4 l = " + variable +
           "; float v = pelican_sample_table(" + image + ", " + std::to_string(feature) +
           "u, uint(l.x), uint(l.y), l.z, l.w);";
}

} // namespace PelicanStudio
