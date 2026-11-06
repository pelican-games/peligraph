#include "../src/core/os/liveinputrecording.hpp"
#include "../src/core/os/liveinputrecordingbake.hpp"
#include "../src/core/communication/renderconfigeditor.hpp"
#include "../src/core/loader/sampletable.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/communication/graphfoldverify.hpp"
#include "../src/core/renderingpass/graphinputs.hpp"
#include "../src/core/watch/problemlist.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>

namespace Pelican {
namespace {
using Json = nlohmann::json;
using Catch::Matchers::ContainsSubstring;

struct TempDir {
    std::filesystem::path root;
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

TempDir tempDir() {
    return {std::filesystem::temp_directory_path() /
            ("pelican_live_recording_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
}

std::string header(Json inputs = Json::array({
    Json{{"name", "phase"}, {"type", "float"}, {"default", 0.0}},
})) {
    return Json{{"schema", "pelican.live_seq"}, {"version", 1},
                {"inputs", std::move(inputs)}, {"clock", "fixed"},
                {"fixed_step", 1.0 / 60.0}, {"start_frame", 1},
                {"start_time", 1.0 / 60.0}}.dump();
}

std::string completeRecording() {
    return header() + "\n" +
        Json{{"frame", 1}, {"time", 1.0 / 60.0},
             {"values", Json{{"phase", 0.25}}}}.dump() + "\n" +
        Json{{"end", true}, {"frame", 1}, {"time", 1.0 / 60.0},
             {"evaluations", 1}}.dump() + "\n";
}

void requireInvalid(std::string_view text, std::string_view detail,
                    std::string_view code = "live_input.recording_invalid@1") {
    try {
        (void)LiveInputRecording::fromJsonLines(text);
        FAIL("recording was accepted: " << detail);
    } catch (const std::exception &error) {
        const std::string message{error.what()};
        CHECK_THAT(message, ContainsSubstring(std::string{code}));
        CHECK_THAT(message, ContainsSubstring(std::string{detail}));
    }
}

LiveInputRecording lookupRecording() {
    LiveInputRecording recording;
    recording.inputs = {{"phase", LiveInputType::floating, 0.0, {}, {}},
                        {"enabled", LiveInputType::boolean, 0.0, {}, {}}};
    recording.frames = {
        {10, 1.0, {{"phase", 1.0}, {"enabled", 0.0}}},
        {11, 2.0, {{"phase", 2.0}}},
        {12, 2.0, {{"phase", 3.0}}},
        {13, 4.0, {{"phase", 4.0}}},
    };
    return recording;
}

double value(const std::map<std::string, double, std::less<>> &values,
             std::string_view name) {
    return values.at(std::string{name});
}
} // namespace

TEST_CASE("OSC-4a live recording preserves full precision and sparse declarations",
          "[osc4a][live_recording]") {
    LiveInputRecording recording;
    recording.clock.start_frame = 7;
    recording.clock.start_time = 0.125;
    recording.inputs = {
        {"phase", LiveInputType::floating, 0.0, -2.0, 2.0},
        {"count", LiveInputType::integer, 1.0, 0.0, 8.0},
        {"enabled", LiveInputType::boolean, 0.0, {}, {}},
    };
    const double exact = std::nextafter(1.0, 2.0);
    recording.frames = {{7, 0.125,
        {{"phase", exact}, {"count", 1.0}, {"enabled", 1.0}}},
        {8, 0.25, {{"phase", 0.5}}},
        {9, 0.375, {{"count", 2.0}, {"enabled", 0.0}}}};
    recording.end = {9, 0.375, 3};

    const auto text = recording.toJsonLines();
    const auto decoded = LiveInputRecording::fromJsonLines(text);
    REQUIRE(decoded.frames.size() == 3);
    CHECK(decoded.frames[0].values.at("phase") == exact);
    CHECK(decoded.frames[1].values.size() == 1);
    CHECK(decoded.frames[1].values.contains("phase"));
    CHECK(decoded.frames[2].values.size() == 2);
    CHECK(decoded.end.evaluations == 3);
    CHECK(decoded.inputs[0].minimum == -2.0);
    CHECK(decoded.inputs[1].maximum == 8.0);

    LiveInputRecording empty;
    empty.inputs = {{"unused", LiveInputType::floating, 0.0, {}, {}}};
    empty.end = {};
    CHECK_THROWS_WITH(empty.toJsonLines(),
                      ContainsSubstring("live_input.recording_incomplete@1"));
}

TEST_CASE("OSC-4a time and relative-frame lookup hold values without interpolation [osc4a]",
          "[osc4a][live_recording]") {
    const auto recording = lookupRecording();
    CHECK(value(recording.valuesAtTime(0.0), "phase") == 1.0);
    CHECK(value(recording.valuesAtTime(1.0), "phase") == 1.0);
    CHECK(value(recording.valuesAtTime(1.5), "phase") == 1.0);
    CHECK(value(recording.valuesAtTime(2.0), "phase") == 3.0);
    CHECK(value(recording.valuesAtTime(3.999), "phase") == 3.0);
    CHECK(value(recording.valuesAtTime(4.0), "phase") == 4.0);
    CHECK(value(recording.valuesAtTime(100.0), "phase") == 4.0);
    CHECK(value(recording.valuesAtTime(100.0), "enabled") == 0.0);

    CHECK(value(recording.valuesAtFrame(9), "phase") == 1.0);
    CHECK(value(recording.valuesAtFrame(11), "phase") == 2.0);
    CHECK(value(recording.valuesAtFrame(12), "phase") == 3.0);
    CHECK(value(recording.valuesAtFrame(100), "phase") == 4.0);
}

TEST_CASE("OSC-4a runtime selects frame lookup only for identical fixed clocks [osc4a]",
          "[osc4a][live_recording]") {
    FastModuleContainer modules;
    auto recording = std::make_shared<LiveInputRecording>(lookupRecording());
    recording->clock = {LiveRecordingClockMode::fixed, 0.5, 10, 1.0, 0};
    auto &runtime = GET_MODULE(LiveInputRecordingRuntime);
    runtime.startReplay(recording,
        {LiveRecordingClockMode::fixed, 0.5, 20, 1.0, 0});
    CHECK(value(runtime.replayValues(21, 2.0), "phase") == 2.0);
    CHECK(value(runtime.replayValues(22, 2.0), "phase") == 3.0);
    runtime.stopReplay();

    runtime.startReplay(recording,
        {LiveRecordingClockMode::fixed, 0.25, 20, 1.0, 0});
    CHECK(value(runtime.replayValues(21, 2.0), "phase") == 3.0);
    CHECK(value(runtime.replayValues(22, 1.5), "phase") == 1.0);
    runtime.stopReplay();

    recording->clock.mode = LiveRecordingClockMode::realtime;
    runtime.startReplay(recording,
        {LiveRecordingClockMode::fixed, 0.5, 20, 1.0, 0});
    CHECK(value(runtime.replayValues(22, 2.0), "phase") == 3.0);
    CHECK(value(runtime.replayValues(22, 1.5), "phase") == 1.0);
    runtime.stopReplay();
}

TEST_CASE("OSC-4a outside clock changes stop recording and replay [osc4a]",
          "[osc4a][live_recording]") {
    FastModuleContainer modules;
    (void)GET_MODULE(ProblemList);
    const auto dir = tempDir();
    std::filesystem::create_directories(dir.root);
    const LiveRecordingClock initial{LiveRecordingClockMode::fixed, 1.0 / 60.0,
                                     1, 1.0 / 60.0, 4};
    auto &runtime = GET_MODULE(LiveInputRecordingRuntime);
    runtime.startRecording(dir.root / "clock.live_seq", {}, initial);
    CHECK_THROWS_WITH(GET_MODULE(LiveInputs).setDeclarationJson(
        R"({"schema":"pelican.live_inputs","version":1,"inputs":[]})"),
        ContainsSubstring("live_input.recording_active@1"));
    auto changed = initial;
    ++changed.time_set_revision;
    runtime.prepareFrame(changed);
    CHECK_FALSE(runtime.isRecording());
    const auto problems = GET_MODULE(ProblemList).problemsOf("live_input_recording");
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].at("code").get<std::string>() ==
          "live_input.record_failed@1");

