#pragma once

#include "../container.hpp"
#include "../handle.hpp"
#include "../userpublic/audio.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>

namespace Pelican {

enum class AudioBus : std::uint8_t {
    Master = 0,
    Bgm = 1,
    Se = 2,
};

AudioBus audioBusFromName(std::string_view bus);

DECLARE_MODULE(Audio) {
  public:
    class Backend;
    struct PreparedSound {
        std::shared_ptr<const void> decoded;
        std::string source;
    };

  private:
    struct Voice {
        AudioBus bus = AudioBus::Se;
        std::uint64_t backend_id = 0;
        float volume = 1.0f;
    };

    std::unique_ptr<Backend> backend;
    std::unordered_map<SoundHandle, Voice, SoundHandle::Hash> voices;
    std::array<float, 3> bus_volumes{1.0f, 1.0f, 1.0f};
    bool voice_volumes_dirty = false;
    std::uint64_t next_handle = 1;

    float effectiveVolume(AudioBus bus) const;
    void updateVoiceVolumes();

  public:
    Audio();
    ~Audio();

    SoundHandle playSound(std::string_view path);
    void stopSound(SoundHandle handle);
    void setBusVolume(std::string_view bus, float volume);
    float busVolume(std::string_view bus) const;
    bool isPlaying(SoundHandle handle) const;
    bool setSoundVolume(SoundHandle handle, float volume);
    bool setSoundPitch(SoundHandle handle, float pitch);
    std::optional<SoundPlaybackState> soundState(SoundHandle handle) const;

    // Effect-transaction seam. Decoding and all engine-side capacity are
    // prepared before commit; backend calls remain in the fallible flush.
    PreparedSound prepareSound(std::string_view path) const;
    SoundHandle reserveSoundHandle();
    void reserveEffectVoices(std::size_t count);
    void prepareVoiceSlot(SoundHandle handle);
    void cancelPreparedVoice(SoundHandle handle) noexcept;
    void commitPreparedPlay(SoundHandle handle, const PreparedSound &sound);
    void commitPreparedStop(SoundHandle handle,
                            bool inject_backend_failure = false);
    void publishBusVolume(std::string_view bus, float volume) noexcept;
    void applyBusVolume();
    void commitBusVolume(std::string_view bus, float volume);
    std::uint64_t nextHandleForTesting() const noexcept { return next_handle; }

    // Called at the shared logical-frame boundary. Completed backend voices
    // are destroyed here rather than accumulating until Audio teardown.
    void update();

    std::size_t voiceCountForTesting() const noexcept;
    std::size_t backendVoiceCountForTesting() const noexcept;
};

} // namespace Pelican
