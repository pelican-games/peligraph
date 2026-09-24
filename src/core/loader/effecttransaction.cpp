#include "effecttransaction.hpp"

#include "scene.hpp"
#include "sceneloadfault.hpp"
#include "../build_features.hpp"
#include "../log.hpp"
#include "../phys/physquery.hpp"
#include "../ecs/archetypemigration.hpp"
#include <details/ecs/componentdeclare.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <utility>

namespace Pelican::internal {
namespace {
thread_local PreparedEffectTransaction *active_effect_transaction = nullptr;
// How many destruction callbacks (EffectDestroyScope) enclose this thread's
// current call, with or without a scene transaction (PG-2b).
thread_local std::size_t destroy_scope_depth = 0;

std::string_view currentExceptionWhat() noexcept {
    try {
        throw;
    } catch (const std::exception &error) {
        return error.what() != nullptr ? std::string_view{error.what()}
                                       : std::string_view{};
    } catch (...) {
        return "unknown effect failure";
    }
}

float &busValue(AudioBusSettings &settings, std::string_view bus) {
    if (bus == "master") return settings.master;
    if (bus == "bgm") return settings.bgm;
    if (bus == "se") return settings.se;
    throw std::runtime_error("unknown audio bus: " + std::string{bus});
}

float busValue(const AudioBusSettings &settings, std::string_view bus) {
    auto copy = settings;
    return busValue(copy, bus);
}
} // namespace

PreparedEffectTransaction::PreparedEffectTransaction(
    Persistence &persistence, SceneLoader &loader)
    : persistence_{&persistence}, loader_{&loader},
      game_settings_shadow_{persistence.gameSettings()},
      audio_shadow_{persistence.audioBusSettings()} {
    entries_.reserve(sceneEffectCapacity);
#if PELICAN_WITH_AUDIO
    audio_ = &GET_MODULE(Audio);
    audio_shadow_.master = audio_->busVolume("master");
    audio_shadow_.bgm = audio_->busVolume("bgm");
    audio_shadow_.se = audio_->busVolume("se");
    audio_->reserveEffectVoices(sceneEffectCapacity);
#endif
}

PreparedEffectTransaction::~PreparedEffectTransaction() {
#if PELICAN_WITH_AUDIO
    for (const auto &entry : entries_) {
        if (entry.kind == EntryKind::play) {
            audio_->cancelPreparedVoice(entry.handle);
        }
    }
#endif
}

PreparedEffectTransaction::Entry *PreparedEffectTransaction::find(
    EntryKind kind, std::string_view resource) noexcept {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
        [&](const Entry &entry) {
            return entry.kind == kind && entry.resource == resource;
        });
    return found == entries_.end() ? nullptr : &*found;
}

const PreparedEffectTransaction::Entry *
PreparedEffectTransaction::findPlay(SoundHandle handle) const noexcept {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
        [&](const Entry &entry) {
            return entry.kind == EntryKind::play && entry.handle == handle;
        });
    return found == entries_.end() ? nullptr : &*found;
}

PreparedEffectTransaction::Entry *
PreparedEffectTransaction::findPlay(SoundHandle handle) noexcept {
    return const_cast<Entry *>(std::as_const(*this).findPlay(handle));
}

void PreparedEffectTransaction::record(
    std::uint64_t sequence, ErrorApi api, ErrorCode code, ErrorFields fields,
    std::string_view primary, std::uint64_t value, std::uint32_t value2,
    std::string_view reason) noexcept {
    if (error_count_ >= sceneEffectCapacity) return;
    auto &slot = errors_[error_count_++];
    slot.api = api;
    slot.code = code;
    slot.fields = fields;
    slot.phase = phase_;
    slot.sequence = sequence;
    slot.value = value;
    slot.value2 = value2;
    const auto copy = [](auto &target, std::string_view source) noexcept {
        const auto count = (std::min)(source.size(), target.bytes.size());
        if (count != 0) std::memcpy(target.bytes.data(), source.data(), count);
        target.size = static_cast<std::uint16_t>(count);
    };
    copy(slot.primary, primary);
    copy(slot.reason, reason);
}