    auto recording = std::make_shared<LiveInputRecording>(lookupRecording());
    runtime.startReplay(recording, initial);
    runtime.prepareFrame(changed);
    CHECK_FALSE(runtime.isReplaying());
    const auto replay_problem = GET_MODULE(ProblemList).problemsOf("live_input_recording");
    REQUIRE(replay_problem.size() == 1);
    CHECK(replay_problem[0].at("code").get<std::string>() ==
          "live_input.replay_time_changed@1");
}

TEST_CASE("OSC-4a reader rejects malformed recordings with their line and cause [osc4a]",
          "[osc4a][live_recording]") {
    const auto base = completeRecording();
    requireInvalid("", "missing header");
    requireInvalid(header() + "\n", "live_input.recording_incomplete@1",
                   "live_input.recording_incomplete@1");
    requireInvalid(base + "{}\n", "line 4: content follows the end line");

    auto json = Json::parse(base.substr(0, base.find('\n')));
    json["version"] = 2;
    requireInvalid(json.dump() + base.substr(base.find('\n')), "line 1: version must be 1");
    json["version"] = 1;
    json["schema"] = "other";
    requireInvalid(json.dump() + base.substr(base.find('\n')), "line 1: schema must be pelican.live_seq");
    requireInvalid(std::string{"{\"schema\":\"pelican.live_seq\",\"schema\":\"pelican.live_seq\"}\n"},
                   "line 1: invalid JSON or duplicate object key");

    const auto first = base.substr(base.find('\n') + 1);
    requireInvalid(header(Json::array({
        Json{{"name", "phase"}, {"type", "float"}, {"default", 0.0}},
        Json{{"name", "phase"}, {"type", "float"}, {"default", 0.0}},
    })) + "\n" + first, "line 1: duplicate input name 'phase'");

    auto frame = Json{{"frame", 1}, {"time", 1.0 / 60.0},
                      {"values", Json{{"phase", 0.25}}}};
    const auto end = Json{{"end", true}, {"frame", 1}, {"time", 1.0 / 60.0},
                          {"evaluations", 1}}.dump();
    frame["unexpected"] = true;
    requireInvalid(header() + "\n" + frame.dump() + "\n" + end + "\n",
                   "line 2: unknown key 'unexpected'");
    frame.erase("unexpected");
    frame["frame"] = 1.5;
    requireInvalid(header() + "\n" + frame.dump() + "\n" + end + "\n",
                   "line 2: field 'frame' must be an unsigned integer");
    frame["frame"] = 1;
    frame["values"]["unknown"] = 1.0;
    requireInvalid(header() + "\n" + frame.dump() + "\n" + end + "\n",
                   "line 2: unknown input name 'unknown'");
    frame["values"] = Json{{"phase", "fast"}};
    requireInvalid(header() + "\n" + frame.dump() + "\n" + end + "\n",
                   "line 2: value for 'phase' must be numeric");
    frame["values"] = Json::object();
    requireInvalid(header() + "\n" + frame.dump() + "\n" + end + "\n",
                   "line 2: values must be a non-empty object");
}

TEST_CASE("OSC-4a reader enforces clock ordering and scalar value domains",
          "[osc4a][live_recording]") {
    auto clock_header = Json::parse(header());
    clock_header["start_time"] = 1.0;
    const auto clock_document = clock_header.dump();
    const std::string first = Json{{"frame", 1}, {"time", 1.0},
        {"values", Json{{"phase", 0.0}}}}.dump();
    const auto document = [&](const Json &second, const Json &end) {
        return clock_document + "\n" + first + "\n" + second.dump() + "\n" + end.dump() + "\n";
    };
    const Json good_end{{"end", true}, {"frame", 2}, {"time", 2.0}, {"evaluations", 2}};
    requireInvalid(document(Json{{"frame", 1}, {"time", 1.5},
        {"values", Json{{"phase", 1.0}}}}, good_end), "line 3: frame must be strictly increasing");
    requireInvalid(document(Json{{"frame", 2}, {"time", 0.5},
        {"values", Json{{"phase", 1.0}}}}, good_end), "line 3: time must be non-decreasing");
    requireInvalid(document(Json{{"frame", 2}, {"time", std::numeric_limits<double>::infinity()},
        {"values", Json{{"phase", 1.0}}}}, good_end), "line 3:");
    requireInvalid(clock_document + "\n" + first + "\n" +
        Json{{"end", false}, {"frame", 1}, {"time", 1.0}, {"evaluations", 1}}.dump() + "\n",
        "line 3: end must be true");
    requireInvalid(clock_document + "\n" + first + "\n" +
        Json{{"end", true}, {"frame", 1}, {"time", 1.0}, {"evaluations", 0}}.dump() + "\n",
        "evaluations", "live_input.recording_invalid@1");
    requireInvalid(clock_document + "\n" +
        Json{{"end", true}, {"frame", 0}, {"time", 0.0}, {"evaluations", 0}}.dump() + "\n",
        "line 2: missing first complete snapshot");

    const auto typed = [&](std::string_view type, Json value) {
        return header(Json::array({Json{{"name", "phase"}, {"type", type},
                                      {"default", 0}}})) + "\n" +
            Json{{"frame", 1}, {"time", 1.0 / 60.0},
                 {"values", Json{{"phase", std::move(value)}}}}.dump() + "\n" +
            Json{{"end", true}, {"frame", 1}, {"time", 1.0 / 60.0},
                 {"evaluations", 1}}.dump() + "\n";
    };
    requireInvalid(typed("int", 1.5), "line 2: value for 'phase' must be an int32 integer");
    requireInvalid(typed("int", 2147483648LL), "line 2: value for 'phase' must be an int32 integer");
    requireInvalid(typed("bool", 2), "line 2: value for 'phase' must be 0 or 1");

    requireInvalid(header() + "\n" + std::string(1024 * 1024 + 1, 'x') + "\n",
                   "line 2: line exceeds 1 MiB");
    requireInvalid(header(Json::array({Json{{"name", std::string(257, 'x')},
        {"type", "float"}, {"default", 0.0}}})) + "\n", "line 1: input name length must be 1..256 bytes");
}

TEST_CASE("OSC-4a loadFile checks the 256 MiB limit from file size",
          "[osc4a][live_recording]") {
    const auto dir = tempDir();
    std::filesystem::create_directories(dir.root);
    const auto path = dir.root / "oversized.live_seq";
    {
        std::ofstream output{path, std::ios::binary};
        output << "x";
    }
    std::filesystem::resize_file(path, 256ull * 1024ull * 1024ull + 1ull);
    try {
        (void)LiveInputRecording::loadFile(path);
        FAIL("oversized recording was accepted");
    } catch (const std::exception &error) {
        const std::string message{error.what()};
        CHECK_THAT(message, ContainsSubstring("live_input.recording_invalid@1: line 1"));
        CHECK_THAT(message, ContainsSubstring("file exceeds 256 MiB"));
    }
}

TEST_CASE("OSC-4a empty declaration recordings preserve their header and end count",
          "[osc4a][live_recording]") {
    LiveInputRecording recording;
    recording.end = {0, 0.0, 0};
    const auto decoded = LiveInputRecording::fromJsonLines(recording.toJsonLines());
    CHECK(decoded.inputs.empty());
    CHECK(decoded.frames.empty());
    CHECK(decoded.end.evaluations == 0);
}

TEST_CASE("OSC-4a runtime chooses matching evaluation lookup or held time lookup",
          "[osc4a][live_recording]") {
    FastModuleContainer modules;
    auto recording = std::make_shared<LiveInputRecording>();
    recording->inputs = {{"phase", LiveInputType::floating, 0.0, {}, {}}};
    recording->clock = {LiveRecordingClockMode::fixed, 0.5, 40, 20.0, 0};
    recording->frames = {{40, 20.0, {{"phase", 1.0}}},
                         {41, 20.0, {{"phase", 2.0}}},
                         {42, 20.5, {{"phase", 4.0}}},
                         {43, 21.0, {{"phase", 8.0}}}};
    recording->end = {43, 21.0, 4};
    auto &runtime = GET_MODULE(LiveInputRecordingRuntime);
    runtime.startReplay(recording,
        {LiveRecordingClockMode::fixed, 0.5, 100, 20.0, 0});
    CHECK(runtime.replayValues(100, 20.0).at("phase") == 1.0);
    CHECK(runtime.replayValues(101, 20.0).at("phase") == 2.0);
    CHECK(runtime.replayValues(102, 20.5).at("phase") == 4.0);
    CHECK(runtime.replayValues(103, 21.0).at("phase") == 8.0);
    CHECK(runtime.replayValues(110, 30.0).at("phase") == 8.0);
    runtime.stopReplay();

    runtime.startReplay(recording,
        {LiveRecordingClockMode::fixed, 1.0, 100, 20.0, 0});
    CHECK(runtime.replayValues(101, 20.0).at("phase") == 2.0);
    CHECK(runtime.replayValues(102, 20.25).at("phase") == 2.0);
    CHECK(runtime.replayValues(103, 20.5).at("phase") == 4.0);
    CHECK(runtime.replayValues(104, 21.5).at("phase") == 8.0);
    runtime.stopReplay();

    recording->clock.start_time = 20.25;
    runtime.startReplay(recording,
        {LiveRecordingClockMode::fixed, 0.5, 100, 20.0, 0});
    CHECK(runtime.replayValues(101, 20.0).at("phase") == 2.0);
    runtime.stopReplay();
}

TEST_CASE("OSC-4a recorder writes first complete and subsequent changed snapshots only",
          "[osc4a][live_recording]") {
    FastModuleContainer modules;
    const auto dir = tempDir();
    std::filesystem::create_directories(dir.root);
    const auto path = dir.root / "runtime.live_seq";
    auto &runtime = GET_MODULE(LiveInputRecordingRuntime);
    const std::vector<LiveInputSpec> specs{{"phase", LiveInputType::floating, 0.0, {}, {}}};
    runtime.startRecording(path, specs, {LiveRecordingClockMode::fixed, 1.0 / 60.0, 20, 2.0, 0});
    LiveInputSnapshot snapshot{specs[0], 1.0, LiveInputSource::rpc, 20, false};
    runtime.recordFrame(20, 2.0, {snapshot});
    runtime.recordFrame(21, 2.0 + 1.0 / 60.0, {snapshot});
    snapshot.value = 2.0;
    runtime.recordFrame(22, 2.0 + 2.0 / 60.0, {snapshot});
    const auto status = runtime.recordStatus();
    REQUIRE(status.has_value());
    CHECK(status->lines == 3);
    CHECK(status->evaluations == 3);
    runtime.stopRecording();
    const auto recording = LiveInputRecording::loadFile(path);
    REQUIRE(recording.frames.size() == 2);
    CHECK(recording.frames[0].values.at("phase") == 1.0);
    CHECK(recording.frames[1].values.at("phase") == 2.0);
    CHECK(recording.end.evaluations == 3);
}

} // namespace Pelican

namespace Pelican {
namespace {
Json liveReadGraph(std::initializer_list<std::string> inputs) {
    Json nodes = Json::array();
    Json outputs = Json::array();
    std::size_t index = 0;
    for (const auto &input : inputs) {
        const auto id = "input" + std::to_string(index++);
        nodes.push_back({{"id", id}, {"op", "input"}, {"name", input}});
        outputs.push_back(id);
    }
    return Json{{"schema", "pelican.value_graph"}, {"version", 1},
                {"nodes", std::move(nodes)}, {"outputs", {{"out", std::move(outputs)}}}};
}

std::string validationError(const ValueGraph::Graph &graph) {
    try {
        validateGraphInputs(graph);
    } catch (const std::exception &error) {
        return error.what();
    }
    return {};
}

std::shared_ptr<LiveInputRecording> declarationRecording(
    std::vector<LiveInputSpec> inputs) {
    auto recording = std::make_shared<LiveInputRecording>();
    recording->inputs = std::move(inputs);
    recording->clock = {LiveRecordingClockMode::fixed, 0.5, 1, 0.5, 0};
    recording->frames = {{1, 0.5, {}}, {2, 0.5, {}}};
    for (const auto &input : recording->inputs) {
        recording->frames[0].values[input.name] = input.default_value;
        recording->frames[1].values[input.name] = input.default_value;
    }
    recording->end = {2, 0.5, 2};
    return recording;
}

struct OfflineLaunch {
    bool previous = GET_MODULE(EngineLaunchConfig).start_time_explicit;
    OfflineLaunch() { GET_MODULE(EngineLaunchConfig).start_time_explicit = true; }
    ~OfflineLaunch() { GET_MODULE(EngineLaunchConfig).start_time_explicit = previous; }
};
} // namespace

TEST_CASE("OSC-4a offline validation covers every current graph read",
          "[osc4a][live_recording]") {
    FastModuleContainer modules;
    (void)GET_MODULE(ProblemList);
    auto &live = GET_MODULE(LiveInputs);
    live.setDeclarationJson(R"({"schema":"pelican.live_inputs","version":1,"inputs":[
        {"name":"a","type":"float","default":0.0,"min":-1.0,"max":1.0},
        {"name":"b","type":"int","default":1}]})");
    const ValueGraph::Graph both{liveReadGraph({"live:a", "live:b"}).dump()};
    const ValueGraph::Graph only_b{liveReadGraph({"live:b"}).dump()};
    OfflineLaunch offline;
    CHECK_THAT(validationError(both), ContainsSubstring("offline.live_input_unrecorded@1"));

