#include "../src/core/audio/audio.hpp"
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/log.hpp"
#include "../src/core/userpublic/gamecontext.hpp"
#include "../src/core/userpublic/valuegraph.hpp"
#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/renderingpass/graphinputs.hpp"
#include "../src/project/graphcomposition.hpp"
#include "scene_load_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

namespace {

void ensureLogger() {
    static const bool initialized = [] {
        setupLogger();
        return true;
    }();
    (void)initialized;
}

void writeText(const std::filesystem::path &path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios_base::binary};
    file << text;
}

void writeU16(std::ofstream &file, std::uint16_t value) {
    file.put(static_cast<char>(value & 0xFF));
    file.put(static_cast<char>((value >> 8) & 0xFF));
}

void writeU32(std::ofstream &file, std::uint32_t value) {
    file.put(static_cast<char>(value & 0xFF));
    file.put(static_cast<char>((value >> 8) & 0xFF));
    file.put(static_cast<char>((value >> 16) & 0xFF));
    file.put(static_cast<char>((value >> 24) & 0xFF));
}

void writeTinyPcmWav(const std::filesystem::path &path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios_base::binary};

    constexpr std::uint16_t channels = 1;
    constexpr std::uint32_t sample_rate = 8000;
    constexpr std::uint16_t bits_per_sample = 16;
    constexpr std::uint16_t block_align = channels * bits_per_sample / 8;
    constexpr std::uint32_t data_size = block_align * 4;
    constexpr std::uint32_t riff_size = 36 + data_size;

    file.write("RIFF", 4);
    writeU32(file, riff_size);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    writeU32(file, 16);
    writeU16(file, 1);
    writeU16(file, channels);
    writeU32(file, sample_rate);
    writeU32(file, sample_rate * block_align);
    writeU16(file, block_align);
    writeU16(file, bits_per_sample);
    file.write("data", 4);
    writeU32(file, data_size);

    writeU16(file, 0);
    writeU16(file, 1024);
    writeU16(file, 0);
    writeU16(file, static_cast<std::uint16_t>(-1024));
}

struct Sandbox {
    std::filesystem::path base;
    std::filesystem::path root;

    Sandbox() {
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        base = std::filesystem::temp_directory_path() / ("pelican_audio_" + suffix);
        root = base / "project";
        std::filesystem::create_directories(root);
    }

    ~Sandbox() {
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
    }
};

// A headless project: its voices mix into the device-less engine.
void configureNullAudioProject(const Sandbox &sandbox) {
    GET_MODULE(EngineLaunchConfig).headless = true;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
}

} // namespace

TEST_CASE("GameContext audio play state works with the headless engine", "[audio]") {
    ensureLogger();
    Sandbox sandbox;
    writeTinyPcmWav(sandbox.root / "tone.wav");

    FastModuleContainer modules;
    configureNullAudioProject(sandbox);

    GameContext context;
    const auto first = context.audio().playSound("project://tone.wav");
    const auto second = context.audio().playSound("project://tone.wav");

    REQUIRE(first != second);
    REQUIRE(context.audio().isPlaying(first));
    REQUIRE(context.audio().isPlaying(second));
    REQUIRE(GET_MODULE(Audio).voiceCountForTesting() == 2);
    REQUIRE(GET_MODULE(Audio).backendVoiceCountForTesting() == 2);

    REQUIRE_NOTHROW(context.audio().setBusVolume("master", 0.75f));
    REQUIRE_NOTHROW(context.audio().setBusVolume("se", 0.5f));
    const ValueGraph::Graph authored{R"({"schema":"pelican.value_graph","version":1,
      "nodes":[{"id":"gain","op":"input","name":"gain"},
               {"id":"pitch","op":"input","name":"pitch"}],
      "outputs":{"volume":["gain"],"rate":["pitch"]},
      "bindings":[{"output":"volume","kind":"sound_volume","target":"voice"},
                  {"output":"rate","kind":"sound_pitch","target":"voice"}]})"};
    const ValueGraph::Graph graph{authored.serialize()};
    const auto published = graph.publish(context, {{"gain", 0.25}, {"pitch", 1.5}}, {{"voice", first}});
    REQUIRE(published.applied == 2);
    REQUIRE(published.unavailable.empty());
    const auto state = context.audio().soundState(first);
    REQUIRE(state);
    REQUIRE(state->volume == 0.25f);
    REQUIRE(state->pitch == 1.5f);
    REQUIRE(state->effective_volume == 0.09375f);
    // AU-1: the device-less engine's cursor is the frames it mixed -- none yet.
    REQUIRE(state->position_seconds == 0.0);
    REQUIRE(context.audio().soundState(second)->volume == 1.0f);
    REQUIRE(context.audio().soundState(second)->pitch == 1.0f);
    context.audio().setBusVolume("master", 0.5f);
    REQUIRE(context.audio().soundState(first)->effective_volume == 0.0625f);
    REQUIRE(context.audio().soundState(second)->effective_volume == 0.25f);

    const auto next_handle = GET_MODULE(Audio).nextHandleForTesting();
    REQUIRE(context.audio().setSoundPaused(first, true));
    REQUIRE(context.audio().setSoundPaused(first, true));
    REQUIRE_FALSE(context.audio().isPlaying(first));
    REQUIRE(context.audio().soundState(first)->paused);
    REQUIRE(context.audio().isPlaying(second));
    GET_MODULE(Audio).update();
    REQUIRE(GET_MODULE(Audio).voiceCountForTesting() == 2);
    REQUIRE(GET_MODULE(Audio).backendVoiceCountForTesting() == 2);
    REQUIRE(context.audio().soundState(first)->pitch == 1.5f);
    REQUIRE(context.audio().setSoundPaused(first, false));
    REQUIRE(context.audio().setSoundPaused(first, false));
    REQUIRE(context.audio().isPlaying(first));
    REQUIRE_FALSE(context.audio().soundState(first)->paused);
    REQUIRE(context.audio().soundState(first)->effective_volume == 0.0625f);
    REQUIRE(GET_MODULE(Audio).nextHandleForTesting() == next_handle);
    REQUIRE(context.audio().setSoundPaused(first, true));

    context.audio().stopSound(first);
    REQUIRE_FALSE(context.audio().isPlaying(first));
    REQUIRE_FALSE(context.audio().soundState(first));
    REQUIRE_FALSE(context.audio().setSoundVolume(first, 1.0f));
    REQUIRE_FALSE(context.audio().setSoundPitch(first, 1.0f));
    REQUIRE_FALSE(context.audio().setSoundPaused(first, false));
    const auto stopped = graph.publish(context, {{"gain", 1}, {"pitch", 1}}, {{"voice", first}});
    REQUIRE(stopped.applied == 0);
    REQUIRE(stopped.unavailable.size() == 2);
    REQUIRE(context.audio().isPlaying(second));
    REQUIRE(GET_MODULE(Audio).voiceCountForTesting() == 1);
    REQUIRE(GET_MODULE(Audio).backendVoiceCountForTesting() == 1);

    context.audio().stopSound(second);
    REQUIRE_FALSE(context.audio().isPlaying(second));
    REQUIRE(GET_MODULE(Audio).voiceCountForTesting() == 0);
    REQUIRE(GET_MODULE(Audio).backendVoiceCountForTesting() == 0);
}