bool PreparedEffectTransaction::reserveEntry(
    std::uint64_t sequence) noexcept {
    if (entries_.size() < sceneEffectCapacity) return true;
    ++dropped_entries_;
    auto &overflow = errors_.back();
    if (!overflow_reported_) {
        overflow_reported_ = true;
        overflow.api = ErrorApi::effect_outbox;
        overflow.code = ErrorCode::effect_outbox_full;
        overflow.fields = ErrorFields::dropped;
        overflow.phase = phase_;
        overflow.sequence = sequence;
    }
    overflow.value = dropped_entries_;
    return false;
}

SoundHandle PreparedEffectTransaction::playSound(std::string_view path) {
    const auto sequence = next_sequence_++;
    try {
#if PELICAN_WITH_AUDIO
        if (!reserveEntry(sequence)) return SoundHandle{};
        auto sound = audio_->prepareSound(path);
        const auto handle = audio_->reserveSoundHandle();
        entries_.push_back(Entry{
            .kind = EntryKind::play,
            .sequence = sequence,
            .handle = handle,
            .sound = std::move(sound),
        });
        try {
            audio_->prepareVoiceSlot(handle);
        } catch (...) {
            entries_.pop_back();
            throw;
        }
        return handle;
#else
        throwBuildFeatureCallDisabled("PELICAN_WITH_AUDIO", "playSound");
#endif
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record(sequence, ErrorApi::play_sound,
#if PELICAN_WITH_AUDIO
               ErrorCode::audio_play_stage_failed,
#else
               ErrorCode::build_feature_disabled,
#endif
               ErrorFields::sound_reason, path, 0, 0,
               currentExceptionWhat());
        return SoundHandle{};
    }
}

void PreparedEffectTransaction::stopSound(SoundHandle handle) {
    const auto sequence = next_sequence_++;
    try {
#if PELICAN_WITH_AUDIO
        if (auto *play = findPlay(handle)) {
            const auto index = static_cast<std::size_t>(play - entries_.data());
            audio_->cancelPreparedVoice(handle);
            entries_.erase(entries_.begin() +
                           static_cast<std::ptrdiff_t>(index));
            return;
        }
        const auto duplicate = std::find_if(
            entries_.begin(), entries_.end(), [&](const Entry &entry) {
                return entry.kind == EntryKind::stop && entry.handle == handle;
            });
        if (duplicate != entries_.end() || !audio_->isPlaying(handle)) return;
        if (!reserveEntry(sequence)) return;
        entries_.push_back(Entry{.kind = EntryKind::stop,
                                 .sequence = sequence,
                                 .handle = handle});
#else
        (void)handle;
        throwBuildFeatureCallDisabled("PELICAN_WITH_AUDIO", "stopSound");
#endif
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record(sequence, ErrorApi::stop_sound,
#if PELICAN_WITH_AUDIO
               ErrorCode::audio_stop_stage_failed,
#else
               ErrorCode::build_feature_disabled,
#endif
               ErrorFields::handle_reason, {}, handle.value, 0,
               currentExceptionWhat());
    }
}

void PreparedEffectTransaction::setBusVolume(std::string_view bus,
                                              float volume) {
    const auto sequence = next_sequence_++;
    try {
#if PELICAN_WITH_AUDIO
        if (!std::isfinite(volume) || volume < 0.0f) {
            throw std::runtime_error(
                "Audio bus volume must be a finite non-negative number");
        }
        (void)audioBusFromName(bus);
        busValue(audio_shadow_, bus) = volume;
        auto *entry = find(EntryKind::bus_volume, bus);
        if (entry != nullptr) {
            entry->volume = volume;
            entry->sequence = sequence;
            return;
        }
        if (!reserveEntry(sequence)) return;
        entries_.push_back(Entry{.kind = EntryKind::bus_volume,
                                 .sequence = sequence,
                                 .resource = std::string{bus},
                                 .volume = volume});
#else
        (void)bus;
        (void)volume;
        throwBuildFeatureCallDisabled("PELICAN_WITH_AUDIO", "setBusVolume");
#endif
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record(sequence, ErrorApi::set_bus_volume,
#if PELICAN_WITH_AUDIO
               ErrorCode::audio_bus_volume_stage_failed,
#else
               ErrorCode::build_feature_disabled,
#endif
               ErrorFields::bus_reason, bus, 0, 0, currentExceptionWhat());
    }
}