    auto only_a = declarationRecording({
        {"a", LiveInputType::floating, 0.0, -1.0, 1.0}});
    auto &runtime = GET_MODULE(LiveInputRecordingRuntime);
    runtime.startReplay(only_a, {LiveRecordingClockMode::fixed, 0.5, 1, 0.5, 0});
    auto error = validationError(both);
    CHECK_THAT(error, ContainsSubstring("offline.live_input_unrecorded@1"));
    CHECK_THAT(error, ContainsSubstring("live:b"));
    error = validationError(only_b);
    CHECK_THAT(error, ContainsSubstring("live:b"));
    runtime.stopReplay();

    auto wrong_type = declarationRecording({
        {"a", LiveInputType::floating, 0.0, -1.0, 1.0},
        {"b", LiveInputType::floating, 1.0, {}, {}}});
    runtime.startReplay(wrong_type, {LiveRecordingClockMode::fixed, 0.5, 1, 0.5, 0});
    error = validationError(both);
    CHECK_THAT(error, ContainsSubstring("live_input.recording_mismatch@1"));
    CHECK_THAT(error, ContainsSubstring("live:b"));
    runtime.stopReplay();

    auto complete = declarationRecording({
        {"a", LiveInputType::floating, 9.0, -100.0, 100.0},
        {"b", LiveInputType::integer, 1.0, {}, {}},
        {"unread", LiveInputType::boolean, 0.0, {}, {}}});
    complete->frames[0].values["a"] = 2.5;
    complete->frames[1].values["a"] = 3.5;
    runtime.startReplay(complete, {LiveRecordingClockMode::fixed, 0.5, 1, 0.5, 0});
    CHECK(validationError(both).empty());
    const auto problems = GET_MODULE(ProblemList).problemsOf("live_input_stale");
    std::size_t stale_a = 0;
    for (const auto &problem : problems) {
        CHECK(problem.at("code").get<std::string>() ==
              "live_input.recording_stale@1");
        if (problem.at("fields").value("input", std::string{}) == "a") ++stale_a;
    }
    CHECK(stale_a == 1);
    auto values = runtime.replayValues(1, 0.5);
    live.freeze(false, 1, &values);
    const auto first = both.evaluate({{"live:a", live.snapshot().at(0).value},
                                      {"live:b", live.snapshot().at(1).value},
                                      {"time", 0.5}, {"delta_time", 0.0}});
    CHECK(first.at("out").at(0) == Catch::Approx(2.5));
    CHECK(live.snapshot().at(0).clamped == false);
    values = runtime.replayValues(2, 0.5);
    live.freeze(false, 2, &values);
    const auto paused = both.evaluate({{"live:a", live.snapshot().at(0).value},
                                       {"live:b", live.snapshot().at(1).value},
                                       {"time", 0.5}, {"delta_time", 0.0}});
    CHECK(paused.at("out").at(0) == Catch::Approx(3.5));
    CHECK(live.snapshot().at(0).source == LiveInputSource::replay);
    runtime.stopReplay();
}