TEST_CASE("graph audio events drive the voice state machine", "[audio][graph-composition]") {
    // AU-1 decision 3. The events are the graph's own state, consumed when
    // their time comes; what the voice should be is re-sent every frame, so a
    // refused directive is reported and the next frame says the same again.
    const ValueGraph::Graph graph{R"({"schema":"pelican.value_graph","version":1,
      "nodes":[{"id":"events","op":"event_input"},
        {"id":"time","op":"input","name":"time"},
        {"id":"sound","op":"audio_playback","time":"time","events":"events","asset":"tone.wav"}],
      "outputs":{}})"};
    auto state = graph.createState();
    using Command = Animation::CurvePlaybackCommand;
    using Phase = ValueGraph::AudioVoiceDirective::Phase;
    std::vector<ValueGraph::AudioVoiceDirective> seen;
    bool accept = false;
    const ValueGraph::AudioVoiceWriter writer = [&](std::string_view node, const ValueGraph::AudioVoiceDirective &directive) {
        REQUIRE(std::string{node} == "sound");
        seen.push_back(directive);
        return accept;
    };
    // Idle and nothing to say: no writer needed, nothing unavailable.
    REQUIRE(graph.publish(GameContext{}, {{"time", 0}}, {}, {}, {}, {}, &state).unavailable.empty());
    REQUIRE(graph.emitEvent(state, "events", Command::start, 0));
    REQUIRE(graph.emitEvent(state, "events", Command::pause, 0.5));
    const auto refused = graph.publish(GameContext{}, {{"time", 0.25}}, {}, {}, {}, {}, &state, writer);
    REQUIRE(refused.applied == 1);
    REQUIRE(refused.unavailable == std::vector<std::string>{"audio.playback:sound"});
    REQUIRE(seen.back().phase == Phase::playing);
    REQUIRE(seen.back().epoch == 1);
    REQUIRE(seen.back().elapsed == 0.25);
    REQUIRE(seen.back().native);
    accept = true;
    const auto paused = graph.publish(GameContext{}, {{"time", 1.0}}, {}, {}, {}, {}, &state, writer);
    REQUIRE(paused.applied == 1);
    REQUIRE(paused.unavailable.empty());
    REQUIRE(seen.back().phase == Phase::paused);
    REQUIRE(seen.back().elapsed == 0.5);
    REQUIRE(seen.back().epoch == 1);
    REQUIRE(graph.emitEvent(state, "events", Command::resume, 1.5));
    REQUIRE(graph.emitEvent(state, "events", Command::start, 2.0));
    REQUIRE(graph.publish(GameContext{}, {{"time", 1.75}}, {}, {}, {}, {}, &state, writer).applied == 1);
    REQUIRE(seen.back().phase == Phase::playing);
    REQUIRE(seen.back().elapsed == 0.75);
    REQUIRE(graph.publish(GameContext{}, {{"time", 2.0}}, {}, {}, {}, {}, &state, writer).applied == 1);
    REQUIRE(seen.back().epoch == 2);
    REQUIRE(seen.back().elapsed == 0.0);
    REQUIRE(graph.emitEvent(state, "events", Command::reset, 2.5));
    REQUIRE(graph.publish(GameContext{}, {{"time", 3.0}}, {}, {}, {}, {}, &state, writer).applied == 1);
    REQUIRE(seen.back().phase == Phase::idle);
}

TEST_CASE("Stopped audio voices do not accumulate in either registry", "[audio]") {
    ensureLogger();
    Sandbox sandbox;
    writeTinyPcmWav(sandbox.root / "tone.wav");

    FastModuleContainer modules;
    configureNullAudioProject(sandbox);

    GameContext context;
    for (std::size_t i = 0; i < 256; ++i) {
        const auto handle = context.audio().playSound("project://tone.wav");
        context.audio().stopSound(handle);
    }

    auto &audio = GET_MODULE(Audio);
    REQUIRE(audio.voiceCountForTesting() == 0);
    REQUIRE(audio.backendVoiceCountForTesting() == 0);
}