float PreparedEffectTransaction::busVolume(std::string_view bus) {
    const auto sequence = next_sequence_++;
    try {
#if PELICAN_WITH_AUDIO
        (void)audioBusFromName(bus);
        return busValue(audio_shadow_, bus);
#else
        (void)bus;
        throwBuildFeatureCallDisabled("PELICAN_WITH_AUDIO", "busVolume");
#endif
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record(sequence, ErrorApi::bus_volume,
#if PELICAN_WITH_AUDIO
               ErrorCode::audio_bus_volume_stage_failed,
#else
               ErrorCode::build_feature_disabled,
#endif
               ErrorFields::bus_reason, bus, 0, 0, currentExceptionWhat());
        return 0.0F;
    }
}

bool PreparedEffectTransaction::isPlaying(SoundHandle handle) {
    const auto sequence = next_sequence_++;
    try {
#if PELICAN_WITH_AUDIO
        if (findPlay(handle) != nullptr) return true;
        const auto stopped = std::find_if(
            entries_.begin(), entries_.end(), [&](const Entry &entry) {
                return entry.kind == EntryKind::stop && entry.handle == handle;
            });
        if (stopped != entries_.end()) return false;
        return audio_->isPlaying(handle);
#else
        (void)handle;
        throwBuildFeatureCallDisabled("PELICAN_WITH_AUDIO", "isPlaying");
#endif
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record(sequence, ErrorApi::is_playing,
#if PELICAN_WITH_AUDIO
               ErrorCode::audio_is_playing_stage_failed,
#else
               ErrorCode::build_feature_disabled,
#endif
               ErrorFields::handle_reason, {}, handle.value, 0,
               currentExceptionWhat());
        return false;
    }
}

nlohmann::json PreparedEffectTransaction::gameSettings() {
    const auto sequence = next_sequence_++;
    try {
        return game_settings_shadow_;
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record(sequence, ErrorApi::game_settings,
               ErrorCode::persistence_shadow_failed, ErrorFields::reason,
               {}, 0, 0, currentExceptionWhat());
        return {};
    }
}

void PreparedEffectTransaction::setGameSettings(nlohmann::json settings) {
    const auto sequence = next_sequence_++;
    try {
        game_settings_shadow_ = std::move(settings);
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record(sequence, ErrorApi::set_game_settings,
               ErrorCode::persistence_shadow_failed, ErrorFields::reason,
               {}, 0, 0, currentExceptionWhat());
    }
}

void PreparedEffectTransaction::saveSettings() {
    const auto sequence = next_sequence_++;
    try {
        auto write = persistence_->prepareSettingsWrite(
            game_settings_shadow_, audio_shadow_);
        auto *entry = find(EntryKind::save_settings, write.resource_key);
        if (entry != nullptr) {
            entry->diagnostic_path = write.path.string();
            entry->write = std::move(write);
            entry->sequence = sequence;
            return;
        }
        if (!reserveEntry(sequence)) return;
        entries_.push_back(Entry{.kind = EntryKind::save_settings,
                                 .sequence = sequence,
                                 .resource = write.resource_key,
                                 .diagnostic_path = write.path.string(),
                                 .write = std::move(write)});
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record(sequence, ErrorApi::save_settings,
               ErrorCode::persistence_stage_failed, ErrorFields::reason,
               {}, 0, 0, currentExceptionWhat());
    }
}

void PreparedEffectTransaction::saveData(
    std::string_view slot, const nlohmann::json &data) {
    const auto sequence = next_sequence_++;
    try {
        auto write = persistence_->prepareDataWrite(slot, data);
        auto *entry = find(EntryKind::save_data, write.resource_key);
        if (entry != nullptr) {
            entry->diagnostic_path = write.path.string();
            entry->write = std::move(write);
            entry->sequence = sequence;
            return;
        }
        if (!reserveEntry(sequence)) return;
        entries_.push_back(Entry{.kind = EntryKind::save_data,
                                 .sequence = sequence,
                                 .resource = write.resource_key,
                                 .diagnostic_path = write.path.string(),
                                 .write = std::move(write)});
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record(sequence, ErrorApi::save_data,
               ErrorCode::persistence_stage_failed, ErrorFields::slot_reason,
               slot, 0, 0, currentExceptionWhat());
    }
}

