#include "effecttransaction.hpp"

#include "scene.hpp"
#include "sceneloadfault.hpp"
#include "../build_features.hpp"
#include "../ecs/archetypemigration.hpp"
#include <details/ecs/componentdeclare.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <stdexcept>
#include <utility>

namespace Pelican::internal {
namespace {
thread_local PreparedEffectTransaction *active_effect_transaction = nullptr;

std::string exceptionText() noexcept {
    try {
        throw;
    } catch (const std::exception &error) {
        return error.what();
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
    errors_.reserve(sceneEffectCapacity + 1U);
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
            return entry.active && entry.kind == kind &&
                   entry.resource == resource;
        });
    return found == entries_.end() ? nullptr : &*found;
}

const PreparedEffectTransaction::Entry *
PreparedEffectTransaction::findPlay(SoundHandle handle) const noexcept {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
        [&](const Entry &entry) {
            return entry.active && entry.kind == EntryKind::play &&
                   entry.handle == handle;
        });
    return found == entries_.end() ? nullptr : &*found;
}

PreparedEffectTransaction::Entry *
PreparedEffectTransaction::findPlay(SoundHandle handle) noexcept {
    return const_cast<Entry *>(std::as_const(*this).findPlay(handle));
}

void PreparedEffectTransaction::record(
    std::string_view api, std::string_view code,
    nlohmann::ordered_json fields) noexcept {
    try {
        if (errors_.size() >= sceneEffectCapacity + 1U) return;
        const auto phase = phase_ == CallbackPhase::destroy
            ? "destroy"
            : phase_ == CallbackPhase::flush ? "flush" : "activate";
        errors_.push_back(CommittedWithEffectFailure::EffectError{
            std::string{api}, std::string{code}, std::move(fields), phase});
    } catch (...) {
        // The report storage was reserved in prepare. If construction of a
        // diagnostic field itself fails, preserve process liveness at the
        // noexcept destroy/commit boundary.
    }
}

bool PreparedEffectTransaction::reserveEntry() noexcept {
    if (entries_.size() < sceneEffectCapacity) return true;
    ++dropped_entries_;
    if (!overflow_reported_) {
        overflow_reported_ = true;
        const auto before = errors_.size();
        record("effect_outbox", "effect_outbox_full@1",
               {{"dropped", dropped_entries_}});
        if (errors_.size() != before) overflow_error_index_ = before;
    } else if (overflow_error_index_ < errors_.size()) {
        try {
            errors_[overflow_error_index_].error_fields["dropped"] =
                dropped_entries_;
        } catch (...) {
            // The first fixed diagnostic remains valid if its scalar update
            // cannot be represented at the noexcept callback boundary.
        }
    }
    return false;
}

SoundHandle PreparedEffectTransaction::playSound(std::string_view path) {
    try {
#if PELICAN_WITH_AUDIO
        auto sound = audio_->prepareSound(path);
        const auto handle = audio_->reserveSoundHandle();
        if (!reserveEntry()) return SoundHandle{};
        entries_.push_back(Entry{
            .kind = EntryKind::play,
            .sequence = next_sequence_++,
            .handle = handle,
            .resource = std::to_string(handle.value),
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
        throwBuildFeatureDisabled("PELICAN_WITH_AUDIO",
                                  "playSound is unavailable");
#endif
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record("playSound",
#if PELICAN_WITH_AUDIO
               "audio_play_stage_failed@1",
#else
               "build_feature_disabled@1",
#endif
               {{"sound", std::string{path}}, {"reason", exceptionText()}});
        return SoundHandle{};
    }
}

void PreparedEffectTransaction::stopSound(SoundHandle handle) {
    try {
#if PELICAN_WITH_AUDIO
        if (auto *play = findPlay(handle)) {
            play->active = false;
#if PELICAN_WITH_AUDIO
            audio_->cancelPreparedVoice(handle);
#endif
            return;
        }
        if (!audio_->isPlaying(handle)) return;
        const auto resource = std::to_string(handle.value);
        if (find(EntryKind::stop, resource) != nullptr) return;
        if (!reserveEntry()) return;
        entries_.push_back(Entry{.kind = EntryKind::stop,
                                 .sequence = next_sequence_++,
                                 .handle = handle,
                                 .resource = resource});
#else
        (void)handle;
        throwBuildFeatureDisabled("PELICAN_WITH_AUDIO",
                                  "stopSound is unavailable");
#endif
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record("stopSound",
#if PELICAN_WITH_AUDIO
               "audio_stop_stage_failed@1",
#else
               "build_feature_disabled@1",
#endif
               {{"handle", handle.value}, {"reason", exceptionText()}});
    }
}

void PreparedEffectTransaction::setBusVolume(std::string_view bus,
                                              float volume) {
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
            entry->sequence = next_sequence_++;
            return;
        }
        if (!reserveEntry()) return;
        entries_.push_back(Entry{.kind = EntryKind::bus_volume,
                                 .sequence = next_sequence_++,
                                 .resource = std::string{bus},
                                 .volume = volume});
#else
        (void)bus;
        (void)volume;
        throwBuildFeatureDisabled("PELICAN_WITH_AUDIO",
                                  "setBusVolume is unavailable");
#endif
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record("setBusVolume",
#if PELICAN_WITH_AUDIO
               "audio_bus_volume_stage_failed@1",
#else
               "build_feature_disabled@1",
#endif
               {{"bus", std::string{bus}}, {"reason", exceptionText()}});
    }
}

