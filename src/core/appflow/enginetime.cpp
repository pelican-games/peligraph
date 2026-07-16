#include "enginetime.hpp"

#include <algorithm>
#include <stdexcept>

namespace Pelican {

namespace {

constexpr double maxRealtimeDelta = 0.1;

} // namespace

EngineTime::EngineTime() : last_tick{Clock::now()} {}

void EngineTime::setup(Mode mode, double fixed_dt) {
    if (fixed_dt <= 0.0) {
        throw std::runtime_error("EngineTime fixed dt must be positive");
    }

    current_mode = mode;
    fixed_delta_time = fixed_dt;
    current_time = 0.0;
    delta_time = 0.0;
    frame_index = 0;
    time_set_revision = 0;
    last_tick = Clock::now();
}

void EngineTime::advance() {
    if (current_mode == Mode::fixed_step) {
        delta_time = fixed_delta_time;
    } else {
        const auto current_tick = Clock::now();
        const auto elapsed = std::chrono::duration<double>(current_tick - last_tick).count();
        delta_time = std::clamp(elapsed, 0.0, maxRealtimeDelta);
        last_tick = current_tick;
    }

    current_time += delta_time;
    ++frame_index;
}

void EngineTime::setTime(double t) {
    current_time = t;
    delta_time = 0.0;
    last_tick = Clock::now();
    ++time_set_revision;
}

double EngineTime::now() const { return current_time; }

double EngineTime::dt() const { return delta_time; }

uint64_t EngineTime::frameIndex() const { return frame_index; }

uint64_t EngineTime::timeSetRevision() const { return time_set_revision; }

} // namespace Pelican