std::optional<nlohmann::json> PreparedEffectTransaction::loadData(
    std::string_view slot) {
    const auto sequence = next_sequence_++;
    try {
        const auto resource = persistence_->dataResourceKey(slot);
        const auto found = std::find_if(entries_.begin(), entries_.end(),
            [&](const Entry &entry) {
                return entry.kind == EntryKind::save_data &&
                       entry.resource == resource;
            });
        if (found != entries_.end() && found->write &&
            found->write->overlay_data) return *found->write->overlay_data;
        return persistence_->loadData(slot);
    } catch (const PersistenceReadError &error) {
        if (phase_ != CallbackPhase::destroy) throw;
        record(sequence, ErrorApi::load_data,
               ErrorCode::persistence_read_failed, ErrorFields::path_reason,
               error.path(), 0, 0, error.reason());
        return std::nullopt;
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record(sequence, ErrorApi::load_data,
               ErrorCode::persistence_overlay_failed,
               ErrorFields::slot_reason, slot, 0, 0,
               currentExceptionWhat());
        return std::nullopt;
    }
}

std::vector<PersistenceSlotInfo> PreparedEffectTransaction::listSaves() {
    const auto sequence = next_sequence_++;
    try {
        auto result = persistence_->listSaves();
        for (const auto &entry : entries_) {
            if (entry.kind != EntryKind::save_data || !entry.write ||
                !entry.write->slot) continue;
            const auto found = std::find_if(result.begin(), result.end(),
                [&](const PersistenceSlotInfo &slot) {
                    return slot.slot == *entry.write->slot;
                });
            if (found == result.end()) {
                result.push_back(PersistenceSlotInfo{
                    *entry.write->slot, std::filesystem::file_time_type{}});
            } else {
                found->timestamp = std::filesystem::file_time_type{};
            }
        }
        std::sort(result.begin(), result.end(), [](const auto &left,
                                                    const auto &right) {
            return left.slot < right.slot;
        });
        return result;
    } catch (const PersistenceReadError &error) {
        if (phase_ != CallbackPhase::destroy) throw;
        record(sequence, ErrorApi::list_saves,
               ErrorCode::persistence_read_failed, ErrorFields::path_reason,
               error.path(), 0, 0, error.reason());
        return {};
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record(sequence, ErrorApi::list_saves,
               ErrorCode::persistence_overlay_failed, ErrorFields::reason,
               {}, 0, 0, currentExceptionWhat());
        return {};
    }
}

void PreparedEffectTransaction::loadScene(std::string_view name) {
    const auto sequence = next_sequence_++;
    try {
        if (phase_ == CallbackPhase::destroy) {
            loader_->requestLoadDuringRetire(std::string{name});
        } else {
            loader_->requestLoad(std::string{name});
        }
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record(sequence, ErrorApi::load_scene,
               ErrorCode::scene_retire_load_rejected,
               ErrorFields::scene_reason, name, 0, 0,
               currentExceptionWhat());
    }
}

const void *PreparedEffectTransaction::retiredComponent(
    GameObjectId id, std::uint64_t component_id) const noexcept {
    for (const auto &object : retired_objects_) {
        if (object.entity() == id &&
            object.kind() == ECSEntityMutationKind::destroy) {
            return object.component(component_id);
        }
    }
    return nullptr;
}

bool PreparedEffectTransaction::rejectRetiredMutation(
    std::string_view api, GameObjectId id) noexcept {
    const auto found = std::find_if(
        retired_objects_.begin(), retired_objects_.end(),
        [&](const ECSEntityMutationToken &object) {
            return object.entity() == id &&
                   object.kind() == ECSEntityMutationKind::destroy;
        });
    if (found == retired_objects_.end()) return false;
    const auto mapped_api = api == "setLocalTransform"
        ? ErrorApi::set_local_transform
        : api == "setSpriteView" ? ErrorApi::set_sprite_view
                                  : ErrorApi::set_sprite_texture;
    record(next_sequence_++, mapped_api,
           ErrorCode::scene_retired_object_mutation_rejected,
           ErrorFields::object_id, {}, id.index, id.generation);
    return true;
}