TEST_CASE("OSC-4a input sequence replay alone does not satisfy offline live reads",
          "[osc4a][live_recording]") {
    FastModuleContainer modules;
    auto &launch = GET_MODULE(EngineLaunchConfig);
    auto &live = GET_MODULE(LiveInputs);
    live.setDeclarationJson(R"({"schema":"pelican.live_inputs","version":1,"inputs":[
        {"name":"a","type":"float","default":0.0}]})");
    const ValueGraph::Graph graph{liveReadGraph({"live:a"}).dump()};
    launch.input_replay = true;
    CHECK_THAT(validationError(graph), ContainsSubstring("offline.live_input_unrecorded@1"));
    auto recording = declarationRecording({{"a", LiveInputType::floating, 0.0, {}, {}}});
    auto &runtime = GET_MODULE(LiveInputRecordingRuntime);
    runtime.startReplay(recording, {LiveRecordingClockMode::fixed, 0.5, 1, 0.5, 0});
    CHECK(validationError(graph).empty());
    runtime.stopReplay();
    launch.input_replay = false;
}

TEST_CASE("OSC-1b output policy is independent of headless and RPC [osc1b]",
          "[osc1b][live_recording]") {
    FastModuleContainer modules;
    auto &launch = GET_MODULE(EngineLaunchConfig);
    const auto previous = launch.output_policy;
    launch.output_policy = EngineLaunchOutputPolicy::deterministic_snapshot;
    for (const bool headless : {false, true}) {
        for (const bool rpc : {false, true}) {
            launch.headless = headless;
            launch.rpc = rpc;
            CHECK(offlineLaunchReadsNoLiveInputs());
        }
    }
    launch.output_policy = EngineLaunchOutputPolicy::realtime_saving;
    CHECK_FALSE(offlineLaunchReadsNoLiveInputs());
    launch.output_policy = EngineLaunchOutputPolicy::interactive;
    CHECK_FALSE(offlineLaunchReadsNoLiveInputs());
    launch.output_policy = previous;
}

TEST_CASE("OSC-1b failed graph validation restores earlier live reads [osc1b]",
          "[osc1b][live_recording]") {
    FastModuleContainer modules;
    (void)GET_MODULE(ProblemList);
    auto &launch = GET_MODULE(EngineLaunchConfig);
    const auto previous_policy = launch.output_policy;
    launch.output_policy = EngineLaunchOutputPolicy::deterministic_snapshot;
    auto &live = GET_MODULE(LiveInputs);
    live.setDeclarationJson(R"({"schema":"pelican.live_inputs","version":1,"inputs":[
        {"name":"a","type":"float","default":0.0},
        {"name":"b","type":"float","default":0.0}]})");
    const ValueGraph::Graph a{liveReadGraph({"live:a"}).dump()};
    const ValueGraph::Graph b{liveReadGraph({"live:b"}).dump()};
    const ValueGraph::Graph both{liveReadGraph({"live:a", "live:b"}).dump()};
    auto &runtime = GET_MODULE(LiveInputRecordingRuntime);
    auto recording = declarationRecording({{"a", LiveInputType::floating, 0.0, {}, {}}});
    runtime.startReplay(recording, {LiveRecordingClockMode::fixed, 0.5, 1, 0.5, 0});
    REQUIRE(validationError(a).empty());
    CHECK(live.graphLiveReads() == std::vector<std::string>{"live:a"});
    const auto error = validationError(b);
    CHECK_THAT(error, ContainsSubstring("live:b"));
    CHECK(live.graphLiveReads() == std::vector<std::string>{"live:a"});
    recording = declarationRecording({
        {"a", LiveInputType::floating, 0.0, {}, {}},
        {"b", LiveInputType::floating, 0.0, {}, {}}});
    runtime.stopReplay();
    runtime.startReplay(recording, {LiveRecordingClockMode::fixed, 0.5, 1, 0.5, 0});
    CHECK(validationError(both).empty());
    CHECK(live.graphLiveReads() == std::vector<std::string>{"live:a", "live:b"});
    CHECK(validationError(a).empty());
    CHECK(live.graphLiveReads() == std::vector<std::string>{"live:a", "live:b"});
    runtime.stopReplay();
    launch.output_policy = previous_policy;
}

