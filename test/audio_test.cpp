#include "../src/core/audio/audio.hpp"
#include "../src/core/audio/audioclock.hpp"
#include "../src/core/appflow/framephase.hpp"
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/log.hpp"
#include "../src/core/userpublic/gamecontext.hpp"
#include "../src/core/userpublic/valuegraph.hpp"
#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/appflow/playmode.hpp"
#include "../src/core/communication/audiostatus.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/renderingpass/graphinputs.hpp"
#include "../src/project/graphcomposition.hpp"
#include "../src/project/graphmodule.hpp"
#include "scene_load_test_support.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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
    // RIFF/WAVE by its first bytes, but its fmt chunk runs past the end.
    writeText(sandbox.root / "broken.wav", std::string_view{"RIFF\x20\0\0\0WAVEfmt \x10\0\0\0\x01\0", 22});

    FastModuleContainer modules;
    configureNullAudioProject(sandbox);

    GameContext context;
    // AU-2: first bytes that are not a WAV's are not a WAV, whatever the name says.
    REQUIRE_THROWS_WITH(context.audio().playSound("project://bad.wav"),
                        Catch::Matchers::ContainsSubstring("audio.format_unsupported@1"));
    REQUIRE_THROWS_WITH(context.audio().playSound("project://broken.wav"),
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
        // Stamped on scene time, as SceneLoader::emitGraphEvent does.
        REQUIRE(graph->emitEvent(scene().graphEvaluationState(graph), input, command,
                                 time().now() - scene().sceneStartTime()));
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
        inputNode("now", "scene_time"), eventsNode("cue"), eventsNode("voice_cue"),
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
        inputNode("now", "scene_time"), eventsNode("cue"),
        {{"id", "voice"}, {"op", "audio.playback"},
         {"parameters", {{"asset", "project://tone.wav"}, {"wrap", "repeat"}, {"source_begin", 0.5},
                         {"source_end", 1.0}}},
         {"inputs", {{"time", edge("now")}, {"events", edge("cue", "events")}}}, {"outputs", {"voice"}}}});
    const auto decomposed = graphDocument({
        inputNode("now", "scene_time"), eventsNode("cue"),
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
        inputNode("now", "scene_time"), eventsNode("cue"),
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
        inputNode("now", "scene_time"), eventsNode("cue"),
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
        inputNode("now", "scene_time"), eventsNode("cue"),
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
        inputNode("now", "scene_time"), eventsNode("cue"),
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
    // An asset edited in place is decoded again in a hot reload build. EB-7:
    // a distribution build (PELICAN_WITH_HOT_RELOAD=OFF) looks at the file
    // only when it first decodes it, and plays what it decoded.
    std::filesystem::last_write_time(rig.sandbox.root / "tone.wav",
        std::filesystem::last_write_time(rig.sandbox.root / "tone.wav") + std::chrono::seconds{2});
    (void)GameContext{}.audio().playSound("project://tone.wav");
    REQUIRE(rig.audio().decodeCountForTesting() == (PELICAN_WITH_HOT_RELOAD ? 2u : 1u));
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
    // AU-3: the integrated form no longer needs its events (it was refused
    // as "unconnected graph input: voice.events" here until then); without
    // them its time may come from any clock.
    REQUIRE_NOTHROW(ValueGraph::Graph{graphDocument({inputNode("now", "time"),
        voice({{"wrap", "repeat"}}, {{"time", edge("now")}})}).dump()});
    REQUIRE_THROWS_WITH(ValueGraph::Graph{graphDocument({inputNode("now", "scene_time"), eventsNode("cue"),
        voice({{"bus", "master"}}, {{"time", edge("now")}, {"events", edge("cue", "events")}})}).dump()},
        Catch::Matchers::ContainsSubstring("graph.render_node_parameter_invalid@1") &&
            Catch::Matchers::ContainsSubstring("\"parameter\":\"bus\""));
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
            {"camera", {{"yfov", 0.5}, {"znear", 0.1}, {"zfar", 100.0}}},
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

// CT-1: get_status.audio.voices -- each audio.playback node's state and the
// source second it stands at, through a play session as Studio drives it:
// the play state moves the way set_play_state moves it (changePlayState, a
// scene load as the session's rebuild), and the frames are the renderer's
// publication. The backend is the null one, which has no clock of its own:
// the values come from what the graph asked of each voice.
TEST_CASE("CT-1 get_status lists each graph voice's state and position, the same on the null backend",
          "[audio][ct1][ct1-voices]") {
    using State = EngineTime::PlayState;
    VoiceRig rig;
    GET_MODULE(EngineLaunchConfig).audio_backend = AudioBackendKind::null;
    writeSineWav(rig.sandbox.root / "tone.wav", 2.0);
    writeText(rig.sandbox.root / "passes/main_rendering_config.json",
              R"({"schema":"pelican.graph","version":1,"definitions":{},"nodes":[],"outputs":{},"rendering":{}})");
    std::ofstream{rig.sandbox.root / "scene.json", std::ios::binary} << R"({"schema":"pelican.scene","version":2,"scenes":{
      "first":{"objects":[{"name":"Stage","components":[{"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}]}]}}})";
    GET_MODULE(ProjectSource).setProjectData(nlohmann::json{{"schema", "pelican.project"}, {"version", 1},
        {"name", "ct1-voices"}, {"engine_min_version", "0.1.0"},
        {"basic_config", {{"window_size", {{"width", 64}, {"height", 64}}},
            {"camera", {{"yfov", 0.5}, {"znear", 0.1}, {"zfar", 100.0}}},
            {"default_scene_id", "first"}, {"scene_data_json", "scene.json"}}}}.dump());
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &scene = rig.scene();
    const auto rebuild = [&] { test_support::loadScene(scene, SceneId{"first"}); };
    // An engine that starts in edit, as Studio launches it.
    rig.time().setPlayState(State::edit);
    rebuild();
    // A voice with neither events nor a position: it plays from its first
    // frame on scene time, the integrated form.
    rig.load(graphDocument({inputNode("now", "scene_time"),
        {{"id", "voice"}, {"op", "audio.playback"}, {"parameters", {{"asset", "project://tone.wav"}}},
         {"inputs", {{"time", edge("now")}}}, {"outputs", {"voice"}}}}));
    REQUIRE(rig.audio().backendKind() == AudioBackendKind::null);
    const auto voices = [] {
        const auto status = internal::audioStatusJson();
        INFO(status.dump());
        REQUIRE(status.at("backend") == "null");
        REQUIRE(status.at("voices").size() == 1);
        const auto &voice = status.at("voices").at(0);
        REQUIRE(voice.at("node") == "voice");
        return voice;
    };
    // The frames the null backend gets: the publication only (it mixes nothing).
    const auto frame = [&] {
        (void)rig.publish();
        rig.time().advance();
    };

    // Edit: nothing plays.
    for (int i = 0; i < 3; ++i) frame();
    CHECK(voices() == nlohmann::json{{"node", "voice"}, {"state", "idle"}, {"position_seconds", 0.0}});

    // Play: playing, the position going on with the game clock from 0.
    REQUIRE(internal::changePlayState(State::playing, rebuild).previous == State::edit);
    for (int i = 0; i < 10; ++i) {
        (void)rig.publish();
        const auto voice = voices();
        CAPTURE(i, voice.dump());
        CHECK(voice.at("state") == "playing");
        CHECK(voice.at("position_seconds").get<double>() == Catch::Approx(i * frameSeconds).margin(1e-9));
        rig.time().advance();
    }
    const auto played_to = voices().at("position_seconds").get<double>();
    REQUIRE(played_to > 0.1);
    // AT-1 decision 6: the same status carries the clock and the master. The
    // null backend has no clock of its own: the game clock stands in for it,
    // and this voice is not a master.
    {
        const auto status = internal::audioStatusJson();
        INFO(status.dump());
        CHECK(status.at("clock") == nlohmann::json{{"source", "game_clock"}, {"sample_rate", 0},
                                                   {"audio_time", rig.time().now()}, {"played_frames", 0},
                                                   {"output_latency_seconds", 0.0}, {"latency_source", "none"},
                                                   {"drift_seconds", 0.0}});
        CHECK(status.at("master").is_null());
    }

    // Pause: paused, standing where it was.
    REQUIRE(internal::changePlayState(State::paused, {}).previous == State::playing);
    for (int i = 0; i < 5; ++i) {
        frame();
        const auto voice = voices();
        CAPTURE(i, voice.dump());
        CHECK(voice.at("state") == "paused");
        CHECK(voice.at("position_seconds").get<double>() == Catch::Approx(played_to + frameSeconds).margin(1e-9));
    }

    // Stop: idle at 0, and it stays so through the edit frames.
    REQUIRE(internal::changePlayState(State::edit, rebuild).previous == State::paused);
    CHECK(voices() == nlohmann::json{{"node", "voice"}, {"state", "idle"}, {"position_seconds", 0.0}});
    for (int i = 0; i < 3; ++i) frame();
    CHECK(voices() == nlohmann::json{{"node", "voice"}, {"state", "idle"}, {"position_seconds", 0.0}});
}

// ---------------------------------------------------------------------------
// AU-2: Ogg Vorbis, streaming, the format check and events on scene time. The
// one audio binary in the repository is test/fixtures/audio/sine440_1s.ogg:
// 1 s of a 440 Hz sine at amplitude 0.5, mono, 48 kHz (see its provenance).

namespace {

std::filesystem::path oggFixture() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" / "audio" / "sine440_1s.ogg";
}

void copyOggFixture(const std::filesystem::path &to) {
    std::filesystem::create_directories(to.parent_path());
    std::filesystem::copy_file(oggFixture(), to, std::filesystem::copy_options::overwrite_existing);
}

double sine440(double seconds) {
    return 0.5 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * seconds);
}

// The integrated form: audio.playback's own map, looping over [begin, end).
nlohmann::json loopingVoice(std::string asset, double begin, double end) {
    return graphDocument({
        inputNode("now", "scene_time"), eventsNode("cue"),
        {{"id", "voice"}, {"op", "audio.playback"},
         {"parameters", {{"asset", std::move(asset)}, {"wrap", "repeat"}, {"source_begin", begin},
                         {"source_end", end}}},
         {"inputs", {{"time", edge("now")}, {"events", edge("cue", "events")}}}, {"outputs", {"voice"}}}});
}

// The decomposed form: the same map as value.playback, feeding position.
nlohmann::json followingVoice(std::string asset, double begin, double end) {
    return graphDocument({
        inputNode("now", "scene_time"), eventsNode("cue"),
        {{"id", "clock"}, {"op", "value.event_clock"},
         {"inputs", {{"time", edge("now")}, {"events", edge("cue", "events")}}}, {"outputs", {"value"}}},
        {{"id", "map"}, {"op", "value.playback"},
         {"parameters", {{"wrap", "repeat"}, {"source_begin", begin}, {"source_end", end}, {"duration", end - begin}}},
         {"inputs", {{"elapsed", edge("clock")}}}, {"outputs", {"value"}}},
        {{"id", "voice"}, {"op", "audio.playback"}, {"parameters", {{"asset", std::move(asset)}}},
         {"inputs", {{"time", edge("now")}, {"events", edge("cue", "events")}, {"position", edge("map")}}},
         {"outputs", {"voice"}}}});
}

struct LoopRun {
    std::vector<double> cursors;
    std::vector<float> mixed;
    std::uint32_t channels = 0;
    std::size_t seeks = 0;
    std::size_t decodes = 0;
    std::size_t streamed = 0;
};

// 1.5 s of one voice over [0.5, 1.0): the loop point is crossed twice. A
// threshold of zero leaves the streaming threshold at its default.
LoopRun runLoop(const nlohmann::json &document, double threshold) {
    using Command = Animation::CurvePlaybackCommand;
    VoiceRig rig;
    if (threshold > 0.0) rig.audio().setStreamingThresholdForTesting(threshold);
    writeSineWav(rig.sandbox.root / "tone.wav", 2.0);
    copyOggFixture(rig.sandbox.root / "tone.ogg");
    rig.load(document);
    rig.emit("cue", Command::start);
    LoopRun result;
    for (int i = 0; i < 90; ++i) {
        (void)rig.publish();
        result.cursors.push_back(rig.cursor("voice"));
        (void)rig.mix();
        rig.time().advance();
    }
    result.mixed = rig.mixed;
    result.channels = rig.audio().outputChannels();
    result.seeks = rig.audio().seekCountForTesting(rig.voice("voice"));
    result.decodes = rig.audio().decodeCountForTesting();
    result.streamed = rig.audio().streamedVoiceCountForTesting();
    return result;
}

// The integrated voice held its own loop point: never sought, always on
// [0.5, 1.0), and it did come back to 0.5.
void requireNativeLoop(const LoopRun &run) {
    REQUIRE(run.seeks == 0);
    bool looped = false;
    for (std::size_t i = 0; i < run.cursors.size(); ++i) {
        CAPTURE(i, run.cursors[i]);
        REQUIRE(run.cursors[i] >= 0.5 - 1e-9);
        REQUIRE(run.cursors[i] < 1.0);
        if (i > 0 && run.cursors[i] < run.cursors[i - 1]) looped = true;
    }
    REQUIRE(looped);
}

// The decomposed voice points where the integrated one does, modulo the loop.
void requireSameSourceTime(const LoopRun &one, const LoopRun &split) {
    REQUIRE(one.cursors.size() == split.cursors.size());
    REQUIRE(split.seeks >= 2);
    for (std::size_t i = 0; i < one.cursors.size(); ++i) {
        CAPTURE(i, one.cursors[i], split.cursors[i]);
        REQUIRE(split.cursors[i] < 1.0 + followTolerance);
        const auto apart = std::fmod(std::abs(one.cursors[i] - split.cursors[i]), 0.5);
        REQUIRE((std::min)(apart, 0.5 - apart) <= followTolerance);
    }
}

double largestStep(const LoopRun &run) {
    double largest = 0.0;
    for (std::size_t n = run.channels; n < run.mixed.size(); n += run.channels)
        largest = (std::max)(largest, static_cast<double>(std::abs(run.mixed[n] - run.mixed[n - run.channels])));
    return largest;
}

// Where the samples of one mixed frame are the sine from `source_seconds` on:
// the largest difference, first channel. The engine's resampler may hold a
// sample or two back, so the best of those few lags counts; a wrong place in
// the source is off by far more (one sample of a 440 Hz sine is 0.029).
double sineError(const std::vector<float> &samples, std::uint32_t channels, double source_seconds) {
    double best = std::numeric_limits<double>::infinity();
    for (int lag = 0; lag <= 2; ++lag) {
        double error = 0.0;
        for (std::size_t n = static_cast<std::size_t>(lag); n * channels < samples.size(); ++n)
            error = (std::max)(error, std::abs(static_cast<double>(samples[n * channels]) -
                                               sine440(source_seconds + static_cast<double>(n - lag) / voiceRate)));
        best = (std::min)(best, error);
    }
    return best;
}

// A WAV whose every sample says where it is: -0.5 at 0 s, rising to 0.5 at 2 s.
double ramp(double seconds) {
    return (seconds - 1.0) / 2.0;
}

// Where in `samples` (first channel) the ramp from `source_seconds` begins: the
// offset k for which every sample from k on is ramp(source_seconds + (n - k)
// / rate), and the largest difference there.
std::pair<std::size_t, double> rampOffset(const std::vector<float> &samples, std::uint32_t channels,
                                          double source_seconds, std::size_t most) {
    std::pair<std::size_t, double> best{0, std::numeric_limits<double>::infinity()};
    const auto frames = samples.size() / channels;
    for (std::size_t k = 0; k <= most && k < frames; ++k) {
        double error = 0.0;
        for (std::size_t n = k; n < frames && error < best.second; ++n)
            error = (std::max)(error, std::abs(static_cast<double>(samples[n * channels]) -
                                               ramp(source_seconds + static_cast<double>(n - k) / voiceRate)));
        if (error < best.second) best = {k, error};
    }
    return best;
}

// The decomposed voice over [0, 1) of the source, then a set_time jump: one
// seek, and the voice follows from the new position. With the ramp, the
// samples say where the voice is: the ones mixed after the seek are the
// source from the sought frame on. (What miniaudio already held in its
// processing cache -- up to 1024 frames read before the seek -- is mixed
// first; the cursor reports the sought frame at once.)
void requireFollowSeek(std::string asset, double threshold, double lossy) {
    using Command = Animation::CurvePlaybackCommand;
    VoiceRig rig;
    if (threshold > 0.0) rig.audio().setStreamingThresholdForTesting(threshold);
    writeSineWav(rig.sandbox.root / "tone.wav", 2.0);
    writeGeneratedWav(rig.sandbox.root / "ramp.wav", 2.0, ramp);
    copyOggFixture(rig.sandbox.root / "tone.ogg");
    const bool ramped = asset.ends_with("ramp.wav");
    rig.load(followingVoice(std::move(asset), 0.0, 1.0));
    rig.emit("cue", Command::start);
    const auto channels = rig.audio().outputChannels();
    // The first frame starts at the source's beginning.
    (void)rig.publish();
    const auto first = rig.mix();
    if (ramped) {
        const auto [offset, error] = rampOffset(first, channels, 0.0, 2);
        CAPTURE(offset, error);
        REQUIRE(error < lossy);
    } else REQUIRE(sineError(first, channels, 0.0) < lossy);
    rig.time().advance();
    for (int i = 0; i < 10; ++i) (void)rig.frame();
    const auto voice = rig.voice("voice");
    REQUIRE(rig.audio().seekCountForTesting(voice) == 0);
    rig.time().setTime(rig.time().now() + 0.3);
    const auto published = rig.publish();
    REQUIRE(rig.audio().seekCountForTesting(voice) == 1);
    const auto target = rig.cursor("voice");
    REQUIRE(std::abs(target - rig.value(published, "map")) <= followTolerance);
    CAPTURE(target);
    std::vector<float> after = rig.mix();
    rig.time().advance();
    for (int i = 0; i < 10; ++i) {
        const auto frame = rig.publish();
        REQUIRE(std::abs(rig.cursor("voice") - rig.value(frame, "map")) <= followTolerance);
        const auto samples = rig.mix();
        if (i == 0) after.insert(after.end(), samples.begin(), samples.end());
        rig.time().advance();
    }
    REQUIRE(rig.audio().seekCountForTesting(voice) == 1);
    REQUIRE(rig.voice("voice") == voice);
    if (ramped) {
        // Two frames from the seek on: the sought frame appears within the
        // cache's length and the ramp runs on from it without a gap.
        const auto [offset, error] = rampOffset(after, channels, target, 1100);
        CAPTURE(offset, error);
        REQUIRE(offset <= 1100);
        REQUIRE(error < lossy);
    }
}

// The first bytes of an Ogg stream whose first packet is Opus, not Vorbis.
std::string oggOpusHead() {
    std::string page{"OggS", 4};
    page += std::string{"\x00\x02", 2};          // version, first page
    page += std::string(8, '\0');                 // granule position
    page += std::string{"\x01\x00\x00\x00", 4};  // serial
    page += std::string(4, '\0');                 // sequence
    page += std::string(4, '\0');                 // checksum
    page += std::string{"\x01\x13", 2};          // one segment of 19 bytes
    page += std::string{"OpusHead\x01\x01\x38\x01\x80\xbb\x00\x00\x00\x00\x00", 19};
    return page;
}

} // namespace

TEST_CASE("AU-2 an Ogg Vorbis asset plays, seeks and loops on its own loop point", "[audio][au2][au2-ogg]") {
    // Under the default threshold the 1 s fixture is decoded whole, once.
    const auto one = runLoop(loopingVoice("project://tone.ogg", 0.5, 1.0), 0.0);
    REQUIRE(one.decodes == 1);
    REQUIRE(one.streamed == 0);
    requireNativeLoop(one);
    // What was mixed is the fixture's sine, from 0.5 s on ...
    const std::vector<float> first(one.mixed.begin(), one.mixed.begin() + frameSamples * one.channels);
    REQUIRE(sineError(first, one.channels, 0.5) < 0.03);
    // ... and it runs on across the loop point without a step: 440 Hz at 0.5
    // moves at most 0.029 a sample, and the encoding is within 0.012.
    REQUIRE(largestStep(one) < 0.06);
    REQUIRE(peak(one.mixed) > 0.45);
    // The decomposed form follows the same source time, sought at each loop.
    requireSameSourceTime(one, runLoop(followingVoice("project://tone.ogg", 0.5, 1.0), 0.0));
    requireFollowSeek("project://tone.ogg", 0.0, 0.03);

    // GameContext::playSound plays an Ogg asset as it does a WAV one.
    VoiceRig rig;
    copyOggFixture(rig.sandbox.root / "tone.ogg");
    const auto prepared = rig.audio().prepareSound("project://tone.ogg");
    REQUIRE(std::abs(Audio::lengthSeconds(prepared) - 1.0) < 1e-9);
    const auto sound = GameContext{}.audio().playSound("project://tone.ogg");
    REQUIRE(rig.audio().decodeCountForTesting() == 1);
    REQUIRE(sineError(rig.mix(), rig.audio().outputChannels(), 0.0) < 0.03);
    GameContext{}.audio().stopSound(sound);
}

TEST_CASE("AU-2 a sound longer than the streaming threshold is read by a decoder per voice, never decoded whole",
          "[audio][au2][au2-stream]") {
    // The threshold is lowered to 0.25 s so the 2 s WAV and the 1 s Ogg stream.
    constexpr double threshold = 0.25;
    for (const std::string asset : {"project://tone.wav", "project://tone.ogg"}) {
        CAPTURE(asset);
        const auto decoded = runLoop(loopingVoice(asset, 0.5, 1.0), 0.0);
        REQUIRE(decoded.decodes == 1);
        REQUIRE(decoded.streamed == 0);
        const auto streamed = runLoop(loopingVoice(asset, 0.5, 1.0), threshold);
        REQUIRE(streamed.decodes == 0);
        REQUIRE(streamed.streamed == 1);
        // The integrated form keeps its sample-accurate loop point ...
        requireNativeLoop(streamed);
        // ... and streaming changes where the samples come from, not what they are.
        REQUIRE(streamed.mixed.size() == decoded.mixed.size());
        double apart = 0.0;
        for (std::size_t n = 0; n < streamed.mixed.size(); ++n)
            apart = (std::max)(apart, static_cast<double>(std::abs(streamed.mixed[n] - decoded.mixed[n])));
        REQUIRE(apart < 1e-5);
        // The decomposed form follows it and seeks the streamed voice.
        const auto split = runLoop(followingVoice(asset, 0.5, 1.0), threshold);
        REQUIRE(split.decodes == 0);
        REQUIRE(split.streamed == 1);
        requireSameSourceTime(streamed, split);
        requireFollowSeek(asset, threshold, asset.ends_with(".ogg") ? 0.03 : 1e-3);
    }
    // A streamed voice is sought to the frame, as a decoded one is: the ramp's
    // samples say where each voice went.
    requireFollowSeek("project://ramp.wav", 0.0, 1e-4);
    requireFollowSeek("project://ramp.wav", threshold, 1e-4);

    // The rendering config's check reads the header only, and every voice
    // opens its own decoder over the one copy of the encoded bytes.
    VoiceRig rig;
    rig.audio().setStreamingThresholdForTesting(threshold);
    writeSineWav(rig.sandbox.root / "tone.wav", 2.0);
    const auto prepared = rig.audio().prepareSound("project://tone.wav");
    REQUIRE(std::abs(Audio::lengthSeconds(prepared) - 2.0) < 1e-9);
    REQUIRE(rig.audio().decodeCountForTesting() == 0);
    const auto first = GameContext{}.audio().playSound("project://tone.wav");
    (void)rig.mix();
    const auto second = GameContext{}.audio().playSound("project://tone.wav");
    REQUIRE(rig.audio().streamedVoiceCountForTesting() == 2);
    REQUIRE(rig.audio().decodeCountForTesting() == 0);
    // Two cursors of their own: the first has read a frame (and what the mixer
    // reads ahead), the second nothing yet.
    REQUIRE(rig.audio().soundState(first)->position_seconds.value() >= frameSeconds - 1e-9);
    REQUIRE(rig.audio().soundState(second)->position_seconds.value() == 0.0);
    GameContext{}.audio().stopSound(first);
    GameContext{}.audio().stopSound(second);
    // Under the threshold, the same asset is decoded whole again.
    rig.audio().setStreamingThresholdForTesting(10.0);
    (void)rig.audio().prepareSound("project://tone.wav");
    REQUIRE(rig.audio().decodeCountForTesting() == 1);
}

TEST_CASE("AU-2 an asset whose extension or first bytes are not WAV or Ogg Vorbis is refused by name",
          "[audio][au2][au2-format]") {
    ensureLogger();
    Sandbox sandbox;
    writeTinyPcmWav(sandbox.root / "tone.wav");
    std::filesystem::copy_file(sandbox.root / "tone.wav", sandbox.root / "upper.WAV");
    std::filesystem::copy_file(sandbox.root / "tone.wav", sandbox.root / "wav_bytes.ogg");
    std::filesystem::copy_file(sandbox.root / "tone.wav", sandbox.root / "tone.mp3");
    std::filesystem::copy_file(sandbox.root / "tone.wav", sandbox.root / "no_extension");
    copyOggFixture(sandbox.root / "tone.ogg");
    copyOggFixture(sandbox.root / "ogg_bytes.wav");
    writeText(sandbox.root / "opus.ogg", oggOpusHead());

    FastModuleContainer modules;
    configureNullAudioProject(sandbox);
    auto &audio = GET_MODULE(Audio);
    const auto refused = [&](const std::string &asset, std::string_view extension, std::string_view signature) {
        CAPTURE(asset);
        try {
            (void)audio.prepareSound(asset);
            FAIL("accepted: " << asset);
        } catch (const GraphRegistryError &error) {
            REQUIRE(error.code() == "audio.format_unsupported@1");
            REQUIRE(error.detail().at("asset") == asset);
            REQUIRE(error.detail().at("extension") == std::string{extension});
            REQUIRE(error.detail().at("signature") == std::string{signature});
        }
    };
    // The name and the bytes disagree.
    refused("project://wav_bytes.ogg", ".ogg", "wav");
    refused("project://ogg_bytes.wav", ".wav", "ogg");
    // A format this build does not play, by name or by content.
    refused("project://tone.mp3", ".mp3", "wav");
    refused("project://no_extension", "", "wav");
    refused("project://opus.ogg", ".ogg", "unknown");
    REQUIRE(audio.decodeCountForTesting() == 0);
    // The same refusal from game code.
    REQUIRE_THROWS_WITH(GameContext{}.audio().playSound("project://ogg_bytes.wav"),
                        Catch::Matchers::ContainsSubstring("audio.format_unsupported@1"));
    // The controls: both agree, whatever the extension's case.
    REQUIRE_NOTHROW(audio.prepareSound("project://tone.wav"));
    REQUIRE_NOTHROW(audio.prepareSound("project://upper.WAV"));
    REQUIRE_NOTHROW(audio.prepareSound("project://tone.ogg"));
    REQUIRE(audio.decodeCountForTesting() == 3);
}

TEST_CASE("AU-2 events read on any clock but scene_time are refused by name",
          "[audio][au2][au2-clock]") {
    const auto voice = [](nlohmann::json parameters, nlohmann::json inputs) {
        parameters["asset"] = "project://tone.wav";
        return nlohmann::json{{"id", "voice"}, {"op", "audio.playback"}, {"parameters", std::move(parameters)},
                              {"inputs", std::move(inputs)}, {"outputs", {"voice"}}};
    };
    const auto map = [](std::string clock) {
        return nlohmann::json{{"id", "map"}, {"op", "value.playback"},
            {"parameters", {{"duration", 1}}}, {"inputs", {{"elapsed", edge(std::move(clock))}}}, {"outputs", {"value"}}};
    };
    const auto mismatch = [](const nlohmann::json &document) {
        try {
            const ValueGraph::Graph graph{document.dump()};
            FAIL("accepted: " << document.dump());
        } catch (const GraphRegistryError &error) {
            REQUIRE(error.code() == "audio.clock_mismatch@1");
            REQUIRE(error.detail().at("node") == "voice");
            REQUIRE(error.detail().at("events_clock") == nlohmann::json::array({"scene_time"}));
            return error.detail().at("time_clock");
        }
        return nlohmann::json{};
    };
    // The integrated form, its events read on app time.
    REQUIRE(mismatch(graphDocument({inputNode("now", "time"), eventsNode("cue"),
        voice({{"wrap", "repeat"}}, {{"time", edge("now")}, {"events", edge("cue", "events")}})})) ==
        nlohmann::json::array({"time"}));
    // The decomposed form with events, everything on app time: one clock, but
    // not the one the events are stamped on.
    REQUIRE(mismatch(graphDocument({inputNode("now", "time"), eventsNode("cue"), map("now"),
        voice({}, {{"time", edge("now")}, {"events", edge("cue", "events")}, {"position", edge("map")}})})) ==
        nlohmann::json::array({"time"}));
    // A time that reads no clock at all.
    REQUIRE(mismatch(graphDocument({{{"id", "fixed"}, {"op", "value.constant"}, {"parameters", {{"value", 1.0}}},
                                     {"outputs", {"value"}}}, eventsNode("cue"),
        voice({}, {{"time", edge("fixed")}, {"events", edge("cue", "events")}})})) == nlohmann::json::array());
    // The controls: events on scene_time, or app time with no events.
    REQUIRE_NOTHROW(ValueGraph::Graph{graphDocument({inputNode("scene", "scene_time"), eventsNode("cue"),
        voice({{"wrap", "repeat"}}, {{"time", edge("scene")}, {"events", edge("cue", "events")}})}).dump()});
    REQUIRE_NOTHROW(ValueGraph::Graph{graphDocument({inputNode("now", "time"), map("now"),
        voice({{"bus", "bgm"}}, {{"time", edge("now")}, {"position", edge("map")}})}).dump()});
}

// AU-3: an audio.playback placed with only its `time` plays. With neither
// events nor a position the voice starts on its owner's first frame -- the
// decomposed form's rule -- and the integrated map's elapsed time counts from
// there, on whichever clock `time` reads.
TEST_CASE("AU-3 an integrated voice with neither events nor a position starts on its owner's first frame",
          "[audio][au3][au3-default-start]") {
    const std::string clock = GENERATE(as<std::string>{}, "scene_time", "time");
    CAPTURE(clock);
    VoiceRig rig;
    writeGeneratedWav(rig.sandbox.root / "ramp.wav", 2.0, ramp);
    rig.load(graphDocument({inputNode("now", clock),
        {{"id", "voice"}, {"op", "audio.playback"}, {"parameters", {{"asset", "project://ramp.wav"}}},
         {"inputs", {{"time", edge("now")}}}, {"outputs", {"voice"}}}}));
    REQUIRE(rig.audio().backendKind() == AudioBackendKind::offline);
    // The owner's first frame comes half a second in: the voice starts there,
    // at the source's beginning, not half a second into it.
    for (int i = 0; i < 30; ++i) rig.time().advance();
    (void)rig.publish();
    const auto voice = rig.voice("voice");
    const auto [offset, error] = rampOffset(rig.mix(), rig.audio().outputChannels(), 0.0, 2);
    CAPTURE(offset, error);
    REQUIRE(error < 1e-4);
    rig.time().advance();
    // From there the offline engine's cursor runs with the graph's time.
    for (int i = 1; i <= 30; ++i) {
        (void)rig.publish();
        CAPTURE(i, rig.cursor("voice"));
        REQUIRE(std::abs(rig.cursor("voice") - i * frameSeconds) <= followTolerance);
        (void)rig.mix();
        rig.time().advance();
    }
    REQUIRE(rig.voice("voice") == voice);
    REQUIRE(rig.audio().seekCountForTesting(voice) == 0);
    REQUIRE(peak(rig.mixed) > 0.4);
}

TEST_CASE("AU-3 without events a voice's time may read any clock; with them, scene_time alone",
          "[audio][au3][au3-clock]") {
    const auto voice = [](nlohmann::json inputs) {
        return nlohmann::json{{"id", "voice"}, {"op", "audio.playback"},
                              {"parameters", {{"asset", "project://tone.wav"}, {"wrap", "repeat"}}},
                              {"inputs", std::move(inputs)}, {"outputs", {"voice"}}};
    };
    // No events: app time, scene time, or a time that reads no clock at all.
    REQUIRE_NOTHROW(ValueGraph::Graph{graphDocument({inputNode("now", "time"),
        voice({{"time", edge("now")}})}).dump()});
    REQUIRE_NOTHROW(ValueGraph::Graph{graphDocument({inputNode("now", "scene_time"),
        voice({{"time", edge("now")}})}).dump()});
    REQUIRE_NOTHROW(ValueGraph::Graph{graphDocument({{{"id", "fixed"}, {"op", "value.constant"},
        {"parameters", {{"value", 1.0}}}, {"outputs", {"value"}}}, voice({{"time", edge("fixed")}})}).dump()});
    // The lowered schema says the same.
    REQUIRE_NOTHROW(ValueGraph::Graph{nlohmann::json{{"schema", "pelican.value_graph"}, {"version", 1},
        {"nodes", nlohmann::json::array({{{"id", "now"}, {"op", "input"}, {"name", "time"}},
            {{"id", "voice"}, {"op", "audio_playback"}, {"time", "now"}, {"asset", "project://tone.wav"}}})},
        {"outputs", nlohmann::json::object()}, {"bindings", nlohmann::json::array()}}.dump()});
    // Events read on app time are still audio.clock_mismatch@1 (AU-2).
    try {
        const ValueGraph::Graph graph{graphDocument({inputNode("now", "time"), eventsNode("cue"),
            voice({{"time", edge("now")}, {"events", edge("cue", "events")}})}).dump()};
        FAIL("events read on app time were accepted");
    } catch (const GraphRegistryError &error) {
        REQUIRE(error.code() == "audio.clock_mismatch@1");
        REQUIRE(error.detail().at("node") == "voice");
        REQUIRE(error.detail().at("events_clock") == nlohmann::json::array({"scene_time"}));
        REQUIRE(error.detail().at("time_clock") == nlohmann::json::array({"time"}));
    }
}

TEST_CASE("AU-3 the backend a launch names is the one Audio opens", "[audio][au3][au3-backend]") {
    for (const auto kind : {AudioBackendKind::device, AudioBackendKind::offline, AudioBackendKind::null})
        REQUIRE(audioBackendKindFromName(audioBackendKindName(kind)) == kind);
    // Names are exact.
    REQUIRE_FALSE(audioBackendKindFromName("Null"));
    REQUIRE_FALSE(audioBackendKindFromName(""));
    REQUIRE_FALSE(audioBackendKindFromName("speakers"));
    ensureLogger();
    {
        // A named null opens nothing, so it has no fallback to report.
        FastModuleContainer modules;
        GET_MODULE(EngineLaunchConfig).headless = true;
        GET_MODULE(EngineLaunchConfig).audio_backend = AudioBackendKind::null;
        const auto &audio = GET_MODULE(Audio);
        REQUIRE(audio.backendKind() == AudioBackendKind::null);
        REQUIRE(audio.backendFallbackReason().empty());
    }
    {
        // A window would open the device; the launch named the offline engine.
        FastModuleContainer modules;
        GET_MODULE(EngineLaunchConfig).headless = false;
        GET_MODULE(EngineLaunchConfig).rpc = true;
        GET_MODULE(EngineLaunchConfig).audio_backend = AudioBackendKind::offline;
        const auto &audio = GET_MODULE(Audio);
        REQUIRE(audio.backendKind() == AudioBackendKind::offline);
        REQUIRE(audio.backendFallbackReason().empty());
    }
}

// PM-1a acceptance 6. Held, the device-less engine mixes silence and moves no
// voice on; released, the voice goes on from exactly where it was held.
TEST_CASE("PM-1a the whole mix held mixes silence and moves no voice on", "[audio][pm1a]") {
    VoiceRig rig;
    writeConstantWav(rig.sandbox.root / "level.wav", 2.0, 0.5);
    auto &audio = rig.audio();
    REQUIRE(audio.backendKind() == AudioBackendKind::offline);
    const auto voice = audio.playSound("project://level.wav");
    const auto position = [&] {
        const auto state = audio.soundState(voice);
        REQUIRE(state);
        REQUIRE(state->position_seconds);
        return *state->position_seconds;
    };

    REQUIRE(peak(rig.mix()) > 0.1);
    const auto playing_at = position();
    REQUIRE(playing_at > 0.0);

    REQUIRE_FALSE(audio.allPaused());
    audio.setAllPaused(true);
    REQUIRE(audio.allPaused());
    for (int frame = 0; frame < 3; ++frame) REQUIRE(peak(rig.mix()) == 0.0);
    REQUIRE(position() == playing_at);
    REQUIRE(audio.isPlaying(voice));

    audio.setAllPaused(false);
    REQUIRE_FALSE(audio.allPaused());
    REQUIRE(peak(rig.mix()) > 0.1);
    REQUIRE(position() > playing_at);
}

// PM-2. Stopping a play session stops every voice at once: the device-less
// engine mixes silence after it, both registries are empty, and a handle held
// from before names nothing any more. The hold on the whole mix is not part
// of it.
TEST_CASE("PM-2 stopAll stops every voice and forgets its handle", "[audio][pm2]") {
    VoiceRig rig;
    writeConstantWav(rig.sandbox.root / "level.wav", 2.0, 0.5);
    auto &audio = rig.audio();
    REQUIRE(audio.backendKind() == AudioBackendKind::offline);
    const auto first = audio.playSound("project://level.wav");
    const auto second = audio.playSound("project://level.wav", AudioBus::Bgm);
    REQUIRE(peak(rig.mix()) > 0.1);
    REQUIRE(audio.voiceCountForTesting() == 2U);
    REQUIRE(audio.backendVoiceCountForTesting() == 2U);
    audio.setAllPaused(true);

    audio.stopAll();
    REQUIRE(audio.voiceCountForTesting() == 0U);
    REQUIRE(audio.backendVoiceCountForTesting() == 0U);
    REQUIRE_FALSE(audio.isPlaying(first));
    REQUIRE_FALSE(audio.soundState(second));
    audio.stopSound(first);
    REQUIRE(audio.allPaused());
    audio.setAllPaused(false);
    REQUIRE(peak(rig.mix()) == 0.0);
    // The engine plays again afterwards.
    (void)audio.playSound("project://level.wav");
    REQUIRE(peak(rig.mix()) > 0.1);
}

// ---------------------------------------------------------------------------
// AT-1: the audio clock (decision 1), the device-less engine mixed with the
// game steps (decision 2), audio_time (decision 3), the master voice
// (decisions 4 and 5) and what get_status.audio says of them (decision 6).

namespace {

// A headless project with one scene, loaded, so the production frame --
// EngineTime::advance and updateFrameState -- runs as a headless player's.
void loadHeadlessStage(VoiceRig &rig) {
    writeText(rig.sandbox.root / "passes/main_rendering_config.json",
              R"({"schema":"pelican.graph","version":1,"definitions":{},"nodes":[],"outputs":{},"rendering":{}})");
    std::ofstream{rig.sandbox.root / "scene.json", std::ios::binary} << R"({"schema":"pelican.scene","version":2,"scenes":{
      "first":{"objects":[{"name":"Stage","components":[{"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}]}]}}})";
    GET_MODULE(ProjectSource).setProjectData(nlohmann::json{{"schema", "pelican.project"}, {"version", 1},
        {"name", "at1-headless"}, {"engine_min_version", "0.1.0"},
        {"basic_config", {{"window_size", {{"width", 64}, {"height", 64}}},
            {"camera", {{"yfov", 0.5}, {"znear", 0.1}, {"zfar", 100.0}}},
            {"default_scene_id", "first"}, {"scene_data_json", "scene.json"}}}}.dump());
    GET_MODULE(ECSPredefinedRegistration).reg();
    test_support::loadScene(rig.scene(), SceneId{"first"});
}

nlohmann::json masterVoice(std::string id, std::string clock, nlohmann::json parameters = nlohmann::json::object()) {
    parameters["asset"] = "project://tone.wav";
    parameters["master"] = true;
    return {{"id", std::move(id)}, {"op", "audio.playback"}, {"parameters", std::move(parameters)},
            {"inputs", {{"time", edge(std::move(clock))}}}, {"outputs", {"voice"}}};
}

} // namespace

// A1. Decision 2: a headless run's device-less engine is mixed with every game
// step, so a voice goes on with the steps -- before AT-1 nothing turned its
// crank and the voice stood at 0. Fixed step 1/60: 120 steps are 96000
// frames and two seconds of the audio clock, exactly. A5 rides on the same
// run: a value.curve reading audio_time gets the frame's audio clock.
TEST_CASE("AT-1 a headless run mixes the device-less engine with every game step: 120 steps are 96000 frames",
          "[audio][at1][at1-offline]") {
    VoiceRig rig;
    loadHeadlessStage(rig);
    writeSineWav(rig.sandbox.root / "tone.wav", 4.0);
    REQUIRE(rig.audio().backendKind() == AudioBackendKind::offline);
    // A graph voice on scene time (started by its owner's first frame) and a
    // curve on the audio clock, published the way the renderer publishes.
    rig.load(graphDocument({inputNode("scene", "scene_time"), inputNode("heard", "audio_time"),
        {{"id", "voice"}, {"op", "audio.playback"},
         {"parameters", {{"asset", "project://tone.wav"}, {"wrap", "repeat"}}},
         {"inputs", {{"time", edge("scene")}}}, {"outputs", {"voice"}}},
        {{"id", "beat"}, {"op", "value.curve"},
         {"parameters", {{"keys", {{{"time", 0.0}, {"value", 0.0}}, {{"time", 4.0}, {"value", 8.0}}}}}},
         {"inputs", {{"time", edge("heard")}}}, {"outputs", {"value"}}}}));
    // And a voice game code started before the first step.
    const auto sound = GameContext{}.audio().playSound("project://tone.wav");
    ValueGraph::PublishResult published;
    for (int step = 1; step <= 120; ++step) {
        rig.time().advance();
        updateFrameState();
        published = rig.publish();
        REQUIRE(rig.audio().mixedFramesForTesting() == static_cast<std::uint64_t>(step) * frameSamples);
    }
    REQUIRE(rig.audio().mixedFramesForTesting() == 96000u);
    const auto clock = rig.audio().clock(rig.time().now());
    REQUIRE(clock.source == Audio::ClockSource::offline);
    REQUIRE(clock.audio_time == 2.0);
    REQUIRE(clock.played_frames == 96000u);
    // The voice game code started went on two seconds into its source. (A
    // cursor stands up to one 512-frame block past what was mixed: a sound
    // reads its source a block at a time.)
    constexpr double block = 512.0 / voiceRate;
    const auto state = rig.audio().soundState(sound);
    REQUIRE(state);
    REQUIRE(state->position_seconds);
    CAPTURE(*state->position_seconds);
    REQUIRE(*state->position_seconds >= 2.0 - 1e-9);
    REQUIRE(*state->position_seconds < 2.0 + block);
    // The graph's voice played on with its own time, never sought.
    const auto voice = rig.voice("voice");
    CAPTURE(rig.cursor("voice"));
    REQUIRE(rig.cursor("voice") >= 119.0 * frameSeconds - 1e-9);
    REQUIRE(rig.cursor("voice") < 119.0 * frameSeconds + block);
    REQUIRE(rig.audio().seekCountForTesting(voice) == 0);
    // A5: the curve read the frame's audio clock, 2.0, not the game clock's
    // sum of 120 steps (a hair under 2.0).
    REQUIRE(rig.value(published, "heard") == 2.0);
    REQUIRE(rig.value(published, "beat") == Catch::Approx(4.0).margin(1e-12));
    // A6: get_status.audio says where the clock came from.
    const auto status = internal::audioStatusJson();
    INFO(status.dump());
    REQUIRE(status.at("backend") == "offline");
    REQUIRE(status.at("clock") == nlohmann::json{{"source", "offline"}, {"sample_rate", 48000},
                                                 {"audio_time", 2.0}, {"played_frames", 96000},
                                                 {"output_latency_seconds", 0.0}, {"latency_source", "none"},
                                                 {"drift_seconds", 0.0}});
    REQUIRE(status.at("master").is_null());

    // Held (PM-1a): a paused game's steps mix nothing and move no voice; the
    // clock stands and goes on from there once the game plays again.
    rig.time().setPlayState(EngineTime::PlayState::paused);
    rig.time().requestSteps(3);
    for (int step = 0; step < 3; ++step) {
        rig.time().advance();
        updateFrameState();
    }
    REQUIRE(rig.audio().mixedFramesForTesting() == 96000u);
    REQUIRE(*rig.audio().soundState(sound)->position_seconds == *state->position_seconds);
    rig.time().setPlayState(EngineTime::PlayState::playing);
    rig.time().advance();
    updateFrameState();
    REQUIRE(rig.audio().mixedFramesForTesting() == 96000u + frameSamples);
    // A set_time moves the game clock, not the audio clock: the mix carries
    // on from what it mixed, one step's worth.
    rig.time().setTime(rig.time().now() + 10.0);
    rig.time().advance();
    updateFrameState();
    REQUIRE(rig.audio().mixedFramesForTesting() == 96000u + 2 * frameSamples);
    GameContext{}.audio().stopSound(sound);
}

// A2. Decision 1: the device clock's line through what a device's audio
// thread would stamp. The device runs 0.01 % fast and mixes 480-frame (10 ms)
// chunks; each stamp is up to 1.5 ms early or late, and now and then a
// callback runs 6 ms late. The game thread reads the clock at 60 frame heads a
// second, off the chunk boundaries. The raw count steps by a chunk at a time;
// read as it is, the clock would be up to 10 ms off.
TEST_CASE("AT-1 the device clock's line takes out the chunk steps: monotonic and within 1 ms of the device",
          "[audio][at1][at1-fit]") {
    constexpr double nominal = 48000.0;
    constexpr double true_rate = nominal * 1.0001;
    constexpr double chunk = 480.0;
    constexpr double latency = 1440.0;
    constexpr double origin = 1000.0; // steady seconds at which the device stood at frame 0
    AudioDeviceClock clock{nominal, latency};
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    const auto uniform = [&] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<double>(state >> 11) / 9007199254740992.0;
    };
    // Chunk k is stamped when the device has taken in its frames, give or
    // take the callback's jitter.
    std::vector<std::pair<double, std::uint64_t>> stamps;
    for (std::uint64_t k = 0; k < 1100; ++k) {
        const auto frames = static_cast<std::uint64_t>(chunk) * (k + 1);
        auto at = origin + static_cast<double>(frames) / true_rate + (uniform() - 0.5) * 3e-3;
        if (k % 97 == 50) at += 6e-3;
        stamps.emplace_back(at, frames);
    }
    const auto truth = [&](double seconds) { return ((seconds - origin) * true_rate - latency) / nominal; };
    std::size_t next = 0;
    double last = 0.0;
    double worst = 0.0;
    std::size_t checked = 0;
    for (int frame = 1; frame <= 600; ++frame) {
        const double head = origin + frame / 60.0 + 0.0037;
        while (next < stamps.size() && stamps[next].first <= head) {
            clock.add(stamps[next].first, stamps[next].second);
            ++next;
        }
        const auto reading = clock.at(head);
        CAPTURE(frame, reading.audio_time, truth(head));
        REQUIRE(reading.running);
        REQUIRE(reading.audio_time >= last);
        last = reading.audio_time;
        // The first tenth of a second: a few chunks to stand the line on.
        if (head - origin < 0.1) continue;
        worst = (std::max)(worst, std::abs(reading.audio_time - truth(head)));
        ++checked;
    }
    CAPTURE(worst);
    REQUIRE(checked > 500);
    REQUIRE(worst < 1e-3);
    // The drift is the device's 100 ppm since the line was long enough to
    // measure from (a second in), to within what the jitter leaves of two
    // readings.
    const auto end = origin + 600 / 60.0 + 0.0037;
    const auto reading = clock.at(end);
    CAPTURE(reading.drift_seconds);
    CHECK(std::abs(reading.drift_seconds - 1e-4 * (end - origin - 1.0)) < 5e-4);
    // Held (the device stopped), the clock stands; started again, the next
    // stamps carry it on from there without going back.
    clock.hold(true);
    CHECK(clock.at(end + 1.0).audio_time == reading.audio_time);
    clock.hold(false);
    // The device starts again two seconds later from the frames it stopped at.
    const auto resumed = end + 2.0;
    const auto stopped_at = stamps[next - 1].second;
    for (int k = 1; k <= 30; ++k)
        clock.add(resumed + k * chunk / true_rate, stopped_at + static_cast<std::uint64_t>(chunk) * k);
    const auto after = clock.at(resumed + 30 * chunk / true_rate + 0.004);
    CAPTURE(after.audio_time, reading.audio_time);
    // On from where it stood -- the 30 chunks and the 4 ms since -- not from
    // where the two seconds of standing would have taken it. (The reading it
    // stood at ran up to a chunk past the device's last stamp.)
    CHECK(after.audio_time >= reading.audio_time);
    CHECK(after.audio_time - reading.audio_time ==
          Catch::Approx((30 * chunk + 0.004 * true_rate) / nominal).margin(0.012));
    // Nor does the stand count as drift.
    CHECK(after.drift_seconds == reading.drift_seconds);
}

// A3. Decision 5, against an audio clock the test moves and ticks the test
// hands EngineTime: while a master voice plays, a realtime step is the audio
// clock's move since the last tick; with no master, before the frame after
// it starts, and once it stops, the steady clock's (the ticks).
TEST_CASE("AT-1 a realtime step follows the audio clock while a master voice plays, the steady clock otherwise",
          "[audio][at1][at1-follow]") {
    ensureLogger();
    FastModuleContainer modules;
    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::realtime, frameSeconds);
    struct FakeAudio {
        double audio_time = 0.0;
        bool master = false;
        bool has_clock = true;
    } fake;
    const EngineTime::AudioClockHook hook = [](void *context, std::chrono::steady_clock::time_point)
        -> std::optional<EngineTime::AudioClockSample> {
        const auto &self = *static_cast<const FakeAudio *>(context);
        if (!self.has_clock) return std::nullopt;
        return EngineTime::AudioClockSample{self.audio_time, self.master};
    };
    REQUIRE(time.installAudioClock(&fake, hook));
    FakeAudio other;
    REQUIRE_FALSE(time.installAudioClock(&other, hook)); // one clock at a time
    using namespace std::chrono;
    const auto base = steady_clock::now();
    const auto tick = [&](int k) { return base + duration_cast<steady_clock::duration>(duration<double>(k / 60.0)); };
    const auto steady = [&](int k) { return duration<double>(tick(k) - tick(k - 1)).count(); };
    // The audio clock runs 2 % fast against the ticks.
    constexpr double audio_step = 1.02 / 60.0;
    time.advanceAt(tick(0));
    double game_time = time.now();
    for (int k = 1; k <= 12; ++k) {
        const auto before = fake.audio_time;
        fake.audio_time += audio_step;
        // The graph starts the master in frame 3; it plays through 8.
        fake.master = k >= 4 && k <= 8;
        time.advanceAt(tick(k));
        CAPTURE(k, time.dt(), steady(k));
        if (fake.master) {
            CHECK(time.followingAudioClock());
            CHECK(time.dt() == fake.audio_time - before);
        } else {
            CHECK_FALSE(time.followingAudioClock());
            CHECK(time.dt() == Catch::Approx(steady(k)).margin(1e-12));
        }
        // The editor clock is the steady clock's all along.
        CHECK(time.editorDt() == Catch::Approx(steady(k)).margin(1e-12));
        game_time += time.dt();
    }
    CHECK(time.now() == Catch::Approx(game_time).margin(1e-12));
    // Bounded as a steady step is.
    fake.master = true;
    fake.audio_time += 5.0;
    time.advanceAt(tick(13));
    CHECK(time.followingAudioClock());
    CHECK(time.dt() == Catch::Approx(0.1));
    // A clock that answers nothing (offline, null) is not followed.
    fake.has_clock = false;
    time.advanceAt(tick(14));
    CHECK_FALSE(time.followingAudioClock());
    CHECK(time.dt() == Catch::Approx(steady(14)).margin(1e-12));
    fake.has_clock = true;
    // Paused, the game clock stands whatever the audio clock does.
    time.setPlayState(EngineTime::PlayState::paused);
    fake.audio_time += audio_step;
    time.advance();
    CHECK(time.dt() == 0.0);
    CHECK_FALSE(time.followingAudioClock());
    // The fixed-step mode never follows: there the audio follows the game.
    time.setPlayState(EngineTime::PlayState::playing);
    time.setup(EngineTime::Mode::fixed_step, frameSeconds);
    for (int k = 0; k < 3; ++k) {
        fake.audio_time += audio_step;
        time.advance();
        CHECK(time.dt() == frameSeconds);
        CHECK_FALSE(time.followingAudioClock());
    }
    time.removeAudioClock(&fake);
    REQUIRE(time.installAudioClock(&other, hook));
    time.removeAudioClock(&other);
}

// A4. Decision 4: one master at a time, counted the way the frames run the
// graph -- an owner lives only in its own scene, so masters owned in two
// scenes never play together; an unowned one plays in every scene.
TEST_CASE("AT-1 master voices that could play at once are refused by name, counted per scene",
          "[audio][at1][at1-master-conflict]") {
    const auto refused = [](const nlohmann::json &document) {
        try {
            const ValueGraph::Graph graph{document.dump()};
            FAIL("accepted: " << document.dump());
        } catch (const GraphRegistryError &error) {
            REQUIRE(error.code() == "audio.master_conflict@1");
            return error.detail().at("nodes");
        }
        return nlohmann::json{};
    };
    const auto voice = [](std::string id) {
        return nlohmann::json{{"id", std::move(id)}, {"op", "audio.playback"},
                              {"parameters", {{"asset", "project://tone.wav"}}},
                              {"inputs", {{"time", edge("now")}}}, {"outputs", {"voice"}}};
    };
    // Two in one document, neither owned.
    REQUIRE(refused(graphDocument({inputNode("now", "time"), masterVoice("music", "now"),
                                   masterVoice("stinger", "now")})) == nlohmann::json::array({"music", "stinger"}));
    // One master beside any number of voices that are not.
    REQUIRE_NOTHROW(ValueGraph::Graph{graphDocument({inputNode("now", "time"), masterVoice("music", "now"),
                                                     voice("hit"), voice("step")}).dump()});
    // One definition with a master, one instance per scene: the per-scene BGM.
    const auto instances = [](nlohmann::json nodes, nlohmann::json top = nlohmann::json::array()) {
        nlohmann::json document{{"schema", "pelican.graph"}, {"version", 1},
            {"definitions", {{"bgm", {{"nodes", {inputNode("now", "time"), masterVoice("voice", "now")}},
                                      {"outputs", nlohmann::json::object()}}}}},
            {"nodes", std::move(top)}, {"outputs", nlohmann::json::object()}};
        for (auto &node : nodes) document["nodes"].push_back(std::move(node));
        return document;
    };
    const auto instance = [](std::string id, std::string scene, std::uint64_t object) {
        return nlohmann::json{{"id", std::move(id)}, {"op", "graph.instance"}, {"definition", "bgm"},
                              {"owner", {{"scene", std::move(scene)}, {"object", object}}}};
    };
    REQUIRE_NOTHROW(ValueGraph::Graph{instances({instance("title_bgm", "title", 1),
                                                 instance("stage_bgm", "stage", 3)}).dump()});
    // The same definition owned twice in one scene: both would play there.
    REQUIRE(refused(instances({instance("left", "stage", 1), instance("right", "stage", 2),
                               instance("title_bgm", "title", 4)})) ==
            nlohmann::json::array({"left/voice", "right/voice"}));
    // An unowned master plays in every scene, beside the one a scene owns.
    REQUIRE(refused(instances(nlohmann::json::array({instance("title_bgm", "title", 1)}),
                              nlohmann::json::array({inputNode("app", "time"), masterVoice("jingle", "app")}))) ==
            nlohmann::json::array({"jingle", "title_bgm/voice"}));
    // `master` is true or false.
    auto numbered = masterVoice("music", "now");
    numbered["parameters"]["master"] = 1;
    REQUIRE_THROWS(ValueGraph::Graph{graphDocument({inputNode("now", "time"), numbered}).dump()});
    // The catalog lists it: a bool, false unless set, and changing it rebuilds
    // the voice (which voice the game clock follows is fixed when it starts).
    const auto catalog = nlohmann::json(builtinGraphModuleRegistry().catalog());
    nlohmann::json playback;
    for (const auto &module : catalog.at("modules"))
        for (const auto &entry : module.at("operations"))
            if (entry.at("op") == "audio.playback") playback = entry;
    REQUIRE(playback.is_object());
    CHECK(playback.at("change").at("parameters").at("master") == "rebuild");
    nlohmann::json listed;
    for (const auto &parameter : playback.at("parameters"))
        if (parameter.at("name") == "master") listed = parameter;
    INFO(listed.dump());
    REQUIRE(listed.is_object());
    CHECK(listed.at("kind") == "bool");
    CHECK(listed.at("default") == false);
}

// A4. Decision 4 on the device-less engine: a master voice -- the integrated
// form on its own loop point, the only form a master takes -- is not sought
// for a gap between its cursor and the graph's time however wide (here the
// mixer runs a whole second ahead), and an explicit jump (set_time) still
// seeks it. The control: the same voice without `master` is sought for the
// same gap.
TEST_CASE("AT-1 a master voice is not sought for a second's gap in the clock, and a set_time still seeks it",
          "[audio][at1][at1-master]") {
    using Command = Animation::CurvePlaybackCommand;
    const bool master = GENERATE(true, false);
    CAPTURE(master);
    VoiceRig rig;
    writeSineWav(rig.sandbox.root / "tone.wav", 4.0);
    rig.load(graphDocument({inputNode("now", "scene_time"), eventsNode("cue"),
        {{"id", "voice"}, {"op", "audio.playback"},
         {"parameters", {{"asset", "project://tone.wav"}, {"wrap", "repeat"}, {"master", master}}},
         {"inputs", {{"time", edge("now")}, {"events", edge("cue", "events")}}}, {"outputs", {"voice"}}}}));
    rig.emit("cue", Command::start);
    for (int i = 0; i < 10; ++i) (void)rig.frame();
    const auto sound = rig.voice("voice");
    REQUIRE(rig.audio().seekCountForTesting(sound) == 0);
    REQUIRE(rig.audio().masterPlaying() == master);
    const auto masterOffset = [&] {
        const auto status = internal::audioStatusJson();
        INFO(status.dump());
        if (!master) {
            REQUIRE(status.at("master").is_null());
            return 0.0;
        }
        const auto &entry = status.at("master");
        REQUIRE(entry.at("node") == "voice");
        // Heard where it was mixed: the cursor less what the sound read
        // ahead of its mix (at most a block).
        const auto heard = entry.at("heard_position_seconds").get<double>();
        REQUIRE(heard <= rig.cursor("voice") + 1e-12);
        REQUIRE(rig.cursor("voice") - heard < 512.0 / voiceRate);
        return entry.at("offset_seconds").get<double>();
    };
    // In step: the master is heard where the graph puts it, frame after frame.
    (void)rig.publish();
    const auto in_step = masterOffset();
    CHECK(std::abs(in_step) <= 1e-9);
    (void)rig.mix();
    rig.time().advance();

    // The mixer runs a second ahead of the graph's clock.
    (void)rig.audio().mixOffline(voiceRate);
    for (int i = 0; i < 10; ++i) {
        (void)rig.publish();
        if (master) CHECK(masterOffset() == Catch::Approx(-1.0).margin(1e-9));
        (void)rig.mix();
        rig.time().advance();
    }
    REQUIRE(rig.voice("voice") == sound);
    if (!master) {
        // The control: a voice that follows the graph was brought back.
        REQUIRE(rig.audio().seekCountForTesting(sound) >= 1);
        return;
    }
    REQUIRE(rig.audio().seekCountForTesting(sound) == 0);
    // An explicit jump: set_time half a second on. The voice is sought there
    // once, and plays on from it unsought.
    rig.time().setTime(rig.time().now() + 0.5);
    (void)rig.publish();
    REQUIRE(rig.audio().seekCountForTesting(sound) == 1);
    CHECK(std::abs(masterOffset()) <= followTolerance);
    (void)rig.mix();
    rig.time().advance();
    for (int i = 0; i < 10; ++i) (void)rig.frame();
    REQUIRE(rig.audio().seekCountForTesting(sound) == 1);
    // Held by its own pause, a master is not playing (the game clock goes
    // back to the steady clock); resumed, it is again.
    rig.emit("cue", Command::pause);
    (void)rig.frame();
    CHECK_FALSE(rig.audio().masterPlaying());
    rig.emit("cue", Command::resume);
    (void)rig.frame();
    CHECK(rig.audio().masterPlaying());
    REQUIRE(rig.audio().seekCountForTesting(sound) == 1);
}

// AT-1 decision 4: a master runs on its own clock -- the integrated form on
// its own loop point, the voice AU-1 holds natively (wrap once or repeat, a
// literal map, speed > 0, no position) -- and any other voice asked to be one
// is refused by name when the graph is built, saying why.
TEST_CASE("AT-1 a master that would not run on its own loop point is refused by name, with the reason",
          "[audio][at1][at1-master-native]") {
    const auto voice = [](nlohmann::json parameters, nlohmann::json inputs) {
        parameters["asset"] = "project://tone.wav";
        parameters["master"] = true;
        return nlohmann::json{{"id", "voice"}, {"op", "audio.playback"}, {"parameters", std::move(parameters)},
                              {"inputs", std::move(inputs)}, {"outputs", {"voice"}}};
    };
    const auto map = nlohmann::json{{"id", "map"}, {"op", "value.playback"},
        {"parameters", {{"wrap", "repeat"}, {"duration", 1}}}, {"inputs", {{"elapsed", edge("now")}}},
        {"outputs", {"value"}}};
    const auto reason = [](const nlohmann::json &document) {
        try {
            const ValueGraph::Graph graph{document.dump()};
            FAIL("accepted: " << document.dump());
        } catch (const GraphRegistryError &error) {
            REQUIRE(error.code() == "audio.master_requires_native@1");
            REQUIRE(error.detail().at("node") == "voice");
            return error.detail().at("reason").get<std::string>();
        }
        return std::string{};
    };
    // The decomposed form follows a position the graph computes.
    CHECK(reason(graphDocument({inputNode("now", "time"), map,
        voice({}, {{"time", edge("now")}, {"position", edge("map")}})})) == "position");
    // A map an edge drives changes while the voice plays.
    CHECK(reason(graphDocument({inputNode("now", "time"),
        voice({{"wrap", "repeat"}}, {{"time", edge("now")}, {"speed", edge("now")}})})) == "driven_mapping");
    // Back and forth, or backwards, it cannot hold its own loop point.
    CHECK(reason(graphDocument({inputNode("now", "time"),
        voice({{"wrap", "ping_pong"}}, {{"time", edge("now")}})})) == "ping_pong");
    CHECK(reason(graphDocument({inputNode("now", "time"),
        voice({{"wrap", "repeat"}, {"speed", -1.0}}, {{"time", edge("now")}})})) == "negative_speed");
    // The controls: once or repeat, a literal range, delay and speed.
    REQUIRE_NOTHROW(ValueGraph::Graph{graphDocument({inputNode("now", "time"),
        voice({}, {{"time", edge("now")}})}).dump()});
    REQUIRE_NOTHROW(ValueGraph::Graph{graphDocument({inputNode("now", "time"),
        voice({{"wrap", "repeat"}, {"source_begin", 0.5}, {"source_end", 1.5}, {"speed", 2.0}, {"delay", 0.25}},
              {{"time", edge("now")}})}).dump()});
    // The same voices without `master` play as they did.
    auto follower = voice({}, {{"time", edge("now")}, {"position", edge("map")}});
    follower["parameters"].erase("master");
    REQUIRE_NOTHROW(ValueGraph::Graph{graphDocument({inputNode("now", "time"), map, follower}).dump()});
}

// AT-1 decision 5 after a hitch: a frame that takes 0.3 s moves the audio
// clock 0.3 s, and the step is bounded at 0.1 s. What the bound held back is
// carried into the next steps (each bounded again), so within a few frames
// the scene time a master has played through -- the game clock's progress
// since the master started, which scene_time takes as it is -- is the audio
// clock's progress again, not 0.2 s behind it for good. A frame with no
// master drops what is owed.
TEST_CASE("AT-1 after a hitch a master's steps carry what the bound held back until the game clock is the sound's again",
          "[audio][at1][at1-follow]") {
    ensureLogger();
    FastModuleContainer modules;
    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::realtime, frameSeconds);
    struct FakeAudio {
        double audio_time = 0.0;
        bool master = false;
    } fake;
    REQUIRE(time.installAudioClock(&fake, [](void *context, std::chrono::steady_clock::time_point)
        -> std::optional<EngineTime::AudioClockSample> {
        const auto &self = *static_cast<const FakeAudio *>(context);
        return EngineTime::AudioClockSample{self.audio_time, self.master};
    }));
    using namespace std::chrono;
    auto at = steady_clock::now();
    const auto frame = [&](double seconds) {
        at += duration_cast<steady_clock::duration>(duration<double>(seconds));
        fake.audio_time += seconds;
        time.advanceAt(at);
    };
    frame(frameSeconds);
    frame(frameSeconds);
    // The master starts here: the scene and the sound are measured from this tick.
    fake.master = true;
    const auto scene_from = time.now();
    const auto audio_from = fake.audio_time;
    const auto apart = [&] { return (fake.audio_time - audio_from) - (time.now() - scene_from); };
    for (int k = 0; k < 5; ++k) {
        frame(frameSeconds);
        CHECK(std::abs(apart()) < 1e-12);
    }
    // The hitch: one frame of 0.3 s. Its step is bounded; the game clock is
    // behind the sound by what the bound held back.
    frame(0.3);
    CHECK(time.followingAudioClock());
    CHECK(time.dt() == Catch::Approx(0.1));
    CHECK(apart() == Catch::Approx(0.2).margin(1e-9));
    // It catches up within a few frames, each step bounded, and then runs with
    // the sound again.
    int caught_up = 0;
    for (int k = 1; k <= 10; ++k) {
        frame(frameSeconds);
        CAPTURE(k, time.dt(), apart());
        CHECK(time.dt() <= 0.1 + 1e-12);
        if (caught_up == 0 && std::abs(apart()) < 1e-9) caught_up = k;
    }
    CAPTURE(caught_up);
    CHECK(caught_up >= 1);
    CHECK(caught_up <= 4);
    CHECK(std::abs(apart()) < 1e-9);
    // A frame with no master owes the sound nothing: a hitch held back and then
    // the master stopping leaves no carry for the next master to pay.
    frame(0.3);
    CHECK(apart() == Catch::Approx(0.2).margin(1e-9));
    fake.master = false;
    frame(frameSeconds);
    CHECK_FALSE(time.followingAudioClock());
    fake.master = true;
    frame(frameSeconds);
    CHECK(time.followingAudioClock());
    CHECK(time.dt() == Catch::Approx(frameSeconds).margin(1e-12));
    time.removeAudioClock(&fake);
}

// A5. Decision 3: audio_time is a frame input the catalog lists, and the
// clock check treats it as a clock: events still read scene_time alone.
TEST_CASE("AT-1 audio_time is a frame input, and events read on it are refused as on any clock but scene_time",
          "[audio][at1][at1-clock]") {
    const auto catalog = nlohmann::json(builtinGraphModuleRegistry().catalog());
    nlohmann::json listed;
    for (const auto &module : catalog.at("modules"))
        for (const auto &entry : module.at("operations"))
            if (entry.at("op") == "value.input")
                for (const auto &name : entry.at("input_names"))
                    if (name.at("name") == "audio_time") listed = name;
    INFO(listed.dump());
    REQUIRE(listed.is_object());
    CHECK(listed.at("type").at("semantic") == "pelican.value.scalar@1");
    CHECK_FALSE(listed.at("description").get<std::string>().empty());
    const ValueGraph::Graph reads{graphDocument(nlohmann::json::array({inputNode("heard", "audio_time")})).dump()};
    REQUIRE_NOTHROW(validateGraphInputs(reads));

    const auto voice = [](nlohmann::json inputs) {
        return nlohmann::json{{"id", "voice"}, {"op", "audio.playback"},
                              {"parameters", {{"asset", "project://tone.wav"}, {"wrap", "repeat"}}},
                              {"inputs", std::move(inputs)}, {"outputs", {"voice"}}};
    };
    const auto map = [](std::string clock) {
        return nlohmann::json{{"id", "map"}, {"op", "value.playback"}, {"parameters", {{"duration", 1}}},
                              {"inputs", {{"elapsed", edge(std::move(clock))}}}, {"outputs", {"value"}}};
    };
    const auto mismatch = [](const nlohmann::json &document) {
        try {
            const ValueGraph::Graph graph{document.dump()};
            FAIL("accepted: " << document.dump());
        } catch (const GraphRegistryError &error) {
            REQUIRE(error.code() == "audio.clock_mismatch@1");
            REQUIRE(error.detail().at("node") == "voice");
            return error.detail();
        }
        return nlohmann::json{};
    };
    // Events read on the audio clock.
    CHECK(mismatch(graphDocument({inputNode("heard", "audio_time"), eventsNode("cue"),
              voice({{"time", edge("heard")}, {"events", edge("cue", "events")}})}))
              .at("time_clock") == nlohmann::json::array({"audio_time"}));
    // Events on scene_time, the position on the audio clock.
    const auto split = mismatch(graphDocument({inputNode("scene", "scene_time"), inputNode("heard", "audio_time"),
        eventsNode("cue"), map("heard"),
        nlohmann::json{{"id", "voice"}, {"op", "audio.playback"}, {"parameters", {{"asset", "project://tone.wav"}}},
                       {"inputs", {{"time", edge("scene")}, {"events", edge("cue", "events")},
                                   {"position", edge("map")}}},
                       {"outputs", {"voice"}}}}));
    CHECK(split.at("position_clock") == nlohmann::json::array({"audio_time"}));
    // Without events a voice's time or position may read it.
    REQUIRE_NOTHROW(ValueGraph::Graph{graphDocument({inputNode("heard", "audio_time"),
        voice({{"time", edge("heard")}})}).dump()});
    REQUIRE_NOTHROW(ValueGraph::Graph{graphDocument({inputNode("heard", "audio_time"), map("heard"),
        nlohmann::json{{"id", "voice"}, {"op", "audio.playback"}, {"parameters", {{"asset", "project://tone.wav"}}},
                       {"inputs", {{"time", edge("heard")}, {"position", edge("map")}}},
                       {"outputs", {"voice"}}}}).dump()});
}

// A6. Decision 6 on the null backend: the same keys, the values from the game
// clock -- audio_time is the game clock, and a master is heard where the graph
// put it.
TEST_CASE("AT-1 get_status.audio carries the clock and the master on the null backend, from the game clock",
          "[audio][at1][at1-status]") {
    VoiceRig rig;
    GET_MODULE(EngineLaunchConfig).audio_backend = AudioBackendKind::null;
    writeSineWav(rig.sandbox.root / "tone.wav", 2.0);
    rig.load(graphDocument({inputNode("now", "scene_time"), masterVoice("voice", "now")}));
    REQUIRE(rig.audio().backendKind() == AudioBackendKind::null);
    for (int i = 0; i < 10; ++i) {
        (void)rig.publish();
        rig.time().advance();
    }
    (void)rig.publish();
    const auto status = internal::audioStatusJson();
    INFO(status.dump());
    CHECK(status.at("clock") == nlohmann::json{{"source", "game_clock"}, {"sample_rate", 0},
                                               {"audio_time", rig.time().now()}, {"played_frames", 0},
                                               {"output_latency_seconds", 0.0}, {"latency_source", "none"},
                                               {"drift_seconds", 0.0}});
    const auto position = status.at("voices").at(0).at("position_seconds").get<double>();
    CHECK(position == Catch::Approx(10 * frameSeconds).margin(1e-9));
    CHECK(status.at("master") == nlohmann::json{{"node", "voice"}, {"position_seconds", position},
                                                {"heard_position_seconds", position}, {"offset_seconds", 0.0}});
    CHECK(rig.audio().masterPlaying());
}

// A7. Decision 1 on this machine's device, opened from the test with no
// window and no player: silence for about a second, read the way a frame's
// head reads it. The clock runs with real time (1 s in 1 +- 0.01 s), never
// goes back, and the latency estimate is above 0. With a silent master voice,
// EngineTime follows it. Skipped, with the reason, where no device opens.
TEST_CASE("AT-1 the device's own clock runs with real time, never back, behind an estimated latency",
          "[audio][at1][at1-device]") {
    using SteadyClock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    ensureLogger();
    Sandbox sandbox;
    writeConstantWav(sandbox.root / "silence.wav", 3.0, 0.0);
    FastModuleContainer modules;
    GET_MODULE(EngineLaunchConfig).headless = false;
    GET_MODULE(EngineLaunchConfig).rpc = true;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    auto &audio = GET_MODULE(Audio);
    if (audio.backendKind() == AudioBackendKind::null && !audio.backendFallbackReason().empty())
        SKIP("no audio device here: " + audio.backendFallbackReason());
    REQUIRE(audio.backendKind() == AudioBackendKind::device);
    const auto latency = audio.deviceLatency();
    REQUIRE(latency);
    REQUIRE(latency->seconds > 0.0);
    const auto read = [&](SteadyClock::time_point at) {
        const auto followed = audio.followedClockAt(at);
        REQUIRE(followed);
        return followed->audio_time;
    };
    // The device's first chunks.
    const auto opened = SteadyClock::now();
    while (read(SteadyClock::now()) == 0.0 && SteadyClock::now() - opened < 3s) std::this_thread::sleep_for(5ms);
    REQUIRE(read(SteadyClock::now()) > 0.0);
    std::this_thread::sleep_for(200ms);
    const auto first_tick = SteadyClock::now();
    const auto first = read(first_tick);
    double last = first;
    std::size_t readings = 0;
    while (SteadyClock::now() - first_tick < 1s) {
        std::this_thread::sleep_for(5ms);
        const auto now = read(SteadyClock::now());
        REQUIRE(now >= last);
        last = now;
        ++readings;
    }
    const auto end_tick = SteadyClock::now();
    const auto end = read(end_tick);
    const auto elapsed = std::chrono::duration<double>(end_tick - first_tick).count();
    const auto reading = audio.clock(0.0);
    WARN("AT-1 device clock: audio " << end - first << " s in " << elapsed << " s (" << readings
         << " readings), rate " << reading.sample_rate << " Hz, latency " << latency->seconds << " s (period "
         << latency->period_frames << " x " << latency->periods << ", buffer " << latency->buffer_frames
         << " at " << latency->device_rate << " Hz, from " << (latency->from_buffer ? "buffer" : "periods")
         << "), drift " << reading.drift_seconds << " s, audio_time " << reading.audio_time << " s");
    CHECK(reading.source == Audio::ClockSource::device);
    CHECK(reading.latency_estimated);
    CHECK(reading.output_latency_seconds == latency->seconds);
    CHECK(std::abs((end - first) - elapsed) <= 0.01 * elapsed);

    // A silent master voice: a realtime step is the audio clock's move.
    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::realtime, frameSeconds);
    Audio::VoiceStart start;
    start.master = true;
    const auto voice = audio.playVoice(audio.prepareSound("project://silence.wav"), start);
    REQUIRE(audio.masterPlaying());
    time.advance();
    const auto before = audio.clock(0.0).audio_time;
    std::this_thread::sleep_for(20ms);
    time.advance();
    const auto after = audio.clock(0.0).audio_time;
    CHECK(time.followingAudioClock());
    CHECK(time.dt() == Catch::Approx(after - before).margin(1e-12));
    CHECK(time.dt() > 0.0);
    audio.stopSound(voice);
    CHECK_FALSE(audio.masterPlaying());
    time.advance();
    CHECK_FALSE(time.followingAudioClock());
}

} // namespace Pelican
