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
        SoundHandle handle{};
        std::string resource;
        std::string diagnostic_path;
        float volume = 1.0f;
        Audio::PreparedSound sound;
        std::optional<Persistence::PreparedWrite> write;
    };

    enum class ErrorApi : std::uint8_t {
        play_sound,
        stop_sound,
        set_bus_volume,
        bus_volume,
        is_playing,
        game_settings,
        set_game_settings,
        save_settings,
        save_data,
        load_data,
        list_saves,
        load_scene,
        set_local_transform,
        set_sprite_view,
        set_sprite_texture,
        effect_outbox,
    };

    enum class ErrorCode : std::uint8_t {
        scene_retired_object_mutation_rejected,
        effect_outbox_full,
        build_feature_disabled,
        audio_play_stage_failed,
        audio_stop_stage_failed,
        audio_bus_volume_stage_failed,
        audio_is_playing_stage_failed,
        persistence_shadow_failed,
        persistence_stage_failed,
        persistence_overlay_failed,
        persistence_read_failed,
        scene_retire_load_rejected,
        audio_play_failed,
        audio_stop_failed,
        audio_bus_volume_failed,
        persistence_write_failed,
    };

    enum class ErrorFields : std::uint8_t {
        reason,
        sound_reason,
        handle_reason,
        bus_reason,
        slot_reason,
        scene_reason,
        object_id,
        path_reason,
        handle_sound_reason,
        dropped,
    };

    template <std::size_t Capacity> struct FixedText {
        std::array<char, Capacity> bytes{};
        std::uint16_t size = 0;
    };

    struct FixedEffectError {
        ErrorApi api = ErrorApi::effect_outbox;
        ErrorCode code = ErrorCode::effect_outbox_full;
        ErrorFields fields = ErrorFields::reason;
        CallbackPhase phase = CallbackPhase::activate;
        std::uint64_t sequence = 0;
        std::uint64_t value = 0;
        std::uint32_t value2 = 0;
        FixedText<512> primary;
        FixedText<384> reason;
    };

    Persistence *persistence_ = nullptr;
#if PELICAN_WITH_AUDIO
    Audio *audio_ = nullptr;
#endif
    SceneLoader *loader_ = nullptr;
    std::vector<Entry> entries_;
    std::array<FixedEffectError, sceneEffectCapacity + 1U> errors_{};
    std::size_t error_count_ = 0;
    nlohmann::json game_settings_shadow_;
    AudioBusSettings audio_shadow_{};
    std::span<const ECSEntityMutationToken> retired_objects_{};
    CallbackPhase phase_ = CallbackPhase::activate;
    std::uint64_t next_sequence_ = 0;
    std::uint64_t dropped_entries_ = 0;
    bool overflow_reported_ = false;

    Entry *find(EntryKind kind, std::string_view resource) noexcept;
    const Entry *findPlay(SoundHandle handle) const noexcept;
    Entry *findPlay(SoundHandle handle) noexcept;
    bool reserveEntry(std::uint64_t sequence) noexcept;
    void record(std::uint64_t sequence, ErrorApi api, ErrorCode code,
                ErrorFields fields, std::string_view primary = {},
                std::uint64_t value = 0,
                std::uint32_t value2 = 0,
                std::string_view reason = {}) noexcept;

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

    void flush(SceneLoadFaultInjector *injector,
               SceneLoadPhaseTrace &phase_trace) noexcept;
    std::vector<CommittedWithEffectFailure::EffectError>
    effectErrors() const;
    std::size_t fixedErrorCount() const noexcept {
        return error_count_ + (overflow_reported_ ? 1U : 0U);
    }
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
