#pragma once

#include "../container.hpp"

#include <chrono>
#include <cstdint>

namespace Pelican {

// PM-1a. Two numbers and two clocks (docs/design_play_mode.md section 3).
//
//  - The evaluation index goes up by one on every drawn frame, paused or not,
//    and never goes back (short of setup()). It is the revision the animation
//    writers, graphs and phases compare, the in-flight frame index of the GPU
//    resources, the GPU timing / TAA / shader frame index, the graph publish,
//    picking, RenderDoc and the XR mirror: everything that means "which
//    evaluation is this".
//  - The game frame (frameIndex()) and the game clock (now / dt /
//    timeSetRevision) move only on a game step: every frame while playing,
//    and one step per frame for the steps advance_frames asked for while
//    paused. While paused, dt is 0 and now stays where it stopped, so the
//    animation, seq, graph and VAT are evaluated again at the stopped time.
//  - The editor clock is the frame's own delta (the realtime measurement, or
//    the fixed step) and the evaluation index. The engine's editor systems
//    read its delta through GameContext; GameContext::frameIndex() is the
//    evaluation index for game code too (the animation ABI's revision).
//
// While playing, the evaluation index and the game frame are the same number,
// and the game dt is the editor dt: a run that never pauses is the run it was
// before the two were separated.
DECLARE_MODULE(EngineTime) {
  public:
    enum class Mode {
        realtime,
        fixed_step,
    };
    enum class PlayState {
        playing,
        paused,
    };

  private:
    using Clock = std::chrono::steady_clock;

    Mode current_mode = Mode::realtime;
    double fixed_delta_time = 1.0 / 60.0;
    double current_time = 0.0;
    double delta_time = 0.0;
    double editor_delta_time = 0.0;
    uint64_t frame_index = 0;
    uint64_t evaluation_index = 0;
    uint64_t time_set_revision = 0;
    PlayState play_state = PlayState::playing;
    uint64_t pending_steps = 0;
    // Whether the frame the last advance() opened took a requested step.
    bool step_frame = false;
    Clock::time_point last_tick;

  public:
    EngineTime();

    // Resets both numbers and the game clock. The play state is kept; steps
    // asked for and not yet taken are dropped.
    void setup(Mode mode, double fixed_dt);
    // One drawn frame: the evaluation index goes up by one; the game frame and
    // the game clock move when this frame is a game step.
    void advance();
    void setTime(double t);
    double now() const;
    // The game clock's delta for this frame: 0 on a frame that is not a game
    // step.
    double dt() const;
    // The game frame: counts game steps.
    uint64_t frameIndex() const;
    uint64_t timeSetRevision() const;

    uint64_t evaluationIndex() const;
    double editorDt() const;
    // Whether the current frame is a game step: always while playing, and
    // while paused on a frame advance() opened with a requested step.
    bool gameStepping() const;

    PlayState playState() const;
    // Returns the state before the change. Moving from paused to playing sets
    // the realtime reference tick to now, so the first frame after a resume
    // does not measure the time spent paused. Leaving playing drops nothing;
    // leaving paused drops the steps not yet taken.
    PlayState setPlayState(PlayState state);
    // Paused only: `count` more game steps, taken one per advance() from the
    // next one on, each with the step delta (1 / fps).
    void requestSteps(uint64_t count);
    uint64_t pendingSteps() const;
    double stepDelta() const;
};

} // namespace Pelican
