#include "persistence.hpp"

#include "../loader/pathresolver.hpp"
#include "../log.hpp"
#if PELICAN_WITH_AUDIO
#include "../audio/audio.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace Pelican {

namespace {

constexpr std::string_view settings_ref = "user://settings.json";
constexpr std::string_view settings_schema = "pelican.settings";
constexpr int settings_version = 1;

std::filesystem::path resolveUserPath(std::string_view ref) {
    const auto resolved = GET_MODULE(PathResolver).resolveProjectRef(ref);
    const auto *path = std::get_if<std::filesystem::path>(&resolved);
    if (path == nullptr) {
        throw std::runtime_error("Persistence path did not resolve to a file: " + std::string{ref});
    }
    return *path;
}

void rejectUnknownKeys(const nlohmann::json &object, const std::set<std::string> &allowed,
                       std::string_view context) {
    for (const auto &[key, value] : object.items()) {
        (void)value;
        if (!allowed.contains(key)) {
            throw std::runtime_error(std::string{context} + " contains unsupported key: " + key);
        }
    }
}

float readBusVolume(const nlohmann::json &bus_volumes, const char *name, float fallback) {
    if (!bus_volumes.contains(name)) {
        return fallback;
    }
    const auto &value = bus_volumes.at(name);
    if (!value.is_number()) {
        throw std::runtime_error(std::string{"settings engine.audio.bus_volumes."} + name +
                                 " must be a number");
    }
    const double volume = value.get<double>();
    if (!std::isfinite(volume) || volume < 0.0 ||
        volume > static_cast<double>((std::numeric_limits<float>::max)())) {
        throw std::runtime_error(std::string{"settings engine.audio.bus_volumes."} + name +
                                 " must be a finite non-negative number");
    }
    return static_cast<float>(volume);
}

std::pair<AudioBusSettings, nlohmann::json> parseSettings(const nlohmann::json &document) {
    if (!document.is_object()) {
        throw std::runtime_error("settings document must be an object");
    }
    rejectUnknownKeys(document, {"schema", "version", "engine", "game"}, "settings document");

    if (!document.contains("schema") || !document.at("schema").is_string() ||
        document.at("schema").get<std::string>() != settings_schema) {
        throw std::runtime_error("settings schema is not supported");
    }
    if (!document.contains("version") || !document.at("version").is_number_integer() ||
        document.at("version").get<int>() != settings_version) {
        throw std::runtime_error("settings version is not supported");
    }
    if (!document.contains("engine") || !document.at("engine").is_object()) {
        throw std::runtime_error("settings engine section must be an object");
    }
    if (!document.contains("game")) {
        throw std::runtime_error("settings game section is required");
    }

    AudioBusSettings volumes;
    const auto &engine = document.at("engine");
    rejectUnknownKeys(engine, {"audio"}, "settings engine section");
    if (engine.contains("audio")) {
        const auto &audio = engine.at("audio");
        if (!audio.is_object()) {
            throw std::runtime_error("settings engine.audio must be an object");
        }
        rejectUnknownKeys(audio, {"bus_volumes"}, "settings engine.audio");
        if (audio.contains("bus_volumes")) {
            const auto &bus_volumes = audio.at("bus_volumes");
            if (!bus_volumes.is_object()) {
                throw std::runtime_error("settings engine.audio.bus_volumes must be an object");
            }
            rejectUnknownKeys(bus_volumes, {"master", "bgm", "se"},
                              "settings engine.audio.bus_volumes");
            volumes.master = readBusVolume(bus_volumes, "master", volumes.master);
            volumes.bgm = readBusVolume(bus_volumes, "bgm", volumes.bgm);
            volumes.se = readBusVolume(bus_volumes, "se", volumes.se);
        }
    }

    return {volumes, document.at("game")};
}

nlohmann::json readJsonFile(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    if (!file.is_open()) {
        throw std::runtime_error("failed to open file");
    }
    return nlohmann::json::parse(file);
}

void replaceFileAtomically(const std::filesystem::path &temp, const std::filesystem::path &destination) {
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const std::error_code ec{static_cast<int>(GetLastError()), std::system_category()};
        throw std::runtime_error("atomic replace failed: " + ec.message());
    }
#else
    std::error_code ec;
    std::filesystem::rename(temp, destination, ec);
    if (ec) {
        throw std::runtime_error("atomic replace failed: " + ec.message());
    }
#endif
}

void atomicWrite(const std::filesystem::path &destination, std::string_view contents) {
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) {
        throw std::runtime_error("failed to create user data directory: " + ec.message());
    }

    auto temp = destination;
    temp += ".tmp";
    try {
        std::ofstream file{temp, std::ios::binary | std::ios::trunc};
        if (!file.is_open()) {
            throw std::runtime_error("failed to open temporary file for writing");
        }
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        file.flush();
        if (!file) {
            throw std::runtime_error("failed to write temporary file");
        }
        file.close();
        if (!file) {
            throw std::runtime_error("failed to close temporary file");
        }
        replaceFileAtomically(temp, destination);
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove(temp, cleanup_error);
        throw;
    }
}

void validateSlot(std::string_view slot) {
    if (slot.empty()) {
        throw std::runtime_error("save slot must not be empty");
    }
    if (slot == "." || slot == ".." || slot.ends_with('.') || slot.ends_with(' ')) {
        throw std::runtime_error("save slot is not a portable file name: " + std::string{slot});
    }
    for (const unsigned char ch : slot) {
        if (ch < 0x20 || ch == '/' || ch == '\\' || ch == '#' || ch == ':' || ch == '"' ||
            ch == '<' || ch == '>' || ch == '|' || ch == '?' || ch == '*') {
            throw std::runtime_error("save slot contains an invalid file-name character: " +
                                     std::string{slot});
        }
    }
}

