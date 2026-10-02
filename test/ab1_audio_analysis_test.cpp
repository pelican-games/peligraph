// AB-1: a sound's analysis baked into a table (`pelican_cli audio analyze`,
// bake_audio_analysis), read back by value.sample_table, and judged stale by
// list_derived_assets once its source changes.
#include "../src/core/communication/renderconfigeditor.hpp"
#include "../src/core/loader/ktx2.hpp"
#include "../src/core/loader/sampletable.hpp"
#include "../src/core/userpublic/valuegraph.hpp"
#include "../src/devcli/audiocommand.hpp"
#include "../src/project/audioanalysisformat.hpp"
#include "../src/project/graphcomposition.hpp"
#include "../src/project/importmanifest.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <numbers>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace Pelican {
namespace {

using Json = nlohmann::json;

std::filesystem::path freshDirectory(std::string_view name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = std::filesystem::temp_directory_path() /
                     ("pelican_" + std::string{name} + "_" + std::to_string(stamp));
    std::filesystem::create_directories(dir);
    return dir;
}

std::string readText(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void writeText(const std::filesystem::path &path, const std::string &text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(output.good());
}

// One second of 16-bit mono at 48 kHz: `frequency` Hz at half scale for the
// first half, silence for the second.
std::string toneThenSilenceWav(double frequency) {
    constexpr std::uint32_t rate = 48000;
    std::vector<std::int16_t> samples(rate, 0);
    for (std::uint32_t i = 0; i < rate / 2; ++i)
        samples[i] = static_cast<std::int16_t>(
            std::lround(0.5 * 32767.0 * std::sin(2.0 * std::numbers::pi * frequency * i / rate)));
    std::string wav;
    const auto u32 = [&](std::uint32_t value) {
        for (int byte = 0; byte < 4; ++byte) wav.push_back(static_cast<char>((value >> (8 * byte)) & 0xFF));
    };
    const auto u16 = [&](std::uint16_t value) {
        wav.push_back(static_cast<char>(value & 0xFF));
        wav.push_back(static_cast<char>(value >> 8));
    };
    const auto data_bytes = static_cast<std::uint32_t>(samples.size() * 2);
    wav += "RIFF";
    u32(36 + data_bytes);
    wav += "WAVEfmt ";
    u32(16);
    u16(1);
    u16(1);
    u32(rate);
    u32(rate * 2);
    u16(2);
    u16(16);
    wav += "data";
    u32(data_bytes);
    for (const auto sample : samples) u16(static_cast<std::uint16_t>(sample));
    return wav;
}

int runCli(std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(), "audio");
    std::vector<char *> argv;
    for (auto &argument : arguments) argv.push_back(argument.data());
    return DevCli::runAudioCommand(static_cast<int>(argv.size()), argv.data());
}

// A table written by hand: two features over three samples half a second
// apart, a = {0, 1, 4} and b = {10, 20, 30}.
std::filesystem::path handTable(const std::filesystem::path &dir) {
    SampleTableLayout layout;
    layout.rows_per_feature = 1;
    layout.sample_count = 3;
    layout.hop_seconds = 0.5;
    layout.features = {"a", "b"};
    std::vector<float> values(sampleTableWidth * 2, 0.0f);
    values[0] = 0.0f;
    values[1] = 1.0f;
    values[2] = 4.0f;
    values[sampleTableWidth + 0] = 10.0f;
    values[sampleTableWidth + 1] = 20.0f;
    values[sampleTableWidth + 2] = 30.0f;
    const auto file = writeKtx2(Ktx2Format::R32Sfloat, sampleTableWidth, 2, std::as_bytes(std::span{values}));
    const auto table = dir / "hand.ktx2";
    writeText(table, std::string{reinterpret_cast<const char *>(file.data()), file.size()});
    writeText(derivedAssetManifestPath(table),
              Json{{"source", {{"metadata", {{"layout", Json(sampleTableLayoutJson(layout))}}}}}}.dump());
    return table;
}

Json sampleGraph(const std::string &asset, const Json &row, bool time_edge) {
    Json nodes = Json::array();
    Json reader{{"id", "reader"}, {"op", "value.sample_table"}, {"parameters", {{"asset", asset}, {"row", row}}},
                {"outputs", Json::array({"value"})}};
    if (time_edge) {
        nodes.push_back({{"id", "clock"}, {"op", "value.input"}, {"parameters", {{"name", "time"}}},
                         {"outputs", Json::array({"value"})}});
        reader["inputs"] = {{"time", {{"node", "clock"}, {"port", "value"}}}};
    }
    nodes.push_back(reader);
    return {{"schema", "pelican.graph"}, {"version", 1}, {"nodes", nodes},
            {"outputs", {{"level", {{"node", "reader"}, {"port", "value"}}}}}};
}

double evaluate(const Json &graph, double time, double scene_time) {
    const ValueGraph::Graph built{graph.dump()};
    return built.evaluate({{"time", time}, {"scene_time", scene_time}}).at("level").at(0);
}

std::string errorCode(const std::function<void()> &build) {
    try {
        build();
    } catch (const GraphRegistryError &error) {
        return error.code();
    }
    return "none";
}

} // namespace