TEST_CASE("GameContext audio reports invalid inputs", "[audio]") {
    ensureLogger();
    Sandbox sandbox;
    writeText(sandbox.root / "bad.wav", "not a wav");

    FastModuleContainer modules;
    configureNullAudioProject(sandbox);

    GameContext context;
    REQUIRE_THROWS_WITH(context.audio().playSound("project://bad.wav"),
                        Catch::Matchers::ContainsSubstring("Invalid WAV"));
    REQUIRE_THROWS_WITH(context.audio().playSound("project://missing.wav"),
                        Catch::Matchers::ContainsSubstring("Failed to open file"));
    REQUIRE_THROWS_WITH(context.audio().setBusVolume("dialog", 1.0f),
                        Catch::Matchers::ContainsSubstring("Unknown audio bus"));
    REQUIRE_THROWS_WITH(context.audio().setBusVolume("se", -0.1f),
                        Catch::Matchers::ContainsSubstring("finite non-negative"));
    REQUIRE_THROWS(context.audio().setSoundVolume(SoundHandle{}, -1.0f));
    REQUIRE_THROWS(context.audio().setSoundPitch(SoundHandle{}, 0.0f));
}

// ---------------------------------------------------------------------------
// AU-1: the graph drives its voices, measured in the device-less engine. The
// test turns the engine's crank itself (Audio::mixOffline), so every cursor
// and every sample below is what was actually mixed, not a clock made up for
// it. The sounds are generated here; the repository carries no audio binary.

namespace {

constexpr std::uint32_t voiceRate = 48000;
constexpr double frameSeconds = 1.0 / 60.0;
constexpr std::uint64_t frameSamples = 800; // frameSeconds at the engine's 48 kHz

// A mono 16-bit WAV whose sample n is sample(n / rate).
template <class Sample>
void writeGeneratedWav(const std::filesystem::path &path, double seconds, Sample sample) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios_base::binary};
    const auto frames = static_cast<std::uint32_t>(std::llround(seconds * voiceRate));
    const std::uint32_t data_size = frames * 2;
    file.write("RIFF", 4);
    writeU32(file, 36 + data_size);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    writeU32(file, 16);
    writeU16(file, 1);
    writeU16(file, 1);
    writeU32(file, voiceRate);
    writeU32(file, voiceRate * 2);
    writeU16(file, 2);
    writeU16(file, 16);
    file.write("data", 4);
    writeU32(file, data_size);
    for (std::uint32_t n = 0; n < frames; ++n) {
        const auto value = std::clamp(sample(static_cast<double>(n) / voiceRate), -1.0, 1.0);
        writeU16(file, static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(value * 32767.0))));
    }
}

void writeSineWav(const std::filesystem::path &path, double seconds) {
    // 440 Hz: a whole number of periods in every half second, so a loop over
    // [0.5, 1.0) joins without a step.
    writeGeneratedWav(path, seconds, [](double t) { return 0.5 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * t); });
}

void writeConstantWav(const std::filesystem::path &path, double seconds, double value) {
    writeGeneratedWav(path, seconds, [value](double) { return value; });
}

double peak(const std::vector<float> &samples) {
    double result = 0.0;
    for (const auto sample : samples) result = (std::max)(result, static_cast<double>(std::abs(sample)));
    return result;
}

// One graph, one scene loader and the device-less engine: a frame is the
// renderer's publication (the same writers, from SceneLoader), then one
// frame's worth of mixed audio.
struct VoiceRig {
    Sandbox sandbox;
    std::unique_ptr<FastModuleContainer> modules;
    std::shared_ptr<const ValueGraph::Graph> graph;
    std::vector<float> mixed;

    VoiceRig() {
        ensureLogger();
        modules = std::make_unique<FastModuleContainer>();
        GET_MODULE(EngineLaunchConfig).headless = true;
        GET_MODULE(PathResolver).setup(sandbox.root, false);
        GET_MODULE(EngineTime).setup(EngineTime::Mode::fixed_step, frameSeconds);
    }
    ~VoiceRig() {
        graph.reset();
        modules.reset();
    }

    Audio &audio() { return GET_MODULE(Audio); }
    SceneLoader &scene() { return GET_MODULE(SceneLoader); }
    EngineTime &time() { return GET_MODULE(EngineTime); }

    void load(const nlohmann::json &document) {
        graph = std::make_shared<const ValueGraph::Graph>(document.dump());
    }
    void emit(std::string_view input, Animation::CurvePlaybackCommand command) {
        REQUIRE(graph->emitEvent(scene().graphEvaluationState(graph), input, command, time().now()));
    }
    ValueGraph::PublishResult publish() {
        auto &loader = scene();
        const auto inactive = loader.inactiveGraphScopes(*graph);
        const auto inputs = collectGraphInputs(*graph, loader, inactive, time().now(), time().dt());
        const auto writers = loader.graphAudioWriters(graph, nullptr);
        auto result = graph->publish(GameContext{}, inputs, {}, {}, loader.graphParameterValues(graph), inactive,
                                     &loader.graphEvaluationState(graph), writers.voices, writers.sounds,
                                     writers.values);
        REQUIRE(result.unavailable.empty());
        return result;
    }
    std::vector<float> mix() {
        auto samples = audio().mixOffline(frameSamples);
        REQUIRE(samples.size() == frameSamples * audio().outputChannels());
        mixed.insert(mixed.end(), samples.begin(), samples.end());
        return samples;
    }
    // Publish at the current time, mix until the next frame, advance.
    ValueGraph::PublishResult frame() {
        auto result = publish();
        (void)mix();
        time().advance();
        return result;
    }
    double value(const ValueGraph::PublishResult &result, std::string_view node) const {
        const auto ids = graph->nodeIds();
        const auto found = std::find(ids.begin(), ids.end(), node);
        REQUIRE(found != ids.end());
        const auto &value = result.node_values.at(static_cast<std::size_t>(found - ids.begin()));
        REQUIRE(value);
        return *value;
    }
    SoundHandle voice(std::string_view node) {
        const auto sound = scene().graphSound(graph, node);
        REQUIRE(sound);
        return *sound;
    }
    double cursor(std::string_view node) {
        const auto state = audio().soundState(voice(node));
        REQUIRE(state);
        REQUIRE(state->position_seconds);
        return *state->position_seconds;
    }
};