TEST_CASE("OSC-1b at-default refuses every external live write [osc1b]",
          "[osc1b][live_recording]") {
    FastModuleContainer modules;
    auto &launch = GET_MODULE(EngineLaunchConfig);
    const bool previous = launch.live_inputs_at_default;
    launch.live_inputs_at_default = true;
    auto &live = GET_MODULE(LiveInputs);
    live.setDeclarationJson(R"({"schema":"pelican.live_inputs","version":1,"inputs":[
        {"name":"a","type":"float","default":0.25}]})");
    try {
        (void)live.stageValues(Json{{"a", 3.0}});
        FAIL("set_live_input was accepted at defaults");
    } catch (const std::exception &error) {
        CHECK_THAT(error.what(), ContainsSubstring("live_input.defaults_active@1"));
    }
    const std::array<LiveInputUpdate, 1> update{{{"a", 2.0}}};
    const auto staged = live.stageBatch(update, LiveInputSource::osc);
    CHECK(staged.applied == 0);
    CHECK(staged.replay == 1);
    live.freeze(false, 8);
    REQUIRE(live.snapshot().size() == 1);
    CHECK(live.snapshot().at(0).value == Catch::Approx(0.25));
    CHECK(live.snapshot().at(0).source == LiveInputSource::none);
    try {
        live.beginLiveReplay();
        FAIL("live replay was accepted at defaults");
    } catch (const std::exception &error) {
        CHECK_THAT(error.what(), ContainsSubstring("live_input.defaults_active@1"));
    }
    launch.live_inputs_at_default = previous;
}

TEST_CASE("OSC-1b verify_fold children use deterministic live defaults [osc1b]",
          "[osc1b][live_recording]") {
    FoldVerifyJob job;
    job.frames = 2;
    job.warmup_seconds = 1.5;
    FoldVerifyProcessOptions options;
    options.width = 64;
    options.height = 64;
    const auto arguments = foldVerifyChildArguments(job, options, "/tmp/project", "/tmp/run");
    std::vector<std::string> args;
    for (const auto &argument : arguments) args.push_back(argument.generic_string());
    CHECK(std::find(args.begin(), args.end(), "--live-inputs-at-default") != args.end());
    CHECK(std::find(args.begin(), args.end(), "--render-out") != args.end());
    CHECK(std::find(args.begin(), args.end(), "--start-time") != args.end());
    CHECK(std::find(args.begin(), args.end(), "1.5") != args.end());
}

TEST_CASE("OSC-1b live clock controls reevaluate without becoming clocks [osc1b]",
          "[osc1b][live_recording]") {
    const Json nodes = Json::array({
        Json{{"id", "now"}, {"op", "value.input"}, {"parameters", {{"name", "time"}}}},
        Json{{"id", "phase"}, {"op", "value.input"}, {"parameters", {{"name", "live:phase"}}}},
        Json{{"id", "clock"}, {"op", "value.trigger_clock"},
             {"inputs", {{"time", {{"node", "now"}, {"port", "value"}}},
                         {"paused", {{"node", "phase"}, {"port", "value"}}}}}},
    });
    CHECK(ValueGraph::valueClockSources(nodes, nodes.at(2).at("inputs").at("time")) ==
          std::set<std::string>{"time"});
    CHECK(ValueGraph::valueClockSources(nodes, nodes.at(2).at("inputs").at("paused")).empty());

    for (const std::string control : {std::string{"paused"}, std::string{"reset"}}) {
        CAPTURE(control);
        const Json graph_document{
            {"schema", "pelican.value_graph"}, {"version", 1},
            {"nodes", Json::array({
                Json{{"id", "now"}, {"op", "input"}, {"name", "time"}},
                Json{{"id", "phase"}, {"op", "input"}, {"name", "live:phase"}},
                Json{{"id", "gate"}, {"op", "constant"}, {"value", 1.0}},
                Json{{"id", "clock"}, {"op", "trigger_clock"},
                     {"time", "now"}, {"trigger", "gate"}, {control, "phase"}}})},
            {"outputs", {{"elapsed", Json::array({"clock"})}}}, {"bindings", Json::array()}};
        const ValueGraph::Graph graph{graph_document.dump()};
        auto state = graph.createState();
        const auto evaluate = [&](double time, double live) {
            return graph.evaluate({{"time", time}, {"live:phase", live}}, {}, {}, &state)
                .at("elapsed").at(0);
        };
        if (control == "paused") {
            CHECK(evaluate(1.0, 0.0) == Catch::Approx(0.0));
            CHECK(evaluate(2.0, 0.0) == Catch::Approx(1.0));
            CHECK(evaluate(2.0, 1.0) == Catch::Approx(1.0));
            CHECK(evaluate(3.0, 1.0) == Catch::Approx(1.0));
            CHECK(evaluate(3.0, 0.0) == Catch::Approx(1.0));
        } else {
            CHECK(evaluate(1.0, 0.0) == Catch::Approx(0.0));
            CHECK(evaluate(2.0, 0.0) == Catch::Approx(1.0));
            CHECK(evaluate(2.0, 1.0) == Catch::Approx(0.0));
            CHECK(evaluate(2.0, 0.0) == Catch::Approx(0.0));
        }
    }
}
} // namespace Pelican

