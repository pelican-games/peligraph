#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/log.hpp"
#include "../src/core/persistence/persistence.hpp"
#include "../src/core/userpublic/gamecontext.hpp"
#if PELICAN_WITH_AUDIO
#include "../src/core/audio/audio.hpp"
#endif

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
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
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file << text;
}

nlohmann::json readJson(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    REQUIRE(file.is_open());
    return nlohmann::json::parse(file);
}

struct Sandbox {
    std::filesystem::path base;
    std::filesystem::path project_root;
    std::filesystem::path user_root;

    Sandbox() {
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        base = std::filesystem::temp_directory_path() / ("pelican_persistence_" + suffix);
        project_root = base / "project";
        user_root = base / "user";
        std::filesystem::create_directories(project_root);
    }

    ~Sandbox() {
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
    }
};

void configureProject(const Sandbox &sandbox) {
    GET_MODULE(PathResolver).setup(
        sandbox.project_root, false,
        nlohmann::json{{"schema", "pelican.project"},
                       {"version", 1},
                       {"name", "persistence-test"},
                       {"engine_min_version", "0.1.0"}}
            .dump(),
        sandbox.user_root);
#if PELICAN_WITH_AUDIO
    GET_MODULE(EngineLaunchConfig).headless = true;
#endif
}

} // namespace

TEST_CASE("Persistence settings round-trip uses only user directory and ignores temp residue",
          "[persistence]") {
    ensureLogger();
    Sandbox sandbox;
    FastModuleContainer modules;
    configureProject(sandbox);

    auto &persistence = GET_MODULE(Persistence);
    REQUIRE_FALSE(persistence.loadSettings());

    GameContext context;
    const auto game = nlohmann::json::array({"chapter-2", 17, nlohmann::json{{"flag", true}}});
    context.setGameSettings(game);
    REQUIRE_FALSE(std::filesystem::exists(sandbox.user_root / "settings.json"));

#if PELICAN_WITH_AUDIO
    context.setBusVolume("master", 0.25f);
    context.setBusVolume("bgm", 0.5f);
    context.setBusVolume("se", 0.75f);
#endif
    context.saveSettings();

    const auto settings_path = sandbox.user_root / "settings.json";
    REQUIRE(std::filesystem::is_regular_file(settings_path));
    REQUIRE_FALSE(std::filesystem::exists(sandbox.project_root / "settings.json"));

    const auto saved = readJson(settings_path);
    REQUIRE(saved.at("schema") == "pelican.settings");
    REQUIRE(saved.at("version") == 1);
    REQUIRE(saved.at("game") == game);
#if PELICAN_WITH_AUDIO
    REQUIRE(saved.at("engine").at("audio").at("bus_volumes").at("master") == 0.25f);
    REQUIRE(saved.at("engine").at("audio").at("bus_volumes").at("bgm") == 0.5f);
    REQUIRE(saved.at("engine").at("audio").at("bus_volumes").at("se") == 0.75f);
#endif

    writeText(settings_path.string() + ".tmp", "interrupted write");
    persistence.resetForTesting();
    REQUIRE(persistence.loadSettings());
    REQUIRE(persistence.gameSettings() == game);
    REQUIRE(std::filesystem::is_regular_file(settings_path.string() + ".tmp"));

#if PELICAN_WITH_AUDIO
    auto &audio = GET_MODULE(Audio);
    audio.setBusVolume("master", 1.0f);
    audio.setBusVolume("bgm", 1.0f);
    audio.setBusVolume("se", 1.0f);
    persistence.applyAudioSettings(audio);
    REQUIRE(audio.busVolume("master") == Catch::Approx(0.25f));
    REQUIRE(audio.busVolume("bgm") == Catch::Approx(0.5f));
    REQUIRE(audio.busVolume("se") == Catch::Approx(0.75f));
#endif
}

TEST_CASE("Persistence keeps running with named warning state for corrupt settings", "[persistence]") {
    ensureLogger();
    Sandbox sandbox;
    FastModuleContainer modules;
    configureProject(sandbox);

    writeText(sandbox.user_root / "settings.json", "{ definitely-not-json");
    auto &persistence = GET_MODULE(Persistence);
    REQUIRE_FALSE(persistence.loadSettings());
    REQUIRE(persistence.gameSettings() == nlohmann::json::object());
    REQUIRE(persistence.lastSettingsError().has_value());
    REQUIRE_THAT(*persistence.lastSettingsError(),
                 Catch::Matchers::ContainsSubstring("settings.json"));
}

TEST_CASE("GameContext save data is atomic, listed deterministically, and rejects escapes",
          "[persistence]") {
    ensureLogger();
    Sandbox sandbox;
    FastModuleContainer modules;
    configureProject(sandbox);

    GameContext context;
    context.saveData("slot-b", nlohmann::json{{"level", 2}});
    context.saveData("slot-a", nlohmann::json{{"level", 1}});
    context.saveData("slot-a", nlohmann::json{{"level", 3}});

    REQUIRE(context.loadData("slot-a") == nlohmann::json{{"level", 3}});
    REQUIRE_FALSE(context.loadData("missing").has_value());

    const auto slots = context.listSaves();
    REQUIRE(slots.size() == 2);
    REQUIRE(slots[0].slot == "slot-a");
    REQUIRE(slots[1].slot == "slot-b");

    writeText(sandbox.user_root / "saves" / "abandoned.json.tmp", "partial");
    REQUIRE(context.listSaves().size() == 2);

    writeText(sandbox.user_root / "saves" / "corrupt.json", "not json");
    REQUIRE_NOTHROW(context.loadData("corrupt"));
    REQUIRE_FALSE(context.loadData("corrupt").has_value());

    REQUIRE_THROWS_WITH(context.saveData("../outside", nlohmann::json::object()),
                        Catch::Matchers::ContainsSubstring("invalid file-name character"));
    REQUIRE_FALSE(std::filesystem::exists(sandbox.project_root / "saves"));
    REQUIRE_FALSE(std::filesystem::exists(sandbox.base / "outside.json"));
}

} // namespace Pelican