nlohmann::json graphDocument(nlohmann::json nodes) {
    return nlohmann::json{{"schema", "pelican.graph"}, {"version", 1}, {"nodes", std::move(nodes)},
                          {"outputs", nlohmann::json::object()}};
}

nlohmann::json inputNode(std::string id, std::string clock) {
    return {{"id", std::move(id)}, {"op", "value.input"}, {"parameters", {{"name", std::move(clock)}}},
            {"outputs", {"value"}}};
}

nlohmann::json eventsNode(std::string id) {
    return {{"id", std::move(id)}, {"op", "event.playback"}, {"outputs", {"events"}}};
}

nlohmann::json edge(std::string node, std::string port = "value") {
    return {{"node", std::move(node)}, {"port", std::move(port)}};
}

// Two cursors within one frame and the 10 ms the follow allows.
constexpr double followTolerance = frameSeconds + 0.010;

} // namespace

TEST_CASE("AU-1 a voice follows its position through a time jump, a paused clock and its own pause",
          "[audio][au1][au1-follow]") {
    using Command = Animation::CurvePlaybackCommand;
    VoiceRig rig;
    writeSineWav(rig.sandbox.root / "tone.wav", 2.0);
    rig.load(graphDocument({
        inputNode("now", "time"), eventsNode("cue"), eventsNode("voice_cue"),
        {{"id", "clock"}, {"op", "value.event_clock"},
         {"inputs", {{"time", edge("now")}, {"events", edge("cue", "events")}}}, {"outputs", {"value"}}},
        {{"id", "map"}, {"op", "value.playback"},
         {"parameters", {{"wrap", "repeat"}, {"source_begin", 0}, {"source_end", 2}, {"duration", 2}}},
         {"inputs", {{"elapsed", edge("clock")}}}, {"outputs", {"value"}}},
        {{"id", "voice"}, {"op", "audio.playback"}, {"parameters", {{"asset", "project://tone.wav"}}},
         {"inputs", {{"time", edge("now")}, {"events", edge("voice_cue", "events")}, {"position", edge("map")}}},
         {"outputs", {"voice"}}}}));
    REQUIRE(rig.audio().backendKind() == AudioBackendKind::offline);
    rig.emit("cue", Command::start);
    rig.emit("voice_cue", Command::start);
    const auto tracks = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            const auto published = rig.publish();
            const auto position = rig.value(published, "map");
            CAPTURE(i, position, rig.cursor("voice"));
            REQUIRE(std::abs(rig.cursor("voice") - position) <= followTolerance);
            (void)rig.mix();
            rig.time().advance();
        }
    };
    tracks(20);
    const auto voice = rig.voice("voice");
    REQUIRE(rig.audio().seekCountForTesting(voice) == 0);

    // set_time: the clock jumps 0.75 s; the voice is sought to the new position.
    rig.time().setTime(rig.time().now() + 0.75);
    tracks(20);
    REQUIRE(rig.audio().seekCountForTesting(voice) == 1);
    REQUIRE(rig.voice("voice") == voice);

    // The clock pauses: the position stands still and the voice is held.
    rig.emit("cue", Command::pause);
    (void)rig.publish();
    (void)rig.mix();
    rig.time().advance();
    const auto held_at = rig.cursor("voice");
    for (int i = 0; i < 5; ++i) {
        (void)rig.publish();
        REQUIRE_FALSE(rig.audio().isPlaying(voice));
        REQUIRE(peak(rig.mix()) < 1e-6);
        REQUIRE(rig.cursor("voice") == held_at);
        rig.time().advance();
    }
    rig.emit("cue", Command::resume);
    tracks(10);

    // The voice's own pause wins over a position that keeps moving.
    rig.emit("voice_cue", Command::pause);
    (void)rig.publish();
    (void)rig.mix();
    rig.time().advance();
    const auto paused_at = rig.cursor("voice");
    double moving = -1.0;
    for (int i = 0; i < 5; ++i) {
        const auto published = rig.publish();
        REQUIRE(rig.value(published, "map") > moving);
        moving = rig.value(published, "map");
        REQUIRE(rig.audio().soundState(voice)->paused);
        REQUIRE(peak(rig.mix()) < 1e-6);
        REQUIRE(rig.cursor("voice") == paused_at);
        rig.time().advance();
    }
    rig.emit("voice_cue", Command::resume);
    const auto seeks = rig.audio().seekCountForTesting(voice);
    tracks(10);
    // Resuming finds the position further on and seeks once to it.
    REQUIRE(rig.audio().seekCountForTesting(voice) == seeks + 1);
    REQUIRE(peak(rig.mixed) > 0.4);
}