namespace Pelican {
namespace {

LiveInputRecording bakeRecording() {
    LiveInputRecording recording;
    recording.clock = {LiveRecordingClockMode::fixed, 0.25, 1, 10.0, 0};
    recording.inputs = {{"phase", LiveInputType::floating, 0.25, {}, {}},
                        {"count", LiveInputType::integer, 2.0, {}, {}},
                        {"enabled", LiveInputType::boolean, 0.0, {}, {}}};
    recording.frames = {{1, 10.0, {{"phase", 0.25}, {"count", 2.0}, {"enabled", 0.0}}},
                        {2, 10.25, {{"phase", 1.0}}},
                        {3, 10.50, {{"phase", 3.0}}},
                        {4, 10.50, {{"phase", 5.0}, {"count", 7.0}, {"enabled", 1.0}}},
                        {5, 11.25, {{"phase", 9.0}}}};
    recording.end = {6, 11.30, 6};
    return recording;
}

float bakedValue(const LiveRecordingTable &table, std::size_t feature, std::uint32_t sample) {
    const auto row = feature * table.layout.rows_per_feature + sample / table.layout.width;
    return table.values.at(row * table.layout.width + sample % table.layout.width);
}

std::string bakeError(const std::function<void()> &action) {
    try {
        action();
    } catch (const GraphRegistryError &error) {
        return error.code();
    }
    return "none";
}

void writeBakeText(const std::filesystem::path &path, const std::string &text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(output.good());
}

std::string readBakeText(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

Json bakeGraph(const std::string &asset, const Json &interpolation, bool time_edge) {
    Json nodes = Json::array();
    Json parameters{{"asset", asset}, {"row", "phase"}};
    if (!interpolation.is_discarded()) parameters["interpolation"] = interpolation;
    Json reader{{"id", "reader"}, {"op", "value.sample_table"},
                {"parameters", parameters}, {"outputs", Json::array({"value"})}};
    if (time_edge) {
        nodes.push_back({{"id", "clock"}, {"op", "value.input"},
                         {"parameters", {{"name", "time"}}}, {"outputs", Json::array({"value"})}});
        reader["inputs"] = {{"time", {{"node", "clock"}, {"port", "value"}}}};
    }
    nodes.push_back(reader);
    return {{"schema", "pelican.graph"}, {"version", 1}, {"nodes", nodes},
            {"outputs", {{"level", {{"node", "reader"}, {"port", "value"}}}}}};
}

double evaluateBakeGraph(const Json &graph, double time, double scene_time) {
    const ValueGraph::Graph built{graph.dump()};
    return built.evaluate({{"time", time}, {"scene_time", scene_time}}).at("level").at(0);
}

} // namespace

TEST_CASE("OSC-4b pure generation uses recording time and declared input order", "[osc4b]") {
    const auto recording = bakeRecording();
    const auto table = makeLiveRecordingTable(recording, 4.0);
    CHECK(table.layout.sample_count == 7);
    CHECK(table.layout.hop_seconds == Catch::Approx(0.25));
    CHECK(table.layout.features == std::vector<std::string>{"phase", "count", "enabled"});
    CHECK(bakedValue(table, 0, 0) == 0.25f);
    CHECK(bakedValue(table, 0, 1) == 1.0f);
    CHECK(bakedValue(table, 0, 2) == 5.0f);
    CHECK(bakedValue(table, 0, 3) == 5.0f);
    CHECK(bakedValue(table, 0, 4) == 5.0f);
    CHECK(bakedValue(table, 0, 5) == 9.0f);
    CHECK(bakedValue(table, 0, 6) == 9.0f);
    CHECK(bakedValue(table, 1, 2) == 7.0f);
    CHECK(bakedValue(table, 2, 2) == 1.0f);

    const auto subset = makeLiveRecordingTable(recording, 4.0,
        std::vector<std::string>{"enabled", "phase"});
    CHECK(subset.layout.features == std::vector<std::string>{"phase", "enabled"});
    CHECK(bakedValue(subset, 0, 2) == 5.0f);
    CHECK(bakedValue(subset, 1, 2) == 1.0f);
}

TEST_CASE("OSC-4b rates, numeric conversion and table dimensions are refused by name", "[osc4b]") {
    for (const double rate : {0.0, -1.0, 1001.0,
                              std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::quiet_NaN()})
        CHECK(bakeError([&] { (void)makeLiveRecordingTable(bakeRecording(), rate); }) ==
              "live_input.bake_invalid@1");
    CHECK(bakeError([&] { (void)makeLiveRecordingTable(bakeRecording(), 4.0,
                            std::vector<std::string>{}); }) == "live_input.bake_invalid@1");
    CHECK(bakeError([&] { (void)makeLiveRecordingTable(bakeRecording(), 4.0,
                            std::vector<std::string>{"phase", "phase"}); }) == "live_input.bake_invalid@1");
    CHECK(bakeError([&] { (void)makeLiveRecordingTable(bakeRecording(), 4.0,
                            std::vector<std::string>{"missing"}); }) == "live_input.bake_invalid@1");
    auto recording = bakeRecording();
    recording.end.time = 9.0;
    CHECK(bakeError([&] { (void)makeLiveRecordingTable(recording, 4.0); }) ==
          "live_input.bake_invalid@1");
    recording = bakeRecording();
    recording.inputs[1].default_value = 16777217.0;
    recording.frames[0].values["count"] = 16777217.0;
    CHECK(bakeError([&] { (void)makeLiveRecordingTable(recording, 4.0); }) ==
          "live_input.bake_invalid@1");
    recording = bakeRecording();
    recording.frames[0].values["phase"] = std::numeric_limits<double>::infinity();
    CHECK(bakeError([&] { (void)makeLiveRecordingTable(recording, 4.0); }) ==
          "live_input.bake_invalid@1");
    recording = bakeRecording();
    recording.frames[0].values["phase"] = static_cast<double>(std::numeric_limits<float>::max()) * 2.0;
    CHECK(bakeError([&] { (void)makeLiveRecordingTable(recording, 4.0); }) ==
          "live_input.bake_invalid@1");
    recording = bakeRecording();
    recording.end.time = 1.0e12;
    CHECK(bakeError([&] { (void)makeLiveRecordingTable(recording, 60.0); }) ==
          "live_input.bake_invalid@1");
}

TEST_CASE("OSC-4b ten minutes at 60 Hz has a bounded nine-row table", "[osc4b]") {
    auto recording = bakeRecording();
    recording.inputs.resize(1);
    for (auto &frame : recording.frames) frame.values = {{"phase", frame.time}};
    recording.end.time = recording.clock.start_time + 600.0;
    const auto table = makeLiveRecordingTable(recording, 60.0);
    CHECK(table.layout.sample_count == 36001);
    CHECK(table.layout.rows_per_feature == 9);
    CHECK(table.values.size() == 36864);
    CHECK(table.values.size() * sizeof(float) == 147456);
}

TEST_CASE("OSC-4b runtime subtracts t0 for hold and linear scene clocks", "[osc4b]") {
    const auto dir = tempDir();
    std::filesystem::create_directories(dir.root);
    const auto source = dir.root / "performance.live_seq";
    const auto table = dir.root / "phase.ktx2";
    writeBakeText(source, bakeRecording().toJsonLines());
    REQUIRE(bakeError([&] { (void)bakeLiveRecording(source, table, 4.0); }) == "none");
    const auto loaded = loadSampleTable(table);
    CHECK(loaded->t0_seconds == Catch::Approx(10.0));
    CHECK(loaded->sample(0, 10.0, SampleTableInterpolation::hold) == 0.25);
    CHECK(loaded->sample(0, 10.375, SampleTableInterpolation::hold) == 1.0);
    CHECK(loaded->sample(0, 10.375, SampleTableInterpolation::linear) == 3.0);
    CHECK(loaded->sample(0, 10.625, SampleTableInterpolation::hold) == 5.0);
    CHECK(loaded->sample(0, 11.125, SampleTableInterpolation::linear) == Catch::Approx(7.0));
    CHECK(loaded->sample(0, -100.0, SampleTableInterpolation::hold) == 0.25);
    CHECK(loaded->sample(0, 100.0, SampleTableInterpolation::hold) == 9.0);
    CHECK(loaded->sample(0, std::numeric_limits<double>::quiet_NaN(),
                         SampleTableInterpolation::hold) == 0.25);

    const auto asset = table.generic_string();
    CHECK(evaluateBakeGraph(bakeGraph(asset, "hold", true), 10.375, 0.0) == 1.0);
    CHECK(evaluateBakeGraph(bakeGraph(asset, "linear", true), 10.375, 0.0) == 3.0);
    CHECK(evaluateBakeGraph(bakeGraph(asset, Json::value_t::discarded, true), 10.375, 0.0) == 3.0);
    CHECK(evaluateBakeGraph(bakeGraph(asset, "hold", false), 0.0, 10.375) == 1.0);
    CHECK(evaluateBakeGraph(bakeGraph(asset, Json::value_t::discarded, false), 0.0, 10.375) == 3.0);
    CHECK(bakeError([&] { (void)ValueGraph::Graph{bakeGraph(asset, "cubic", true).dump()}; }) ==
          "graph.render_node_parameter_invalid@1");

    SampleTableLayout boundaries;
    boundaries.sample_count = 36001;
    boundaries.rows_per_feature = 9;
    boundaries.hop_seconds = 1.0 / 60.0;
    boundaries.features = {"phase"};
    std::vector<float> values(9 * sampleTableWidth);
    for (std::uint32_t frame = 0; frame < boundaries.sample_count; ++frame)
        values[(frame / sampleTableWidth) * sampleTableWidth + frame % sampleTableWidth] =
            static_cast<float>(frame);
    for (std::uint32_t frame = 0; frame < boundaries.sample_count; ++frame) {
        const double relative = static_cast<double>(frame) / 60.0;
        CHECK(sampleTableValue(boundaries, values, 0, relative,
                               SampleTableInterpolation::hold) == frame);
    }
}

TEST_CASE("OSC-4b generated manifests load without their recording and track real changes", "[osc4b]") {
    const auto dir = tempDir();
    std::filesystem::create_directories(dir.root);
    const auto source = dir.root / "performance.live_seq";
    const auto table = dir.root / "phase.ktx2";
    const auto manifest = derivedAssetManifestPath(table);
    writeBakeText(source, bakeRecording().toJsonLines());
    const auto baked = bakeLiveRecording(source, table, 4.0);
    CHECK(baked.rows == std::vector<std::uint32_t>{0, 1, 2});
    const auto manifest_json = Json::parse(readBakeText(manifest));
    CHECK(manifest_json.at("tool").at("name") == "pelican.bake_live_recording");
    CHECK(manifest_json.at("source").at("metadata").at("t0_seconds") == 10.0);
    CHECK(manifest_json.at("source").at("metadata").contains("settings") == false);
    CHECK(manifest_json.at("outputs").at(0).at("schema") == "pelican.live_recording_table");
    auto status = readDerivedAssetStatus(manifest);
    CHECK_FALSE(status.stale);
    CHECK(loadSampleTable(table)->layout.features ==
          std::vector<std::string>{"phase", "count", "enabled"});

    writeBakeText(source, bakeRecording().toJsonLines() + "\n");
    status = readDerivedAssetStatus(manifest);
    CHECK(status.stale);
    CHECK(std::find(status.stale_reasons.begin(), status.stale_reasons.end(), "source_changed") !=
          status.stale_reasons.end());
    std::filesystem::remove(source);
    status = readDerivedAssetStatus(manifest);
    CHECK_FALSE(status.stale);
    CHECK(std::find(status.stale_reasons.begin(), status.stale_reasons.end(), "source_missing") ==
          status.stale_reasons.end());
    CHECK(loadSampleTable(table)->layout.features.size() == 3);
    writeBakeText(table, readBakeText(table) + "changed");
    status = readDerivedAssetStatus(manifest);
    CHECK(std::find(status.stale_reasons.begin(), status.stale_reasons.end(), "output_changed") !=
          status.stale_reasons.end());
    std::filesystem::remove(table);
    status = readDerivedAssetStatus(manifest);
    CHECK(std::find(status.stale_reasons.begin(), status.stale_reasons.end(), "output_missing") !=
          status.stale_reasons.end());
    const auto audio_manifest = derivedAssetManifestPath(source);
    const auto missing_audio_sha = std::string(64, '0');
    SampleTableLayout audio_layout;
    audio_layout.sample_count = 1;
    audio_layout.features = {"rms"};
    writeBakeText(audio_manifest, audioAnalysisManifest(source, missing_audio_sha, AudioAnalysisSettings{},
        audio_layout, table, missing_audio_sha).dump(2));
    const auto audio_status = readDerivedAssetStatus(audio_manifest);
    CHECK(audio_status.stale);
    CHECK(std::find(audio_status.stale_reasons.begin(), audio_status.stale_reasons.end(), "source_missing") !=
          audio_status.stale_reasons.end());
}

TEST_CASE("OSC-4b two-file commit rolls back after either replacement", "[osc4b]") {
    const auto dir = tempDir();
    std::filesystem::create_directories(dir.root);
    const auto source = dir.root / "performance.live_seq";
    const auto table = dir.root / "phase.ktx2";
    const auto manifest = derivedAssetManifestPath(table);
    auto staged_table = table;
    staged_table += ".live-bake.tmp";
    auto staged_manifest = manifest;
    staged_manifest += ".live-bake.tmp";
    writeBakeText(source, bakeRecording().toJsonLines());

    for (const auto fault : {LiveRecordingCommitFault::after_table,
                             LiveRecordingCommitFault::after_manifest}) {
        CHECK(bakeError([&] { (void)bakeLiveRecording(source, table, 4.0, std::nullopt, fault); }) ==
              "live_input.bake_failed@1");
        CHECK_FALSE(std::filesystem::exists(table));
        CHECK_FALSE(std::filesystem::exists(manifest));
        CHECK_FALSE(std::filesystem::exists(staged_table));
        CHECK_FALSE(std::filesystem::exists(staged_manifest));
    }
    (void)bakeLiveRecording(source, table, 4.0);
    const auto old_table = readBakeText(table);
    const auto old_manifest = readBakeText(manifest);
    for (const auto fault : {LiveRecordingCommitFault::after_table,
                             LiveRecordingCommitFault::after_manifest}) {
        CHECK(bakeError([&] { (void)bakeLiveRecording(source, table, 8.0, std::nullopt, fault); }) ==
              "live_input.bake_failed@1");
        CHECK(readBakeText(table) == old_table);
        CHECK(readBakeText(manifest) == old_manifest);
        CHECK_FALSE(std::filesystem::exists(staged_table));
        CHECK_FALSE(std::filesystem::exists(staged_manifest));
    }
    const auto held = loadSampleTable(table);
    std::atomic<bool> writing{true};
    std::thread reader{[&] {
        while (writing.load()) {
            const auto loaded = loadSampleTable(table);
            CHECK((loaded->layout.sample_count == 7 || loaded->layout.sample_count == 12));
        }
    }};
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    CHECK(bakeError([&] { (void)bakeLiveRecording(source, table, 8.0); }) == "none");
    writing = false;
    reader.join();
    CHECK(loadSampleTable(table)->layout.sample_count == 12);
}

struct BakeService {
    std::filesystem::path root;
    RenderConfigEditorService service;

