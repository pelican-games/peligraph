#pragma once

#include "../container.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

class PersistenceReadError final : public std::runtime_error {
    std::string path_;
    std::string reason_;

  public:
    PersistenceReadError(std::string path, std::string reason);
    const std::string &path() const noexcept { return path_; }
    const std::string &reason() const noexcept { return reason_; }
};

class Audio;

struct AudioBusSettings {
    float master = 1.0f;
    float bgm = 1.0f;
    float se = 1.0f;
};

struct PersistenceSlotInfo {
    std::string slot;
    std::filesystem::file_time_type timestamp;
};

DECLARE_MODULE(Persistence) {
    nlohmann::json game_settings = nlohmann::json::object();
    AudioBusSettings audio_bus_settings;
    std::optional<std::string> last_settings_error;
    mutable std::size_t prepared_write_commit_count = 0;

  public:
    struct PreparedWrite {
        std::filesystem::path path;
        std::string bytes;
        std::string resource_key;
        std::optional<std::string> slot;
        std::optional<nlohmann::json> overlay_data;
    };

    bool loadSettings();
    void saveSettings() const;

    nlohmann::json gameSettings() const;
    void setGameSettings(nlohmann::json settings);
    const AudioBusSettings &audioBusSettings() const;
    const std::optional<std::string> &lastSettingsError() const;

#if PELICAN_WITH_AUDIO
    void applyAudioSettings(Audio &audio) const;
    void captureAudioSettings(const Audio &audio);
#endif

    void saveData(std::string_view slot, const nlohmann::json &data) const;
    std::optional<nlohmann::json> loadData(std::string_view slot) const;
    std::vector<PersistenceSlotInfo> listSaves() const;

    PreparedWrite prepareSettingsWrite(
        const nlohmann::json &game,
        const AudioBusSettings &audio) const;
    PreparedWrite prepareDataWrite(
        std::string_view slot, const nlohmann::json &data) const;
    std::string dataResourceKey(std::string_view slot) const;
    void commitPreparedWrite(const PreparedWrite &write) const;
    void commitGameSettings(nlohmann::json &&settings) noexcept;
    std::size_t preparedWriteCommitCountForTesting() const noexcept {
        return prepared_write_commit_count;
    }

    void resetForTesting();
};

} // namespace Pelican