TEST_CASE("AU-1 the integrated and the decomposed voice point at the same source time",
          "[audio][au1][au1-equivalence]") {
    using Command = Animation::CurvePlaybackCommand;
    // The same map written twice: once as audio.playback's own parameters,
    // once as value.playback feeding its position.
    const auto integrated = graphDocument({
        inputNode("now", "time"), eventsNode("cue"),
        {{"id", "voice"}, {"op", "audio.playback"},
         {"parameters", {{"asset", "project://tone.wav"}, {"wrap", "repeat"}, {"source_begin", 0.5},
                         {"source_end", 1.0}}},
         {"inputs", {{"time", edge("now")}, {"events", edge("cue", "events")}}}, {"outputs", {"voice"}}}});
    const auto decomposed = graphDocument({
        inputNode("now", "time"), eventsNode("cue"),
        {{"id", "clock"}, {"op", "value.event_clock"},
         {"inputs", {{"time", edge("now")}, {"events", edge("cue", "events")}}}, {"outputs", {"value"}}},
        {{"id", "map"}, {"op", "value.playback"},
         {"parameters", {{"wrap", "repeat"}, {"source_begin", 0.5}, {"source_end", 1.0}, {"duration", 0.5}}},
         {"inputs", {{"elapsed", edge("clock")}}}, {"outputs", {"value"}}},
        {{"id", "voice"}, {"op", "audio.playback"}, {"parameters", {{"asset", "project://tone.wav"}}},
         {"inputs", {{"time", edge("now")}, {"events", edge("cue", "events")}, {"position", edge("map")}}},
         {"outputs", {"voice"}}}});
    struct Run {
        std::vector<double> cursors;
        std::size_t seeks = 0;
        std::vector<float> mixed;
        std::uint32_t channels = 0;
    };
    const auto run = [](const nlohmann::json &document) {
        VoiceRig rig;
        writeSineWav(rig.sandbox.root / "tone.wav", 2.0);
        rig.load(document);
        rig.emit("cue", Command::start);
        Run result;
        // 1.5 s: the half-second loop point is crossed twice.
        for (int i = 0; i < 90; ++i) {
            (void)rig.publish();
            result.cursors.push_back(rig.cursor("voice"));
            (void)rig.mix();
            rig.time().advance();
        }
        result.seeks = rig.audio().seekCountForTesting(rig.voice("voice"));
        result.mixed = rig.mixed;
        result.channels = rig.audio().outputChannels();
        return result;
    };
    const auto one = run(integrated);
    const auto split = run(decomposed);
    REQUIRE(one.cursors.size() == split.cursors.size());
    // The integrated voice loops at the data source's loop point: never sought,
    // and its samples join without a step. The decomposed one is sought back
    // at every loop point.
    REQUIRE(one.seeks == 0);
    REQUIRE(split.seeks >= 2);
    double largest_step = 0.0;
    for (std::size_t n = one.channels; n < one.mixed.size(); n += one.channels)
        largest_step = (std::max)(largest_step, static_cast<double>(std::abs(one.mixed[n] - one.mixed[n - one.channels])));
    // 440 Hz at amplitude 0.5 moves at most 2*pi*440/48000*0.5 = 0.029 a sample.
    REQUIRE(largest_step < 0.04);
    bool looped = false;
    for (std::size_t i = 0; i < one.cursors.size(); ++i) {
        CAPTURE(i, one.cursors[i], split.cursors[i]);
        REQUIRE(one.cursors[i] >= 0.5 - 1e-9);
        REQUIRE(one.cursors[i] < 1.0);
        // The decomposed voice may overrun the loop end by up to one frame
        // before it is sought back; on the loop, the two are the same place.
        REQUIRE(split.cursors[i] < 1.0 + followTolerance);
        const auto apart = std::fmod(std::abs(one.cursors[i] - split.cursors[i]), 0.5);
        REQUIRE((std::min)(apart, 0.5 - apart) <= followTolerance);
        if (i > 0 && one.cursors[i] < one.cursors[i - 1]) looped = true;
    }
    REQUIRE(looped);
}

TEST_CASE("AU-1 an integrated voice on its loop point comes back to the graph's time after a jump",
          "[audio][au1][au1-equivalence]") {
    using Command = Animation::CurvePlaybackCommand;
    VoiceRig rig;
    writeSineWav(rig.sandbox.root / "tone.wav", 2.0);
    rig.load(graphDocument({
        inputNode("now", "time"), eventsNode("cue"),
        {{"id", "voice"}, {"op", "audio.playback"},
         {"parameters", {{"asset", "project://tone.wav"}, {"wrap", "repeat"}, {"source_begin", 0.5},
                         {"source_end", 1.5}}},
         {"inputs", {{"time", edge("now")}, {"events", edge("cue", "events")}}}, {"outputs", {"voice"}}}}));
    rig.emit("cue", Command::start);
    for (int i = 0; i < 10; ++i) (void)rig.frame();
    const auto voice = rig.voice("voice");
    REQUIRE(rig.audio().seekCountForTesting(voice) == 0);
    // A jump far past the frame-to-mixer drift: the voice is brought back once.
    rig.time().setTime(rig.time().now() + 0.4);
    (void)rig.publish();
    REQUIRE(rig.audio().seekCountForTesting(voice) == 1);
    const auto expected = 0.5 + std::fmod(rig.time().now(), 1.0);
    REQUIRE(std::abs(rig.cursor("voice") - expected) <= followTolerance);
    (void)rig.mix();
    rig.time().advance();
    for (int i = 0; i < 20; ++i) (void)rig.frame();
    REQUIRE(rig.audio().seekCountForTesting(voice) == 1);
    REQUIRE(rig.voice("voice") == voice);
}