    explicit BakeService(std::filesystem::path directory) : root{std::move(directory)}, service{dependencies()} {}
    RenderConfigEditorDependencies dependencies() const {
        return {.source_reference = "passes/main.json",
                .source_path = root / "passes" / "main.json",
                .source_bytes = "{}",
                .project_root = root,
                .gate = [] { return RenderConfigEditorGateObservation{}; },
                .runtime_snapshot = [] { return RenderConfigRuntimeSnapshot{}; },
                .apply_candidate = [](RenderConfigCandidateDocumentSet, RenderConfigSourceCommit,
                                      RenderConfigRuntimeApplyCompletion) {
                    return RenderConfigRuntimeApplyResult{};
                },
                .feature_catalog = [] { return std::vector<RenderFeatureCatalogEntry>{}; }};
    }
    Json await(const nlohmann::ordered_json &accepted) {
        REQUIRE(accepted.at("status") == "accepted");
        for (int attempt = 0; attempt != 2000; ++attempt) {
            service.commitPending();
            auto result = Json(service.getResult({{"ticket", accepted.at("ticket")}}));
            if (result.at("status") != "running") return result;
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        FAIL("live recording bake did not finish");
        return {};
    }
};

TEST_CASE("OSC-4b RPC tickets return hold rows and validate requests", "[osc4b]") {
    const auto dir = tempDir();
    std::filesystem::create_directories(dir.root);
    const auto source = dir.root / "takes" / "performance.live_seq";
    writeBakeText(source, bakeRecording().toJsonLines());
    writeBakeText(dir.root / "passes" / "main.json", "{}");
    BakeService bake{dir.root};
    const auto accepted = bake.service.bakeLiveRecording(
        {{"recording", "takes/performance.live_seq"}, {"out", "tables/phase.ktx2"}});
    CHECK(accepted.at("status") == "accepted");
    const auto result = bake.await(accepted);
    INFO(result.dump());
    REQUIRE(result.at("status") == "committed");
    CHECK(result.at("features") == Json::array({"phase", "count", "enabled"}));
    CHECK(result.at("rows") == Json::array({0, 1, 2}));
    CHECK(result.at("sample_count") == 7);
    CHECK(result.at("hop_seconds") == 0.25);
    CHECK(result.at("t0_seconds") == 10.0);
    CHECK(result.at("interpolation") == "hold");
    CHECK(result.at("approximation").get<std::string>().find("one hop late") != std::string::npos);
    CHECK(std::filesystem::is_regular_file(dir.root / "tables" / "phase.ktx2"));
    CHECK(std::filesystem::is_regular_file(dir.root / "tables" / "phase.ktx2.import.json"));
    CHECK(bake.await(bake.service.bakeLiveRecording(
              {{"recording", "takes/performance.live_seq"}, {"out", "tables/phase.ktx2"},
               {"rate", 8}, {"inputs", {"enabled"}}})).at("status") == "committed");

    const auto failure = bake.await(bake.service.bakeLiveRecording(
        {{"recording", "takes/missing.live_seq"}, {"out", " tables/none.ktx2"}}));
    CHECK(failure.at("status") == "rejected");
    CHECK(failure.at("error").at("code") == "live_input.bake_failed@1");

    auto incomplete = bakeRecording().toJsonLines();
    incomplete.erase(incomplete.rfind("{\"end\":"));
    writeBakeText(dir.root / "takes" / "incomplete.live_seq", incomplete);
    const auto rejected = bake.await(bake.service.bakeLiveRecording(
        {{"recording", "takes/incomplete.live_seq"}, {"out", "tables/incomplete.ktx2"}}));
    CHECK(rejected.at("error").at("code") == "live_input.recording_incomplete@1");
    CHECK_FALSE(std::filesystem::exists(dir.root / "tables" / "incomplete.ktx2"));
    CHECK_FALSE(std::filesystem::exists(dir.root / "tables" / "incomplete.ktx2.import.json"));
    const auto unknown = bake.await(bake.service.bakeLiveRecording(
        {{"recording", "takes/performance.live_seq"}, {"out", "tables/unknown.ktx2"},
         {"inputs", {"unknown"}}}));
    CHECK(unknown.at("error").at("code") == "live_input.bake_invalid@1");
    CHECK(unknown.at("error").at("reason") == "unknown_input");
    const auto duplicate = bake.await(bake.service.bakeLiveRecording(
        {{"recording", "takes/performance.live_seq"}, {"out", "tables/duplicate.ktx2"},
         {"inputs", Json::array({"phase", "phase"})}}));
    CHECK(duplicate.at("error").at("reason") == "duplicate_input");
    const auto empty = bake.await(bake.service.bakeLiveRecording(
        {{"recording", "takes/performance.live_seq"}, {"out", "tables/empty.ktx2"},
         {"inputs", Json::array()}}));
    CHECK(empty.at("error").at("reason") == "empty_inputs");

    for (const Json bad : {Json{{"recording", "takes/performance.live_seq"}, {"out", "tables/a.ktx2"}, {"rate", 0}},
                           Json{{"recording", "takes/performance.live_seq"}, {"out", "tables/a.ktx2"}, {"rate", "fast"}},
                           Json{{"recording", 7}, {"out", "tables/a.ktx2"}},
                           Json{{"recording", "takes/performance.live_seq"}, {"out", "../a.ktx2"}},
                           Json{{"recording", "takes/performance.live_seq"}, {"out", "tables/a.txt"}},
                           Json{{"recording", "takes/performance.live_seq"}, {"out", "tables/a.ktx2"}, {"inputs", {7}}},
                           Json{{"recording", "takes/performance.live_seq"}, {"out", "tables/a.ktx2"}, {"inputs", {}}}}) {
        CHECK_THROWS((void)bake.service.bakeLiveRecording(bad));
        CHECK_FALSE(std::filesystem::exists(dir.root / "tables" / "a.ktx2"));
    }
    CHECK_THROWS((void)bake.service.bakeLiveRecording(
        {{"recording", "tables/a.ktx2.import.json"}, {"out", "tables/a.ktx2"}}));
#ifndef _WIN32
    std::filesystem::create_directory_symlink(dir.root / "..", dir.root / "escape");
    CHECK_THROWS((void)bake.service.bakeLiveRecording(
        {{"recording", "takes/performance.live_seq"}, {"out", "escape/a.ktx2"}}));
#endif
}

} // namespace Pelican
