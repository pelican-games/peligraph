#pragma once

#include "sceneload.hpp"
#include "../persistence/persistence.hpp"
#include "../audio/audio.hpp"
#include <components/localtransform.hpp>
#include <components/spriteview.hpp>
#include <handle.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {
class ECSEntityMutationToken;
class SceneLoader;

namespace internal {

inline constexpr std::size_t sceneEffectCapacity = 256;

class PreparedEffectTransaction {
  public:
    enum class CallbackPhase : std::uint8_t { activate, destroy, flush };

  private:
    enum class EntryKind : std::uint8_t {
        play,
        stop,
        bus_volume,
        save_data,
        save_settings,
    };

    struct Entry {
        EntryKind kind = EntryKind::save_data;
        std::uint64_t sequence = 0;
        bool active = true;
        SoundHandle handle{};
        std::string resource;
        float volume = 1.0f;
        Audio::PreparedSound sound;
        std::optional<Persistence::PreparedWrite> write;
    };

    Persistence *persistence_ = nullptr;
#if PELICAN_WITH_AUDIO
    Audio *audio_ = nullptr;
#endif
    SceneLoader *loader_ = nullptr;
    std::vector<Entry> entries_;
    std::vector<CommittedWithEffectFailure::EffectError> errors_;
    nlohmann::json game_settings_shadow_;
    AudioBusSettings audio_shadow_{};
    std::span<const ECSEntityMutationToken> retired_objects_{};
    CallbackPhase phase_ = CallbackPhase::activate;
    std::uint64_t next_sequence_ = 0;
    std::uint64_t dropped_entries_ = 0;
    std::size_t overflow_error_index_ = static_cast<std::size_t>(-1);
    bool overflow_reported_ = false;

    Entry *find(EntryKind kind, std::string_view resource) noexcept;
    const Entry *findPlay(SoundHandle handle) const noexcept;
    Entry *findPlay(SoundHandle handle) noexcept;
    bool reserveEntry() noexcept;
    void record(std::string_view api, std::string_view code,
                nlohmann::ordered_json fields = {}) noexcept;

  public:
    PreparedEffectTransaction(Persistence &persistence, SceneLoader &loader);
    ~PreparedEffectTransaction();
    PreparedEffectTransaction(const PreparedEffectTransaction &) = delete;
    PreparedEffectTransaction &operator=(const PreparedEffectTransaction &) = delete;

    CallbackPhase phase() const noexcept { return phase_; }
    void setPhase(CallbackPhase phase) noexcept { phase_ = phase; }
    void setRetiredObjects(
        std::span<const ECSEntityMutationToken> objects) noexcept {
        retired_objects_ = objects;
    }

    SoundHandle playSound(std::string_view path);
    void stopSound(SoundHandle handle);
    void setBusVolume(std::string_view bus, float volume);
    float busVolume(std::string_view bus);
    bool isPlaying(SoundHandle handle);

    nlohmann::json gameSettings();
    void setGameSettings(nlohmann::json settings);
    void saveSettings();
    void saveData(std::string_view slot, const nlohmann::json &data);
    std::optional<nlohmann::json> loadData(std::string_view slot);
    std::vector<PersistenceSlotInfo> listSaves();
    void loadScene(std::string_view name);

    const void *retiredComponent(GameObjectId id,
                                 std::uint64_t component_id) const noexcept;
    bool rejectRetiredMutation(std::string_view api,
                               GameObjectId id) noexcept;

    std::vector<CommittedWithEffectFailure::EffectError>
    flush(SceneLoadFaultInjector *injector,
          SceneLoadPhaseTrace &phase_trace) noexcept;
};

PreparedEffectTransaction *activeEffectTransaction() noexcept;

class EffectTransactionScope {
    PreparedEffectTransaction *previous_ = nullptr;
  public:
    explicit EffectTransactionScope(PreparedEffectTransaction &transaction) noexcept;
    ~EffectTransactionScope();
    EffectTransactionScope(const EffectTransactionScope &) = delete;
    EffectTransactionScope &operator=(const EffectTransactionScope &) = delete;
};

class EffectDestroyScope {
    PreparedEffectTransaction *transaction_ = nullptr;
    PreparedEffectTransaction::CallbackPhase previous_{};
  public:
    EffectDestroyScope() noexcept;
    ~EffectDestroyScope();
};

} // namespace internal
} // namespace Pelican