TEST_CASE("AU-1 a bgm voice follows the Bgm bus and not the Se bus", "[audio][au1][au1-bus]") {
    using Command = Animation::CurvePlaybackCommand;
    VoiceRig rig;
    writeConstantWav(rig.sandbox.root / "level.wav", 1.0, 0.5);
    rig.load(graphDocument({
        inputNode("now", "time"), eventsNode("cue"),
        {{"id", "voice"}, {"op", "audio.playback"},
         {"parameters", {{"asset", "project://level.wav"}, {"bus", "bgm"}, {"wrap", "repeat"}}},
         {"inputs", {{"time", edge("now")}, {"events", edge("cue", "events")}}}, {"outputs", {"voice"}}}}));
    rig.emit("cue", Command::start);
    (void)rig.frame();
    const auto full = peak(rig.mix());
    REQUIRE(full > 0.3);
    GameContext context;
    context.audio().setBusVolume("se", 0.0f);
    rig.audio().update();
    (void)rig.frame();
    REQUIRE(std::abs(peak(rig.mix()) - full) < 1e-4);
    context.audio().setBusVolume("bgm", 0.5f);
    rig.audio().update();
    (void)rig.frame();
    REQUIRE(std::abs(peak(rig.mix()) - full * 0.5) < 1e-4);
    REQUIRE(rig.audio().soundState(rig.voice("voice"))->effective_volume == 0.5f);

    // The same bus through GameContext::playSound.
    context.audio().setBusVolume("bgm", 1.0f);
    context.audio().setBusVolume("se", 1.0f);
    rig.emit("cue", Command::reset);
    (void)rig.frame();
    REQUIRE(peak(rig.mix()) < 1e-6);
    const auto sound = context.audio().playSound("project://level.wav", "bgm");
    (void)rig.mix();
    const auto alone = peak(rig.mix());
    REQUIRE(alone > 0.3);
    context.audio().setBusVolume("se", 0.0f);
    rig.audio().update();
    REQUIRE(std::abs(peak(rig.mix()) - alone) < 1e-4);
    context.audio().setBusVolume("bgm", 0.0f);
    rig.audio().update();
    REQUIRE(peak(rig.mix()) < 1e-6);
    context.audio().stopSound(sound);
    REQUIRE_THROWS_WITH(context.audio().playSound("project://level.wav", "master"),
                        Catch::Matchers::ContainsSubstring("Unknown audio bus"));
}

TEST_CASE("AU-1 a volume curve reaches each frame's value over that frame", "[audio][au1][au1-fade]") {
    using Command = Animation::CurvePlaybackCommand;
    VoiceRig rig;
    writeConstantWav(rig.sandbox.root / "level.wav", 2.0, 0.5);
    rig.load(graphDocument({
        inputNode("now", "time"), eventsNode("cue"),
        {{"id", "voice"}, {"op", "audio.playback"},
         {"parameters", {{"asset", "project://level.wav"}, {"wrap", "repeat"}}},
         {"inputs", {{"time", edge("now")}, {"events", edge("cue", "events")}}}, {"outputs", {"voice"}}},
        {{"id", "fade"}, {"op", "value.curve"},
         {"parameters", {{"keys", {{{"time", 0.0}, {"value", 0.0}}, {{"time", 1.0}, {"value", 1.0}}}}}},
         {"inputs", {{"time", edge("now")}}}, {"outputs", {"value"}}},
        {{"id", "level"}, {"op", "audio.volume"},
         {"inputs", {{"value", edge("fade")}, {"voice", edge("voice", "voice")}}}, {"outputs", nlohmann::json::array()}}}));
    rig.emit("cue", Command::start);
    const auto channels = rig.audio().outputChannels();
    std::vector<float> first;
    for (int i = 0; i < 40; ++i) {
        (void)rig.publish();
        auto samples = rig.mix();
        rig.time().advance();
        if (i == 0) first = samples;
    }
    // The first frame starts at its value (0): silence.
    REQUIRE(peak(first) < 1e-6);
    // Afterwards the envelope is the curve itself, piecewise linear between
    // the frames' values: no step at a frame boundary, a constant slope within.
    const auto &mixed = rig.mixed;
    const auto level = static_cast<double>(mixed[mixed.size() - channels]);
    const auto last_value = 39.0 * frameSeconds;
    REQUIRE(level > 0.0);
    const auto scale = level / last_value; // the constant source through the mixer
    double largest_step = 0.0;
    for (std::size_t n = frameSamples * channels; n < mixed.size(); n += channels)
        largest_step = (std::max)(largest_step, static_cast<double>(std::abs(mixed[n] - mixed[n - channels])));
    // Linear: one sample moves scale/48000. A step per frame would be scale/60.
    CAPTURE(largest_step, scale);
    REQUIRE(largest_step < scale / 60.0 / 20.0);
    REQUIRE(largest_step > scale / voiceRate * 0.5);
    for (int frame = 1; frame < 40; ++frame) {
        const auto sample = [&](std::uint64_t n) { return static_cast<double>(mixed[(frame * frameSamples + n) * channels]); };
        // At the end of frame k the envelope has reached the curve at t_k.
        REQUIRE(std::abs(sample(frameSamples - 1) - scale * frame * frameSeconds) < scale * 2e-3);
    }
}

