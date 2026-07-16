#pragma once

#include "../container.hpp"

#include <chrono>
#include <cstdint>

namespace Pelican {

DECLARE_MODULE(EngineTime) {
  public:
    enum class Mode {
        realtime,
        fixed_step,
    };

  private:
    using Clock = std::chrono::steady_clock;

    Mode current_mode = Mode::realtime;
    double fixed_delta_time = 1.0 / 60.0;
    double current_time = 0.0;
    double delta_time = 0.0;
    uint64_t frame_index = 0;
    uint64_t time_set_revision = 0;
    Clock::time_point last_tick;

  public:
    EngineTime();

    void setup(Mode mode, double fixed_dt);
    void advance();
    void setTime(double t);
    double now() const;
    double dt() const;
    uint64_t frameIndex() const;
    uint64_t timeSetRevision() const;
};

} // namespace Pelican
