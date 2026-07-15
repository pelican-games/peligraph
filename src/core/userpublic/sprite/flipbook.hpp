#pragma once

#include "../export.hpp"
#include <details/ecs/entity.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

class GameContext;

namespace sprite {

enum class FlipbookPlayback {
    once,
    loop,
};

struct FlipbookFrame {
    std::string texture;
    double duration_seconds = 0.0;
};

// A deliberately unprivileged standard-library helper. It owns no ECS system,
// clock, renderer state, or asset privilege: callers provide deterministic
// local time and apply the sampled texture through the public GameContext API.
class PELICAN_API FlipbookClip {
    struct State;
    State *state_;
    static State *createState(std::vector<FlipbookFrame> frames,
                              FlipbookPlayback playback);

  public:
    FlipbookClip(std::vector<FlipbookFrame> frames,
                 FlipbookPlayback playback = FlipbookPlayback::loop);
    ~FlipbookClip();
    FlipbookClip(FlipbookClip &&) noexcept;
    FlipbookClip &operator=(FlipbookClip &&) noexcept;
    FlipbookClip(const FlipbookClip &) = delete;
    FlipbookClip &operator=(const FlipbookClip &) = delete;

    std::size_t frameIndex(double local_time_seconds) const;
    std::string_view texture(double local_time_seconds) const;
    bool apply(GameContext &context, GameObjectId object, double local_time_seconds) const;

    double duration() const noexcept;
    std::size_t frameCount() const noexcept;
    FlipbookPlayback playback() const noexcept;
};

} // namespace sprite
} // namespace Pelican