void PreparedEffectTransaction::flush(SceneLoadFaultInjector *injector,
                                      SceneLoadPhaseTrace &phase_trace) noexcept {
    (void)phase_trace; // EventOutbox owns the executor's single flush marker.
    phase_ = CallbackPhase::flush;
    persistence_->commitGameSettings(std::move(game_settings_shadow_));

    for (std::uint64_t sequence = 0; sequence < next_sequence_; ++sequence) {
        auto found = std::find_if(entries_.begin(), entries_.end(),
            [&](const Entry &entry) { return entry.sequence == sequence; });
        if (found == entries_.end()) continue;
        auto &entry = *found;
        try {
            switch (entry.kind) {
            case EntryKind::play:
#if PELICAN_WITH_AUDIO
                if (sceneLoadMutationEnabled(
                        injector, SceneLoadFaultPhase::flush, "audio",
                        scene_load_mutation::effect_audio_play)) {
                    record(entry.sequence, ErrorApi::play_sound,
                           ErrorCode::audio_play_failed,
                           ErrorFields::handle_sound_reason,
                           entry.sound.source, entry.handle.value, 0,
                           "injected audio play failure");
                    break;
                }
                audio_->commitPreparedPlay(entry.handle, entry.sound);
#endif
                break;
            case EntryKind::stop:
#if PELICAN_WITH_AUDIO
                audio_->commitPreparedStop(
                    entry.handle,
                    sceneLoadMutationEnabled(
                        injector, SceneLoadFaultPhase::flush, "audio",
                        scene_load_mutation::effect_audio_stop));
#endif
                break;
            case EntryKind::bus_volume:
#if PELICAN_WITH_AUDIO
                audio_->publishBusVolume(entry.resource, entry.volume);
                if (sceneLoadMutationEnabled(
                        injector, SceneLoadFaultPhase::flush, "audio",
                        scene_load_mutation::effect_audio_bus_volume)) {
                    record(entry.sequence, ErrorApi::set_bus_volume,
                           ErrorCode::audio_bus_volume_failed,
                           ErrorFields::bus_reason, entry.resource, 0, 0,
                           "injected audio bus volume failure");
                    break;
                }
                audio_->applyBusVolume();
#endif
                break;
            case EntryKind::save_data:
            case EntryKind::save_settings: {
                const auto mutation = entry.kind == EntryKind::save_data
                    ? scene_load_mutation::effect_save_data
                    : scene_load_mutation::effect_save_settings;
                if (sceneLoadMutationEnabled(
                        injector, SceneLoadFaultPhase::flush, "persistence",
                        mutation)) {
                    record(entry.sequence,
                           entry.kind == EntryKind::save_data
                               ? ErrorApi::save_data
                               : ErrorApi::save_settings,
                           ErrorCode::persistence_write_failed,
                           ErrorFields::path_reason,
                           entry.diagnostic_path, 0, 0,
                           "injected persistence write failure");
                    break;
                }
                persistence_->commitPreparedWrite(*entry.write);
                break;
            }
            }
        } catch (...) {
            const auto reason = currentExceptionWhat();
            switch (entry.kind) {
            case EntryKind::play:
                record(entry.sequence, ErrorApi::play_sound,
                       ErrorCode::audio_play_failed,
                       ErrorFields::handle_sound_reason, entry.sound.source,
                       entry.handle.value, 0, reason);
                break;
            case EntryKind::stop:
                record(entry.sequence, ErrorApi::stop_sound,
                       ErrorCode::audio_stop_failed,
                       ErrorFields::handle_reason, {}, entry.handle.value, 0,
                       reason);
                break;
            case EntryKind::bus_volume:
                record(entry.sequence, ErrorApi::set_bus_volume,
                       ErrorCode::audio_bus_volume_failed,
                       ErrorFields::bus_reason, entry.resource, 0, 0, reason);
                break;
            case EntryKind::save_data:
            case EntryKind::save_settings:
                record(entry.sequence,
                       entry.kind == EntryKind::save_data
                           ? ErrorApi::save_data
                           : ErrorApi::save_settings,
                       ErrorCode::persistence_write_failed,
                       ErrorFields::path_reason,
                       entry.diagnostic_path,
                       0, 0, reason);
                break;
            }
        }
    }
}

