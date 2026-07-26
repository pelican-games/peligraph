#include "../src/core/audio/audio.hpp"
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/log.hpp"
#include "../src/core/userpublic/gamecontext.hpp"

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
    const auto first = context.playSound("project://tone.wav");
    const auto second = context.playSound("project://tone.wav");

    REQUIRE(first != second);
    REQUIRE(context.isPlaying(first));
    REQUIRE(context.isPlaying(second));
    REQUIRE(GET_MODULE(Audio).voiceCountForTesting() == 2);
    REQUIRE(GET_MODULE(Audio).backendVoiceCountForTesting() == 2);

    REQUIRE_NOTHROW(context.setBusVolume("master", 0.75f));
    REQUIRE_NOTHROW(context.setBusVolume("se", 0.5f));

    context.stopSound(first);
    REQUIRE_FALSE(context.isPlaying(first));
    REQUIRE(context.isPlaying(second));
    REQUIRE(GET_MODULE(Audio).voiceCountForTesting() == 1);
    REQUIRE(GET_MODULE(Audio).backendVoiceCountForTesting() == 1);

    context.stopSound(second);
    REQUIRE_FALSE(context.isPlaying(second));
    REQUIRE(GET_MODULE(Audio).voiceCountForTesting() == 0);
    REQUIRE(GET_MODULE(Audio).backendVoiceCountForTesting() == 0);
}

TEST_CASE("Stopped audio voices do not accumulate in either registry", "[audio]") {
    ensureLogger();
    Sandbox sandbox;
    writeTinyPcmWav(sandbox.root / "tone.wav");

    FastModuleContainer modules;
    configureNullAudioProject(sandbox);

    GameContext context;
    for (std::size_t i = 0; i < 256; ++i) {
        const auto handle = context.playSound("project://tone.wav");
        context.stopSound(handle);
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
    REQUIRE_THROWS_WITH(context.playSound("project://bad.wav"),
                        Catch::Matchers::ContainsSubstring("Invalid WAV"));
    REQUIRE_THROWS_WITH(context.playSound("project://missing.wav"),
                        Catch::Matchers::ContainsSubstring("Failed to open file"));
    REQUIRE_THROWS_WITH(context.setBusVolume("dialog", 1.0f),
                        Catch::Matchers::ContainsSubstring("Unknown audio bus"));
    REQUIRE_THROWS_WITH(context.setBusVolume("se", -0.1f),
                        Catch::Matchers::ContainsSubstring("finite non-negative"));
}

} // namespace Pelican