float PreparedEffectTransaction::busVolume(std::string_view bus) {
    try {
#if PELICAN_WITH_AUDIO
        (void)audioBusFromName(bus);
        return busValue(audio_shadow_, bus);
#else
        (void)bus;
        throwBuildFeatureDisabled("PELICAN_WITH_AUDIO",
                                  "busVolume is unavailable");
#endif
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record("busVolume",
#if PELICAN_WITH_AUDIO
               "audio_bus_volume_stage_failed@1",
#else
               "build_feature_disabled@1",
#endif
               {{"bus", std::string{bus}}, {"reason", exceptionText()}});
        return 0.0F;
    }
}

bool PreparedEffectTransaction::isPlaying(SoundHandle handle) {
    try {
#if PELICAN_WITH_AUDIO
        if (findPlay(handle) != nullptr) return true;
        const auto resource = std::to_string(handle.value);
        if (find(EntryKind::stop, resource) != nullptr) return false;
        return audio_->isPlaying(handle);
#else
        (void)handle;
        throwBuildFeatureDisabled("PELICAN_WITH_AUDIO",
                                  "isPlaying is unavailable");
#endif
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record("isPlaying",
#if PELICAN_WITH_AUDIO
               "audio_is_playing_stage_failed@1",
#else
               "build_feature_disabled@1",
#endif
               {{"handle", handle.value}, {"reason", exceptionText()}});
        return false;
    }
}

nlohmann::json PreparedEffectTransaction::gameSettings() {
    try {
        return game_settings_shadow_;
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record("gameSettings", "persistence_shadow_failed@1",
               {{"reason", exceptionText()}});
        return nlohmann::json::object();
    }
}

void PreparedEffectTransaction::setGameSettings(nlohmann::json settings) {
    try {
        game_settings_shadow_ = std::move(settings);
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record("setGameSettings", "persistence_shadow_failed@1",
               {{"reason", exceptionText()}});
    }
}

void PreparedEffectTransaction::saveSettings() {
    try {
        auto write = persistence_->prepareSettingsWrite(
            game_settings_shadow_, audio_shadow_);
        auto *entry = find(EntryKind::save_settings, write.resource_key);
        if (entry != nullptr) {
            entry->write = std::move(write);
            entry->sequence = next_sequence_++;
            return;
        }
        if (!reserveEntry()) return;
        entries_.push_back(Entry{.kind = EntryKind::save_settings,
                                 .sequence = next_sequence_++,
                                 .resource = write.resource_key,
                                 .write = std::move(write)});
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record("saveSettings", "persistence_stage_failed@1",
               {{"reason", exceptionText()}});
    }
}

void PreparedEffectTransaction::saveData(
    std::string_view slot, const nlohmann::json &data) {
    try {
        auto write = persistence_->prepareDataWrite(slot, data);
        auto *entry = find(EntryKind::save_data, write.resource_key);
        if (entry != nullptr) {
            entry->write = std::move(write);
            entry->sequence = next_sequence_++;
            return;
        }
        if (!reserveEntry()) return;
        entries_.push_back(Entry{.kind = EntryKind::save_data,
                                 .sequence = next_sequence_++,
                                 .resource = write.resource_key,
                                 .write = std::move(write)});
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record("saveData", "persistence_stage_failed@1",
               {{"slot", std::string{slot}}, {"reason", exceptionText()}});
    }
}