namespace {
using EffectErrorApi = PreparedEffectTransaction::ErrorApi;
using EffectErrorCode = PreparedEffectTransaction::ErrorCode;

std::string_view errorApiName(EffectErrorApi api) noexcept {
    switch (api) {
    case EffectErrorApi::play_sound: return "playSound";
    case EffectErrorApi::stop_sound: return "stopSound";
    case EffectErrorApi::set_bus_volume: return "setBusVolume";
    case EffectErrorApi::bus_volume: return "busVolume";
    case EffectErrorApi::is_playing: return "isPlaying";
    case EffectErrorApi::game_settings: return "gameSettings";
    case EffectErrorApi::set_game_settings: return "setGameSettings";
    case EffectErrorApi::save_settings: return "saveSettings";
    case EffectErrorApi::save_data: return "saveData";
    case EffectErrorApi::load_data: return "loadData";
    case EffectErrorApi::list_saves: return "listSaves";
    case EffectErrorApi::load_scene: return "loadScene";
    case EffectErrorApi::set_local_transform: return "setLocalTransform";
    case EffectErrorApi::set_sprite_view: return "setSpriteView";
    case EffectErrorApi::set_sprite_texture: return "setSpriteTexture";
    case EffectErrorApi::effect_outbox: return "effect_outbox";
    case EffectErrorApi::set_sound_volume: return "setSoundVolume";
    case EffectErrorApi::set_sound_pitch: return "setSoundPitch";
    case EffectErrorApi::set_sound_paused: return "setSoundPaused";
    case EffectErrorApi::sound_state: return "soundState";
    case EffectErrorApi::raycast_all: return "raycastAll";
    case EffectErrorApi::raycast_closest: return "raycastClosest";
    case EffectErrorApi::overlap_all: return "overlapAll";
    case EffectErrorApi::shape_cast_all: return "shapeCastAll";
    case EffectErrorApi::shape_cast_closest: return "shapeCastClosest";
    }
    return "effect_outbox";
}

std::string_view errorCodeName(EffectErrorCode code) noexcept {
    switch (code) {
    case EffectErrorCode::scene_retired_object_mutation_rejected:
        return "SceneRetiredObjectMutationRejected";
    case EffectErrorCode::effect_outbox_full: return "effect_outbox_full@1";
    case EffectErrorCode::build_feature_disabled:
        return BuildFeatureCallDisabledError::error_code;
    case EffectErrorCode::audio_play_stage_failed:
        return "audio_play_stage_failed@1";
    case EffectErrorCode::audio_stop_stage_failed:
        return "audio_stop_stage_failed@1";
    case EffectErrorCode::audio_bus_volume_stage_failed:
        return "audio_bus_volume_stage_failed@1";
    case EffectErrorCode::audio_is_playing_stage_failed:
        return "audio_is_playing_stage_failed@1";
    case EffectErrorCode::persistence_shadow_failed:
        return "persistence_shadow_failed@1";
    case EffectErrorCode::persistence_stage_failed:
        return "persistence_stage_failed@1";
    case EffectErrorCode::persistence_overlay_failed:
        return "persistence_overlay_failed@1";
    case EffectErrorCode::persistence_read_failed:
        return "persistence_read_failed@1";
    case EffectErrorCode::scene_retire_load_rejected:
        return "scene_retire_load_rejected@1";
    case EffectErrorCode::audio_play_failed: return "audio_play_failed@1";
    case EffectErrorCode::audio_stop_failed: return "audio_stop_failed@1";
    case EffectErrorCode::audio_bus_volume_failed:
        return "audio_bus_volume_failed@1";
    case EffectErrorCode::persistence_write_failed:
        return "persistence_write_failed@1";
    case EffectErrorCode::physics_provider_unavailable:
        return phys::ProviderUnavailableError::error_code;
    }
    return "effect_failure@1";
}
} // namespace

