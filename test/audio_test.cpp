#include "../src/core/audio/audio.hpp"
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/log.hpp"
#include "../src/core/userpublic/gamecontext.hpp"
#include "../src/core/userpublic/valuegraph.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

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

void configureNullAudioProject(const Sandbox &sandbox) {
    GET_MODULE(EngineLaunchConfig).headless = true;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
}

} // namespace

TEST_CASE("GameContext audio play state works with null backend", "[audio]") {
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
    REQUIRE_FALSE(state->position_seconds);
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

TEST_CASE("graph audio events acknowledge only successful deliveries", "[audio][graph-composition]") {
    const ValueGraph::Graph graph{R"({"schema":"pelican.value_graph","version":1,
      "nodes":[{"id":"events","op":"event_input"},
        {"id":"time","op":"input","name":"time"},
        {"id":"sound","op":"audio_playback","time":"time","events":"events","asset":"tone.wav"}],
      "outputs":{}})"};
    auto state = graph.createState();
    using Command = Animation::CurvePlaybackCommand;
    REQUIRE(graph.emitEvent(state, "events", Command::start, 0));
    REQUIRE(graph.emitEvent(state, "events", Command::pause, 0));
    unsigned starts = 0, pauses = 0;
    const auto first = graph.publish(GameContext{}, {{"time", 0}}, {}, {}, {}, {}, &state,
        [&](std::string_view node, Command command) {
            REQUIRE(std::string{node} == "sound");
            if (command == Command::start) { ++starts; return true; }
            return false;
        });
    REQUIRE(first.applied == 1);
    REQUIRE(first.unavailable.size() == 1);
    const auto retry = graph.publish(GameContext{}, {{"time", 0}}, {}, {}, {}, {}, &state,
        [&](std::string_view, Command command) {
            if (command == Command::start) ++starts;
            if (command == Command::pause) ++pauses;
            return true;
        });
    REQUIRE(retry.applied == 1);
    REQUIRE(retry.unavailable.empty());
    REQUIRE(starts == 1);
    REQUIRE(pauses == 1);
    REQUIRE(graph.publish(GameContext{}, {{"time", 1}}, {}, {}, {}, {}, &state).applied == 0);
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

} // namespace Pelican
