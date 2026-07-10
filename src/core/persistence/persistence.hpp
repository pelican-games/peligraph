#pragma once

#include "../container.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

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

  public:
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

    void resetForTesting();
};

} // namespace Pelican