std::string saveRef(std::string_view slot) {
    validateSlot(slot);
    return "user://saves/" + std::string{slot} + ".json";
}

std::string dumpedJson(const nlohmann::json &json) {
    return json.dump(2) + "\n";
}

} // namespace

bool Persistence::loadSettings() {
    audio_bus_settings = {};
    game_settings = nlohmann::json::object();
    last_settings_error.reset();

    std::filesystem::path path;
    try {
        path = resolveUserPath(settings_ref);
        std::error_code ec;
        const bool exists = std::filesystem::exists(path, ec);
        if (ec) {
            throw std::runtime_error("failed to inspect settings file: " + ec.message());
        }
        if (!exists) {
            return false;
        }

        auto [volumes, game] = parseSettings(readJsonFile(path));
        audio_bus_settings = volumes;
        game_settings = std::move(game);
        LOG_INFO(logger, "loaded user settings: {}", path.string());
        return true;
    } catch (const std::exception &error) {
        const auto name = path.empty() ? std::string{settings_ref} : path.string();
        last_settings_error = "failed to load settings '" + name + "': " + error.what();
        LOG_WARNING(logger, "{}", *last_settings_error);
        return false;
    }
}

void Persistence::saveSettings() const {
    const nlohmann::json document{
        {"schema", settings_schema},
        {"version", settings_version},
        {"engine",
         {{"audio",
           {{"bus_volumes",
             {{"master", audio_bus_settings.master},
              {"bgm", audio_bus_settings.bgm},
              {"se", audio_bus_settings.se}}}}}}},
        {"game", game_settings},
    };
    const auto path = resolveUserPath(settings_ref);
    atomicWrite(path, dumpedJson(document));
    LOG_INFO(logger, "saved user settings: {}", path.string());
}

nlohmann::json Persistence::gameSettings() const {
    return game_settings;
}

void Persistence::setGameSettings(nlohmann::json settings) {
    game_settings = std::move(settings);
}

const AudioBusSettings &Persistence::audioBusSettings() const {
    return audio_bus_settings;
}

const std::optional<std::string> &Persistence::lastSettingsError() const {
    return last_settings_error;
}

#if PELICAN_WITH_AUDIO
void Persistence::applyAudioSettings(Audio &audio) const {
    audio.setBusVolume("master", audio_bus_settings.master);
    audio.setBusVolume("bgm", audio_bus_settings.bgm);
    audio.setBusVolume("se", audio_bus_settings.se);
}

void Persistence::captureAudioSettings(const Audio &audio) {
    audio_bus_settings.master = audio.busVolume("master");
    audio_bus_settings.bgm = audio.busVolume("bgm");
    audio_bus_settings.se = audio.busVolume("se");
}
#endif

void Persistence::saveData(std::string_view slot, const nlohmann::json &data) const {
    const auto path = resolveUserPath(saveRef(slot));
    atomicWrite(path, dumpedJson(data));
    LOG_INFO(logger, "saved data slot '{}': {}", std::string{slot}, path.string());
}

std::optional<nlohmann::json> Persistence::loadData(std::string_view slot) const {
    std::filesystem::path path;
    try {
        path = resolveUserPath(saveRef(slot));
        std::error_code ec;
        const bool exists = std::filesystem::exists(path, ec);
        if (ec) {
            throw std::runtime_error("failed to inspect save file: " + ec.message());
        }
        if (!exists) {
            return std::nullopt;
        }
        return readJsonFile(path);
    } catch (const std::exception &error) {
        const auto name = path.empty() ? std::string{slot} : path.string();
        LOG_WARNING(logger, "failed to load save '{}': {}", name, error.what());
        return std::nullopt;
    }
}

std::vector<PersistenceSlotInfo> Persistence::listSaves() const {
    const auto saves_dir = resolveUserPath("user://saves");
    std::error_code ec;
    if (!std::filesystem::exists(saves_dir, ec)) {
        if (ec) {
            LOG_WARNING(logger, "failed to inspect saves directory '{}': {}", saves_dir.string(),
                        ec.message());
        }
        return {};
    }

    std::vector<PersistenceSlotInfo> slots;
    std::filesystem::directory_iterator it{saves_dir, ec};
    const std::filesystem::directory_iterator end;
    while (!ec && it != end) {
        const auto &entry = *it;
        std::error_code entry_error;
        if (entry.is_regular_file(entry_error) && !entry_error && entry.path().extension() == ".json") {
            const auto timestamp = entry.last_write_time(entry_error);
            if (!entry_error) {
                slots.push_back(PersistenceSlotInfo{
                    .slot = entry.path().stem().string(),
                    .timestamp = timestamp,
                });
            } else {
                LOG_WARNING(logger, "failed to read save timestamp '{}': {}", entry.path().string(),
                            entry_error.message());
            }
        }
        it.increment(ec);
    }
    if (ec) {
        LOG_WARNING(logger, "failed to list saves directory '{}': {}", saves_dir.string(), ec.message());
    }

    std::sort(slots.begin(), slots.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.slot < rhs.slot;
    });
    return slots;
}

void Persistence::resetForTesting() {
    audio_bus_settings = {};
    game_settings = nlohmann::json::object();
    last_settings_error.reset();
}

} // namespace Pelican