std::vector<CommittedWithEffectFailure::EffectError>
PreparedEffectTransaction::effectErrors() const {
    std::array<const FixedEffectError *, sceneEffectCapacity + 1U> ordered{};
    std::size_t count = 0;
    for (std::size_t index = 0; index < error_count_; ++index) {
        ordered[count++] = &errors_[index];
    }
    if (overflow_reported_) ordered[count++] = &errors_.back();
    std::sort(ordered.begin(), ordered.begin() +
              static_cast<std::ptrdiff_t>(count),
              [](const auto *left, const auto *right) {
                  return left->sequence < right->sequence;
              });

    const auto apiName = errorApiName;
    const auto codeName = errorCodeName;
    const auto phaseName = [](CallbackPhase phase) -> std::string_view {
        switch (phase) {
        case CallbackPhase::activate: return "activate";
        case CallbackPhase::destroy: return "destroy";
        case CallbackPhase::flush: return "flush";
        }
        return "activate";
    };
    const auto text = [](const auto &value) {
        return std::string{value.bytes.data(), value.size};
    };

    std::vector<CommittedWithEffectFailure::EffectError> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto &fixed = *ordered[index];
        nlohmann::ordered_json fields = nlohmann::ordered_json::object();
        switch (fixed.fields) {
        case ErrorFields::reason:
            fields["reason"] = text(fixed.reason);
            break;
        case ErrorFields::sound_reason:
            fields["sound"] = text(fixed.primary);
            fields["reason"] = text(fixed.reason);
            break;
        case ErrorFields::handle_reason:
            fields["handle"] = fixed.value;
            fields["reason"] = text(fixed.reason);
            break;
        case ErrorFields::bus_reason:
            fields["bus"] = text(fixed.primary);
            fields["reason"] = text(fixed.reason);
            break;
        case ErrorFields::slot_reason:
            fields["slot"] = text(fixed.primary);
            fields["reason"] = text(fixed.reason);
            break;
        case ErrorFields::scene_reason:
            fields["scene"] = text(fixed.primary);
            fields["reason"] = text(fixed.reason);
            break;
        case ErrorFields::object_id:
            fields["object_id"] = std::to_string(fixed.value) + ":" +
                                  std::to_string(fixed.value2);
            break;
        case ErrorFields::path_reason:
            fields["path"] = text(fixed.primary);
            fields["reason"] = text(fixed.reason);
            break;
        case ErrorFields::handle_sound_reason:
            fields["handle"] = fixed.value;
            fields["sound"] = text(fixed.primary);
            fields["reason"] = text(fixed.reason);
            break;
        case ErrorFields::dropped:
            fields["dropped"] = fixed.value;
            break;
        }
        result.push_back(CommittedWithEffectFailure::EffectError{
            std::string{apiName(fixed.api)}, std::string{codeName(fixed.code)},
            std::move(fields), std::string{phaseName(fixed.phase)}});
    }
    return result;
}

PreparedEffectTransaction *activeEffectTransaction() noexcept {
    return active_effect_transaction;
}

EffectTransactionScope::EffectTransactionScope(
    PreparedEffectTransaction &transaction) noexcept
    : previous_{std::exchange(active_effect_transaction, &transaction)} {}

EffectTransactionScope::~EffectTransactionScope() {
    active_effect_transaction = previous_;
}

EffectDestroyScope::EffectDestroyScope() noexcept
    : transaction_{active_effect_transaction} {
    ++destroy_scope_depth;
    if (transaction_ != nullptr) {
        previous_ = transaction_->phase();
        transaction_->setPhase(
            PreparedEffectTransaction::CallbackPhase::destroy);
    }
}

EffectDestroyScope::~EffectDestroyScope() {
    if (transaction_ != nullptr) transaction_->setPhase(previous_);
    --destroy_scope_depth;
}

bool PreparedEffectTransaction::recordDestroyRefusal(
    ErrorApi api, ErrorCode code, ErrorFields fields, std::uint64_t value,
    std::string_view reason) noexcept {
    if (phase_ != CallbackPhase::destroy) return false;
    record(next_sequence_++, api, code, fields, {}, value, 0, reason);
    return true;
}

bool absorbDestroyRefusal(PreparedEffectTransaction::ErrorApi api,
                          PreparedEffectTransaction::ErrorCode code,
                          PreparedEffectTransaction::ErrorFields fields,
                          std::uint64_t value,
                          std::string_view reason) noexcept {
    if (auto *transaction = active_effect_transaction) {
        // A scene transaction decides by its own phase, as its staged calls
        // (playSound, loadData, ...) already do.
        return transaction->recordDestroyRefusal(api, code, fields, value,
                                                 reason);
    }
    if (destroy_scope_depth == 0) return false;
    // A destruction callback outside a scene load (an object removed during
    // play, a game DLL unloading) has no effect error list to carry the
    // refusal, so the log carries it.
    try {
        if (logger != nullptr) {
            LOG_ERROR(logger, "{} in a destruction callback: {} answered empty ({})",
                      std::string{errorCodeName(code)},
                      std::string{errorApiName(api)}, std::string{reason});
        }
    } catch (...) {
    }
    return true;
}

} // namespace Pelican::internal
