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
    session_started = false;
    time_before_session = 0.0;
    last_tick = Clock::now();
    last_audio_time.reset();
    following_audio_clock = false;
    audio_carry = 0.0;
}

void EngineTime::advance() { advanceAt(Clock::now()); }

void EngineTime::advanceAt(Clock::time_point tick) {
    // The frame's own delta: what the game clock takes while playing, and
    // what the editor clock takes on every frame.
    if (current_mode == Mode::fixed_step) {
        editor_delta_time = fixed_delta_time;
    } else {
        const auto elapsed = std::chrono::duration<double>(tick - last_tick).count();
        editor_delta_time = std::clamp(elapsed, 0.0, maxRealtimeDelta);
        last_tick = tick;
    }
    ++evaluation_index;
    // AT-1 decision 5: the audio clock at this tick, read every frame so the
    // step after a master starts has the reading before it.
    const auto audio = audio_clock_hook ? audio_clock_hook(audio_clock_context, tick) : std::nullopt;
    const auto previous_audio_time = last_audio_time;
    last_audio_time = audio ? std::optional{audio->audio_time} : std::nullopt;

    step_frame = false;
    following_audio_clock = false;
    if (play_state == PlayState::playing) {
        delta_time = editor_delta_time;
        // A master plays: this step is the audio clock's move since the last
        // tick, under the same bound as the steady clock's. What the bound
        // holds back after a hitch is carried into the next steps, each under
        // the bound again, so the game clock catches up with the sound within
        // a few frames instead of staying behind it.
        if (current_mode == Mode::realtime && audio && audio->master_playing && previous_audio_time) {
            const auto owed = audio->audio_time - *previous_audio_time + audio_carry;
            delta_time = std::clamp(owed, 0.0, maxRealtimeDelta);
            audio_carry = (std::max)(owed - delta_time, 0.0);
            following_audio_clock = true;
        }
    } else if (pending_steps > 0) {
        --pending_steps;
        step_frame = true;
        delta_time = fixed_delta_time;
    } else {
        delta_time = 0.0;
    }
    // A frame no master plays in owes the sound nothing.
    if (!following_audio_clock) audio_carry = 0.0;
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
    // An explicit time is where the timeline stands; the master is sought to
    // it (a jump), so nothing is owed from before.
    audio_carry = 0.0;
}

bool EngineTime::installAudioClock(void *context, AudioClockHook hook) noexcept {
    if (context == nullptr || hook == nullptr || audio_clock_hook != nullptr) return false;
    audio_clock_context = context;
    audio_clock_hook = hook;
    last_audio_time.reset();
    audio_carry = 0.0;
    return true;
}

void EngineTime::removeAudioClock(void *context) noexcept {
    if (context != audio_clock_context) return;
    audio_clock_context = nullptr;
    audio_clock_hook = nullptr;
    last_audio_time.reset();
    following_audio_clock = false;
    audio_carry = 0.0;
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
    if (previous != PlayState::playing) {
        pending_steps = 0;
        // The resume's first frame measures from here, not from the last
        // frame before it.
        last_tick = Clock::now();
    }
    return previous;
}

void EngineTime::beginGameSession() {
    const auto before = current_time;
    setTime(0.0);
    frame_index = 0;
    session_started = true;
    time_before_session = before;
}

void EngineTime::endGameSession() {
    setTime(session_started ? time_before_session : 0.0);
    session_started = false;
    time_before_session = 0.0;
}

EngineTime::GameClock EngineTime::gameClock() const {
    return GameClock{
        .time = current_time,
        .frame = frame_index,
        .session_started = session_started,
        .time_before_session = time_before_session,
    };
}

void EngineTime::restoreGameClock(const GameClock &clock) {
    setTime(clock.time);
    frame_index = clock.frame;
    session_started = clock.session_started;
    time_before_session = clock.time_before_session;
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