std::optional<nlohmann::json> PreparedEffectTransaction::loadData(
    std::string_view slot) {
    try {
        const auto resource = "save:" + std::string{slot};
        const auto found = std::find_if(entries_.begin(), entries_.end(),
            [&](const Entry &entry) {
                return entry.active && entry.kind == EntryKind::save_data &&
                       entry.resource == resource;
            });
        if (found != entries_.end() && found->write &&
            found->write->overlay_data) return *found->write->overlay_data;
        return persistence_->loadData(slot);
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record("loadData", "persistence_overlay_failed@1",
               {{"slot", std::string{slot}}, {"reason", exceptionText()}});
        return std::nullopt;
    }
}

std::vector<PersistenceSlotInfo> PreparedEffectTransaction::listSaves() {
    try {
        auto result = persistence_->listSaves();
        for (const auto &entry : entries_) {
            if (!entry.active || entry.kind != EntryKind::save_data ||
                !entry.write || !entry.write->slot) continue;
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
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record("listSaves", "persistence_overlay_failed@1",
               {{"reason", exceptionText()}});
        return {};
    }
}

void PreparedEffectTransaction::loadScene(std::string_view name) {
    try {
        if (phase_ == CallbackPhase::destroy) {
            loader_->requestLoadDuringRetire(std::string{name});
        } else {
            loader_->requestLoad(std::string{name});
        }
    } catch (...) {
        if (phase_ != CallbackPhase::destroy) throw;
        record("loadScene", "scene_retire_load_rejected@1",
               {{"scene", std::string{name}}, {"reason", exceptionText()}});
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
    record(api, "SceneRetiredObjectMutationRejected",
           {{"object_id", toString(id)}});
    return true;
}

std::vector<CommittedWithEffectFailure::EffectError>
PreparedEffectTransaction::flush(SceneLoadFaultInjector *injector,
                                 SceneLoadPhaseTrace &phase_trace) noexcept {
    (void)phase_trace; // EventOutbox owns the executor's single flush marker.
    phase_ = CallbackPhase::flush;
    persistence_->commitGameSettings(std::move(game_settings_shadow_));

    for (std::uint64_t sequence = 0; sequence < next_sequence_; ++sequence) {
        auto found = std::find_if(entries_.begin(), entries_.end(),
            [&](const Entry &entry) {
                return entry.active && entry.sequence == sequence;
            });
        if (found == entries_.end()) continue;
        auto &entry = *found;
        try {
            switch (entry.kind) {
            case EntryKind::play:
#if PELICAN_WITH_AUDIO
                if (sceneLoadMutationEnabled(
                        injector, SceneLoadFaultPhase::flush, "audio",
                        scene_load_mutation::effect_audio_play)) {
                    throw std::runtime_error("injected audio play failure");
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
                    throw std::runtime_error(
                        "injected audio bus volume failure");
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
                    throw std::runtime_error("injected persistence write failure");
                }
                persistence_->commitPreparedWrite(*entry.write);
                break;
            }
            }
        } catch (...) {
            switch (entry.kind) {
            case EntryKind::play:
                record("playSound", "audio_play_failed@1",
                       {{"handle", entry.handle.value},
                        {"sound", entry.sound.source},
                        {"reason", exceptionText()}});
                break;
            case EntryKind::stop:
                record("stopSound", "audio_stop_failed@1",
                       {{"handle", entry.handle.value},
                        {"reason", exceptionText()}});
                break;
            case EntryKind::bus_volume:
                record("setBusVolume", "audio_bus_volume_failed@1",
                       {{"bus", entry.resource},
                        {"reason", exceptionText()}});
                break;
            case EntryKind::save_data:
                record("saveData", "persistence_write_failed@1",
                       {{"path", entry.write->path.string()},
                        {"reason", exceptionText()}});
                break;
            case EntryKind::save_settings:
                record("saveSettings", "persistence_write_failed@1",
                       {{"path", entry.write->path.string()},
                        {"reason", exceptionText()}});
                break;
            }
        }
    }
    return std::move(errors_);
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
    if (transaction_ != nullptr) {
        previous_ = transaction_->phase();
        transaction_->setPhase(
            PreparedEffectTransaction::CallbackPhase::destroy);
    }
}

EffectDestroyScope::~EffectDestroyScope() {
    if (transaction_ != nullptr) transaction_->setPhase(previous_);
}

} // namespace Pelican::internal