TEST_CASE("AB-1 a bake of 440 Hz then silence: rms and peak follow the tone and the band holding 440 Hz leads",
          "[audio][ab1]") {
    const auto dir = freshDirectory("ab1_bake");
    writeText(dir / "tone.wav", toneThenSilenceWav(440.0));
    REQUIRE(runCli({"analyze", "--asset", (dir / "tone.wav").string(), "--out", (dir / "tone.ktx2").string()}) == 0);

    // The manifest beside it is a pelican.import manifest the engine reads.
    const auto manifest_text = readText(dir / "tone.ktx2.import.json");
    const auto manifest_json = Json::parse(manifest_text);
    const auto manifest = parseImportManifestJson(manifest_json);
    CHECK(manifest.tool.name == "pelican.audio_analysis");
    CHECK(manifest.tool.version == std::optional<std::string>{"1"});
    CHECK(manifest.source.file == "tone.wav");
    REQUIRE(manifest.outputs.size() == 1);
    CHECK(manifest.outputs[0].file == "tone.ktx2");
    CHECK(manifest.outputs[0].schema == "pelican.audio_analysis_table");
    CHECK(manifest.outputs[0].version == std::optional<int>{1});
    const auto &metadata = manifest_json.at("source").at("metadata");
    CHECK(metadata.at("sha256").get<std::string>().size() == 64);
    CHECK(metadata.at("settings") ==
          Json{{"features", {"rms", "peak", "bands"}}, {"bands", 8}, {"hop_ms", 10.0}, {"window_ms", 20.0},
               {"smoothing", {{"attack_ms", 0.0}, {"release_ms", 0.0}}}, {"channels", "mix"}});
    const auto &layout_json = metadata.at("layout");
    CHECK(layout_json.at("width") == 4096);
    CHECK(layout_json.at("sample_count") == 101);
    CHECK(layout_json.at("rows_per_feature") == 1);
    CHECK(layout_json.at("hop_seconds") == 0.01);
    CHECK(layout_json.at("features") ==
          Json::array({"rms", "peak", "band:0", "band:1", "band:2", "band:3", "band:4", "band:5", "band:6", "band:7"}));

    const auto table = loadSampleTable(dir / "tone.ktx2");
    const auto row = [&](const char *name) { return *sampleTableRow(table->layout, name); };
    for (const double t : {0.05, 0.15, 0.25, 0.35, 0.45}) {
        INFO(t);
        // A half-scale sine: rms 0.5 / sqrt 2, peak 0.5.
        CHECK_THAT(table->sample(row("rms"), t), Catch::Matchers::WithinAbs(0.3536, 0.01));
        CHECK_THAT(table->sample(row("peak"), t), Catch::Matchers::WithinAbs(0.5, 0.01));
        // 440 Hz is in band 3 of 8 log bands from 20 Hz to 24 kHz
        // ([286, 693) Hz), and that band leads by far.
        const auto lead = table->sample(row("band:3"), t);
        CHECK(lead > 0.4);
        for (std::uint32_t band = 0; band < 8; ++band)
            if (band != 3) CHECK(table->sample(row(("band:" + std::to_string(band)).c_str()), t) < lead * 0.2);
    }
    for (const double t : {0.55, 0.7, 0.85, 1.0}) {
        INFO(t);
        CHECK(table->sample(row("rms"), t) < 1e-6);
        CHECK(table->sample(row("band:3"), t) < 1e-6);
    }

    // The same bytes and settings write the same files.
    const auto table_bytes = readText(dir / "tone.ktx2");
    REQUIRE(runCli({"analyze", "--asset", (dir / "tone.wav").string(), "--out", (dir / "tone.ktx2").string()}) == 0);
    CHECK(readText(dir / "tone.ktx2") == table_bytes);
    CHECK(readText(dir / "tone.ktx2.import.json") == manifest_text);

    // The options are the RPC's settings: rms alone, 4 bands, 20 ms.
    REQUIRE(runCli({"analyze", "--asset", (dir / "tone.wav").string(), "--out", (dir / "few.ktx2").string(),
                    "--features", "rms,bands", "--bands", "4", "--hop-ms", "20", "--channels", "left"}) == 0);
    const auto few = loadSampleTable(dir / "few.ktx2");
    CHECK(few->layout.features == std::vector<std::string>{"rms", "band:0", "band:1", "band:2", "band:3"});
    CHECK(few->layout.sample_count == 51);
    CHECK(few->layout.hop_seconds == 0.02);
    // A setting out of range is refused and nothing is written.
    CHECK(runCli({"analyze", "--asset", (dir / "tone.wav").string(), "--out", (dir / "bad.ktx2").string(), "--bands",
                  "0"}) != 0);
    CHECK_FALSE(std::filesystem::exists(dir / "bad.ktx2"));

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

TEST_CASE("AB-1 value.sample_table reads a row by name or index at its time edge, or at scene_time without one",
          "[audio][ab1][graph-composition]") {
    const auto dir = freshDirectory("ab1_read");
    const auto table = handTable(dir).generic_string();

    // Between two samples, linearly; outside, the end's value.
    CHECK(evaluate(sampleGraph(table, "a", true), 0.25, 99.0) == 0.5);
    CHECK(evaluate(sampleGraph(table, "a", true), 0.75, 99.0) == 2.5);
    CHECK(evaluate(sampleGraph(table, "a", true), -1.0, 99.0) == 0.0);
    CHECK(evaluate(sampleGraph(table, "a", true), 10.0, 99.0) == 4.0);
    CHECK(evaluate(sampleGraph(table, 1, true), 0.5, 99.0) == 20.0);
    // No time edge: scene_time, whatever `time` is.
    CHECK(evaluate(sampleGraph(table, "b", false), 99.0, 0.25) == 15.0);
    const ValueGraph::Graph scene_clocked{sampleGraph(table, "b", false).dump()};
    CHECK(scene_clocked.inputNames() == std::set<std::string>{"scene_time"});

    // Refused by name: a row the table does not have, a table without its
    // manifest.
    CHECK(errorCode([&] { (void)ValueGraph::Graph{sampleGraph(table, "band:3", true).dump()}; }) ==
          "value.sample_table_row_unknown@1");
    CHECK(errorCode([&] { (void)ValueGraph::Graph{sampleGraph(table, 2, true).dump()}; }) ==
          "value.sample_table_row_unknown@1");
    std::filesystem::copy_file(dir / "hand.ktx2", dir / "bare.ktx2");
    CHECK(errorCode([&] {
              (void)ValueGraph::Graph{sampleGraph((dir / "bare.ktx2").generic_string(), "a", true).dump()};
          }) == "value.sample_table_unreadable@1");

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

TEST_CASE("AB-1 bake_audio_analysis runs on a ticket, and list_derived_assets calls the table stale once its "
          "source or its settings change",
          "[audio][ab1][render-config-editor]") {
    const auto dir = freshDirectory("ab1_rpc");
    const std::string root =
        R"({"schema": "pelican.graph", "version": 1, "definitions": {}, "nodes": [], "outputs": {}})"
        "\n";
    writeText(dir / "passes" / "main.json", root);
    writeText(dir / "sounds" / "tone.wav", toneThenSilenceWav(440.0));
    RenderConfigEditorService service{RenderConfigEditorDependencies{
        .source_reference = "passes/main.json",
        .source_path = dir / "passes" / "main.json",
        .source_bytes = root,
        .project_root = dir,
        .gate = [] { return RenderConfigEditorGateObservation{}; },
        .runtime_snapshot = [] { return RenderConfigRuntimeSnapshot{}; },
        .apply_candidate = [](RenderConfigCandidateDocumentSet, RenderConfigSourceCommit,
                              RenderConfigRuntimeApplyCompletion) { return RenderConfigRuntimeApplyResult{}; },
        .feature_catalog = [] { return std::vector<RenderFeatureCatalogEntry>{}; },
    }};
    const auto await = [&](const nlohmann::ordered_json &accepted) {
        INFO(accepted.dump());
        REQUIRE(accepted.at("status") == "accepted");
        for (int attempt = 0; attempt < 2000; ++attempt) {
            service.commitPending();
            auto result = Json(service.getResult({{"ticket", accepted.at("ticket")}}));
            if (result.at("status") != "running") return result;
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        FAIL("bake_audio_analysis did not end");
        return Json{};
    };

    const auto accepted = service.bakeAudioAnalysis(
        {{"asset", "sounds/tone.wav"}, {"out", "tables/tone.ktx2"}, {"settings", {{"bands", 4}}}});
    CHECK(accepted.at("asset") == "sounds/tone.wav");
    CHECK(accepted.at("out") == "tables/tone.ktx2");
    const auto result = await(accepted);
    INFO(result.dump());
    REQUIRE(result.at("status") == "committed");
    CHECK(result.at("committed") == true);
    CHECK(result.at("operation") == "bake_audio_analysis");
    CHECK(result.at("out") == "tables/tone.ktx2");
    CHECK(result.at("manifest") == "tables/tone.ktx2.import.json");
    CHECK(result.at("layout") == Json{{"width", 4096}, {"rows_per_feature", 1}, {"sample_count", 101},
                                      {"hop_seconds", 0.01},
                                      {"features", {"rms", "peak", "band:0", "band:1", "band:2", "band:3"}}});
    CHECK(std::filesystem::is_regular_file(dir / "tables" / "tone.ktx2"));

    const auto listed = [&] { return Json(service.listDerivedAssets(Json::object())); };
    auto list = listed();
    INFO(list.dump());
    REQUIRE(list.size() == 1);
    CHECK(list[0].at("asset") == "tables/tone.ktx2");
    CHECK(list[0].at("manifest") == "tables/tone.ktx2.import.json");
    CHECK(list[0].at("source") == "sounds/tone.wav");
    CHECK(list[0].at("tool") == Json{{"name", "pelican.audio_analysis"}, {"version", "1"}});
    CHECK(list[0].at("derived_from") == Json{{"source", "sounds/tone.wav"}, {"tool", list[0].at("tool")}});
    CHECK(list[0].at("stale") == false);
    CHECK(list[0].at("stale_reasons") == Json::array());
    // The same status initial_asset reports, from the table's file.
    const auto status = derivedAssetStatusOf(dir / "tables" / "tone.ktx2");
    REQUIRE(status.has_value());
    CHECK_FALSE(status->stale);

    // Another sound in the source's place: stale.
    writeText(dir / "sounds" / "tone.wav", toneThenSilenceWav(1000.0));
    list = listed();
    CHECK(list[0].at("stale") == true);
    CHECK(list[0].at("stale_reasons") == Json::array({"source_changed"}));
    // Baked again: not stale.
    CHECK(await(service.bakeAudioAnalysis({{"asset", "sounds/tone.wav"}, {"out", "tables/tone.ktx2"},
                                           {"settings", {{"bands", 4}}}}))
              .at("status") == "committed");
    CHECK(listed()[0].at("stale") == false);
    // Settings asked for in the manifest that the table was not baked with.
    auto manifest = Json::parse(readText(dir / "tables" / "tone.ktx2.import.json"));
    manifest["source"]["metadata"]["settings"]["bands"] = 16;
    writeText(dir / "tables" / "tone.ktx2.import.json", manifest.dump(2));
    CHECK(listed()[0].at("stale_reasons") == Json::array({"settings_changed"}));

    // Refused at once: a setting it does not take.
    const auto refused = Json(service.bakeAudioAnalysis(
        {{"asset", "sounds/tone.wav"}, {"out", "tables/x.ktx2"}, {"settings", {{"hop_ms", 0}}}}));
    CHECK(refused.at("status") == "rejected");
    CHECK(refused.at("error").at("code") == "audio.analysis_settings_invalid@1");
    CHECK(refused.at("error").at("field") == "hop_ms");
    // Refused on the ticket: a file that is not WAV or Ogg Vorbis.
    writeText(dir / "sounds" / "tone.mp3", "ID3 not a sound");
    const auto unsupported = await(
        service.bakeAudioAnalysis({{"asset", "sounds/tone.mp3"}, {"out", "tables/mp3.ktx2"}}));
    CHECK(unsupported.at("status") == "rejected");
    CHECK(unsupported.at("error").at("code") == "audio.format_unsupported@1");
    CHECK_FALSE(std::filesystem::exists(dir / "tables" / "mp3.ktx2"));

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

} // namespace Pelican
