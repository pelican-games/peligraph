#include "../src/core/os/liveinputrecording.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/renderingpass/graphinputs.hpp"
#include "../src/core/watch/problemlist.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

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
} // namespace Pelican
