#pragma once

#include "../container.hpp"
#include "../handle.hpp"

#include <array>
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

  private:
    struct Voice {
        AudioBus bus = AudioBus::Se;
        std::uint64_t backend_id = 0;
    };

    std::unique_ptr<Backend> backend;
    std::unordered_map<SoundHandle, Voice, SoundHandle::Hash> voices;
    std::array<float, 3> bus_volumes{1.0f, 1.0f, 1.0f};
    std::uint64_t next_handle = 1;

    float effectiveVolume(AudioBus bus) const;
    void updateVoiceVolumes();

  public:
    Audio();
    ~Audio();

    SoundHandle playSound(std::string_view path);
    void stopSound(SoundHandle handle);
    void setBusVolume(std::string_view bus, float volume);
    bool isPlaying(SoundHandle handle) const;
};

} // namespace Pelican