TEST_CASE("AU-1 an asset decodes once however many voices play it", "[audio][au1][au1-decode]") {
    using Command = Animation::CurvePlaybackCommand;
    VoiceRig rig;
    writeSineWav(rig.sandbox.root / "tone.wav", 0.5);
    rig.load(graphDocument({
        inputNode("now", "time"), eventsNode("cue"),
        {{"id", "voice"}, {"op", "audio.playback"}, {"parameters", {{"asset", "project://tone.wav"}}},
         {"inputs", {{"time", edge("now")}, {"events", edge("cue", "events")}}}, {"outputs", {"voice"}}}}));
    // What the rendering config registration does to check the asset.
    (void)rig.audio().prepareSound("project://tone.wav");
    REQUIRE(rig.audio().decodeCountForTesting() == 1);
    rig.emit("cue", Command::start);
    (void)rig.frame();
    const auto first = rig.voice("voice");
    rig.emit("cue", Command::start);
    (void)rig.frame();
    const auto second = rig.voice("voice");
    REQUIRE(first != second);
    REQUIRE_FALSE(rig.audio().soundState(first));
    (void)GameContext{}.audio().playSound("project://tone.wav");
    REQUIRE(rig.audio().decodeCountForTesting() == 1);
    // An asset edited in place is decoded again.
    std::filesystem::last_write_time(rig.sandbox.root / "tone.wav",
        std::filesystem::last_write_time(rig.sandbox.root / "tone.wav") + std::chrono::seconds{2});
    (void)GameContext{}.audio().playSound("project://tone.wav");
    REQUIRE(rig.audio().decodeCountForTesting() == 2);
}

TEST_CASE("AU-1 a document that mixes the two forms or two clocks is refused by name",
          "[audio][au1][au1-errors]") {
    const auto voice = [](nlohmann::json parameters, nlohmann::json inputs) {
        parameters["asset"] = "project://tone.wav";
        return nlohmann::json{{"id", "voice"}, {"op", "audio.playback"}, {"parameters", std::move(parameters)},
                              {"inputs", std::move(inputs)}, {"outputs", {"voice"}}};
    };
    const auto map = [](std::string clock) {
        return nlohmann::json{{"id", "map"}, {"op", "value.playback"},
            {"parameters", {{"duration", 1}}}, {"inputs", {{"elapsed", edge(std::move(clock))}}}, {"outputs", {"value"}}};
    };
    const auto refused = [](const nlohmann::json &document, const std::string &code) {
        try {
            const ValueGraph::Graph graph{document.dump()};
            FAIL("accepted: " << document.dump());
        } catch (const GraphRegistryError &error) {
            REQUIRE(error.code() == code);
            REQUIRE(error.detail().at("node") == "voice");
            return error.detail();
        }
        return nlohmann::json{};
    };
    // A position and a map on one node.
    const auto conflict = refused(graphDocument({inputNode("now", "time"), map("now"),
        voice({{"wrap", "repeat"}, {"speed", 2}}, {{"time", edge("now")}, {"position", edge("map")}})}),
        "audio.playback_mapping_conflict@1");
    REQUIRE(conflict.at("parameters") == nlohmann::json::array({"wrap", "speed"}));
    (void)refused(graphDocument({inputNode("now", "time"), map("now"),
        voice({}, {{"time", edge("now")}, {"position", edge("map")}, {"source_begin", edge("now")}})}),
        "audio.playback_mapping_conflict@1");
    // Scene-stamped events read on scene_time, a position on app time.
    const auto mismatch = refused(graphDocument({inputNode("now", "time"), inputNode("scene", "scene_time"),
        eventsNode("cue"), map("now"),
        voice({}, {{"time", edge("scene")}, {"events", edge("cue", "events")}, {"position", edge("map")}})}),
        "audio.clock_mismatch@1");
    REQUIRE(mismatch.at("events_clock") == nlohmann::json::array({"scene_time"}));
    REQUIRE(mismatch.at("position_clock") == nlohmann::json::array({"time"}));
    // The controls: one clock for both, or no events at all, is accepted.
    REQUIRE_NOTHROW(ValueGraph::Graph{graphDocument({inputNode("scene", "scene_time"), eventsNode("cue"),
        map("scene"), voice({}, {{"time", edge("scene")}, {"events", edge("cue", "events")},
                                 {"position", edge("map")}})}).dump()});
    REQUIRE_NOTHROW(ValueGraph::Graph{graphDocument({inputNode("now", "time"), inputNode("scene", "scene_time"),
        map("now"), voice({{"bus", "bgm"}}, {{"time", edge("scene")}, {"position", edge("map")}})}).dump()});
    // The integrated form still needs its events.
    REQUIRE_THROWS_WITH(ValueGraph::Graph{graphDocument({inputNode("now", "time"),
        voice({{"wrap", "repeat"}}, {{"time", edge("now")}})}).dump()},
        Catch::Matchers::ContainsSubstring("unconnected graph input: voice.events"));
    REQUIRE_THROWS_WITH(ValueGraph::Graph{graphDocument({inputNode("now", "time"), eventsNode("cue"),
        voice({{"bus", "master"}}, {{"time", edge("now")}, {"events", edge("cue", "events")}})}).dump()},
        Catch::Matchers::ContainsSubstring("unknown audio bus"));
}

