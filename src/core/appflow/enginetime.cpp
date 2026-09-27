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
    editor_delta_time = 0.0;
    frame_index = 0;
    evaluation_index = 0;
    time_set_revision = 0;
    pending_steps = 0;
    step_frame = false;
    last_tick = Clock::now();
}

void EngineTime::advance() {
    // The frame's own delta: what the game clock takes while playing, and
    // what the editor clock takes on every frame.
    if (current_mode == Mode::fixed_step) {
        editor_delta_time = fixed_delta_time;
    } else {
        const auto current_tick = Clock::now();
        const auto elapsed = std::chrono::duration<double>(current_tick - last_tick).count();
        editor_delta_time = std::clamp(elapsed, 0.0, maxRealtimeDelta);
        last_tick = current_tick;
    }
    ++evaluation_index;

    step_frame = false;
    if (play_state == PlayState::playing) {
        delta_time = editor_delta_time;
    } else if (pending_steps > 0) {
        --pending_steps;
        step_frame = true;
        delta_time = fixed_delta_time;
    } else {
        delta_time = 0.0;
    }
    if (gameStepping()) {
        current_time += delta_time;
        ++frame_index;
    }
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

uint64_t EngineTime::evaluationIndex() const { return evaluation_index; }

double EngineTime::editorDt() const { return editor_delta_time; }

bool EngineTime::gameStepping() const {
    return play_state == PlayState::playing || step_frame;
}

EngineTime::PlayState EngineTime::playState() const { return play_state; }

EngineTime::PlayState EngineTime::setPlayState(PlayState state) {
    const auto previous = play_state;
    if (state == previous) return previous;
    play_state = state;
    step_frame = false;
    if (previous == PlayState::paused) {
        pending_steps = 0;
        // The resume's first frame measures from here, not from the last
        // frame before it.
        last_tick = Clock::now();
    }
    return previous;
}

void EngineTime::requestSteps(uint64_t count) {
    if (play_state != PlayState::paused) {
        throw std::logic_error("EngineTime steps can be requested only while paused");
    }
    pending_steps += count;
}

uint64_t EngineTime::pendingSteps() const { return pending_steps; }

double EngineTime::stepDelta() const { return fixed_delta_time; }

} // namespace Pelican