TEST_CASE("AU-1 a player with a window opens the audio device, a headless one the device-less engine",
          "[audio][au1][au1-backend]") {
    REQUIRE(preferredAudioBackend(true) == AudioBackendKind::offline);
    REQUIRE(preferredAudioBackend(false) == AudioBackendKind::device);
    ensureLogger();
    FastModuleContainer modules;
    // The Studio's embedded player: RPC with a window.
    GET_MODULE(EngineLaunchConfig).rpc = true;
    GET_MODULE(EngineLaunchConfig).headless = false;
    const auto &audio = GET_MODULE(Audio);
    if (audio.backendKind() == AudioBackendKind::null && !audio.backendFallbackReason().empty())
        SKIP("no audio device here: " + audio.backendFallbackReason());
    REQUIRE(audio.backendKind() == AudioBackendKind::device);
}

TEST_CASE("AU-1 a scene change stops the graph's voices and a time-clocked BGM carries on",
          "[audio][au1][au1-scene]") {
    VoiceRig rig;
    writeSineWav(rig.sandbox.root / "bgm.wav", 2.0);
    writeText(rig.sandbox.root / "passes/main_rendering_config.json",
              R"({"schema":"pelican.graph","version":1,"definitions":{},"nodes":[],"outputs":{},"rendering":{}})");
    std::ofstream{rig.sandbox.root / "scene.json", std::ios::binary} << R"({"schema":"pelican.scene","version":2,"scenes":{
      "first":{"objects":[{"name":"Bgm","components":[{"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}]}]},
      "second":{"objects":[{"name":"Floor","components":[{"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}]},
                           {"name":"Bgm","components":[{"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}]}]}}})";
    GET_MODULE(ProjectSource).setProjectData(nlohmann::json{{"schema", "pelican.project"}, {"version", 1},
        {"name", "au1-scene"}, {"engine_min_version", "0.1.0"},
        {"basic_config", {{"window_size", {{"width", 64}, {"height", 64}}},
            {"camera", {{"yfov", 0.5}, {"znear", 0.1}, {"zfar", 100.0}, {"up", {0.0, 1.0, 0.0}}}},
            {"default_scene_id", "first"}, {"scene_data_json", "scene.json"}}}}.dump());
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &scene = rig.scene();
    const auto authoringId = [&](SceneId id) {
        test_support::loadScene(scene, std::move(id));
        const auto object = scene.objectId("Bgm");
        REQUIRE(object);
        for (const auto &binding : scene.runtimeObjectBindings())
            if (binding.object_id == *object) return binding.authoring_object_id.value;
        FAIL("the Bgm object has no authoring id");
        return std::uint64_t{};
    };
    const auto second_id = authoringId(SceneId{"second"});
    const auto first_id = authoringId(SceneId{"first"});
    // One BGM definition, owned by an object of the same name in each scene,
    // clocked by app time and started by its owner (no events).
    nlohmann::json document{{"schema", "pelican.graph"}, {"version", 1},
        {"definitions", {{"bgm", {{"nodes", {
            inputNode("now", "time"),
            {{"id", "map"}, {"op", "value.playback"},
             {"parameters", {{"wrap", "repeat"}, {"source_begin", 0}, {"source_end", 2}, {"duration", 2}}},
             {"inputs", {{"elapsed", edge("now")}}}, {"outputs", {"value"}}},
            {{"id", "voice"}, {"op", "audio.playback"},
             {"parameters", {{"asset", "project://bgm.wav"}, {"bus", "bgm"}}},
             {"inputs", {{"time", edge("now")}, {"position", edge("map")}}}, {"outputs", {"voice"}}}}},
            {"outputs", nlohmann::json::object()}}}}},
        {"nodes", {
            {{"id", "first_bgm"}, {"op", "graph.instance"}, {"definition", "bgm"},
             {"owner", {{"scene", "first"}, {"object", first_id}}}},
            {{"id", "second_bgm"}, {"op", "graph.instance"}, {"definition", "bgm"},
             {"owner", {{"scene", "second"}, {"object", second_id}}}}}},
        {"outputs", nlohmann::json::object()}};
    rig.load(document);
    std::string first_voice, second_voice;
    for (const auto &[node, asset] : rig.graph->audioSources())
        (node.starts_with("first_bgm") ? first_voice : second_voice) = node;
    REQUIRE_FALSE(first_voice.empty());
    REQUIRE_FALSE(second_voice.empty());

    rig.time().setTime(0.4);
    for (int i = 0; i < 30; ++i) (void)rig.frame();
    const auto playing = rig.voice(first_voice);
    REQUIRE(rig.audio().isPlaying(playing));
    REQUIRE_FALSE(scene.graphSound(rig.graph, second_voice));

    test_support::loadScene(scene, SceneId{"second"});
    // Decision 1: the commit stopped the first scene's voice.
    REQUIRE_FALSE(rig.audio().soundState(playing));
    REQUIRE(rig.audio().backendVoiceCountForTesting() == 0);
    REQUIRE(peak(rig.mix()) < 1e-6);

    // Decision 8: the next scene's owner starts the same BGM where app time
    // says it is.
    const auto published = rig.publish();
    const auto expected = std::fmod(rig.time().now(), 2.0);
    REQUIRE(std::abs(rig.value(published, second_voice.substr(0, second_voice.rfind('/')) + "/map") - expected) < 1e-9);
    REQUIRE(std::abs(rig.cursor(second_voice) - expected) <= followTolerance);
    (void)rig.mix();
    rig.time().advance();
    for (int i = 0; i < 10; ++i) {
        const auto frame = rig.publish();
        REQUIRE(std::abs(rig.cursor(second_voice) - std::fmod(rig.time().now(), 2.0)) <= followTolerance);
        (void)frame;
        (void)rig.mix();
        rig.time().advance();
    }
    REQUIRE(peak(rig.mixed) > 0.4);
}

} // namespace Pelican
