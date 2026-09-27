#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {

// PM-3a / PM-3b. The game's play state as the engine reports it (PM-1a,
// PM-1b, PM-2; docs/design_play_mode.md sections 2, 3, 6, 7, 8 and 12).
// Studio starts the engine in edit (--start-state edit): the scene is edited
// with the game asleep, Play starts a play session, Stop ends it and the
// engine puts the scene back as it was, except for the edits marked Keep.
enum class PlayState : std::uint8_t {
    edit,
    playing,
    paused,
};

// The camera the engine draws from (PM-1b): the editor camera, or the game's.
enum class ViewCamera : std::uint8_t {
    editor,
    game,
};

// One entry of get_status.play.recorded_edits (PM-2): a scene edit committed
// during the session, which Stop takes back unless it is kept.
struct RecordedEdit {
    std::string transaction_id;
    std::uint64_t actor = 0;

    bool operator==(const RecordedEdit &) const = default;
};

// get_status.play. `game_frame` counts game steps; it is not get_status.frame,
// which is the evaluation number and goes on rising while the game is paused.
// `view_camera`, `session` and `recorded_edits` are PM-1b's and PM-2's; an
// engine from before them leaves them out (no view, no session).
struct PlayStatus {
    PlayState state = PlayState::playing;
    std::uint64_t game_frame = 0;
    double game_time = 0.0;
    std::uint64_t pending_steps = 0;
    std::optional<ViewCamera> view_camera;
    bool session = false;
    std::vector<RecordedEdit> recorded_edits;

    bool operator==(const PlayStatus &) const = default;
};

// The `play` member of a get_status result. Empty when there is none -- an
// engine from before PM-1a -- or when it is not the shape the engine writes.
[[nodiscard]] std::optional<PlayStatus> parsePlayStatus(const nlohmann::json &status);

// The play refusals of section 10, and the game logic reload refusals Play
// and Reload game logic meet, one sentence each: "<sentence> (<code>)."
// `error` is the refusal's code and message with its data's fields beside
// them ({"code":..., "message":..., "state":..., "required":...}). Empty for
// any other refusal. knownRefusalDescription answers these too.
[[nodiscard]] std::string playRefusalDescription(const nlohmann::json &error);

// PM-3b. Why an edit marked Keep could not be applied again after Stop
// (set_play_state's not_kept[].code, an editor error name), as a sentence.
[[nodiscard]] std::string notKeptReason(std::string_view code);

struct PlayRpcRequest {
    std::uint64_t request_id = 0;
    std::string method;
    nlohmann::json params;
};

// PM-3b. A scene edit Studio's own session committed, as the panel that sent
// it knows it. The Play changes panel names a recorded edit by this.
enum class CommittedEditKind : std::uint8_t {
    edit,
    undo,
    redo,
};

struct CommittedEdit {
    std::string transaction_id;
    // What the panel called it ("Edit light.intensity"); for an undo or a
    // redo, the edit it took back or put back is looked up instead.
    std::string label;
    CommittedEditKind kind = CommittedEditKind::edit;
    // An undo's or a redo's source_transaction_id: the edit an undo takes
    // back, the undo a redo takes back.
    std::string source_transaction_id;
    // Marked Keep as soon as it lands (Take from game).
    bool keep = false;
};

// One row of the Play changes panel.
struct PlayChangeRow {
    std::string transaction_id;
    std::uint64_t actor = 0;
    // "Edit light.intensity", "Undo Edit light.intensity", or for an edit
    // Studio did not send, "Actor 7 · <transaction id>".
    std::string text;
    bool keep = false;
    // Studio sent it (its name is known).
    bool studio = false;
};

// What the last Stop did, for the status line and the panel.
struct StopOutcome {
    std::size_t reverted = 0;
    std::size_t kept = 0;
    struct NotKept {
        std::string transaction_id;
        std::string text;
        std::string code;
        std::string reason;
    };
    std::vector<NotKept> not_kept;
};

// What the play buttons, the play line of the status bar and the Play
// changes panel show, and what they send.
//
// Studio reads get_status only when something asks (Q-2 stopped the idle
// clocks), so the reading here moves with every get_status reply any panel
// receives (observeStatus), with one read after each change of state and
// after each edit Studio commits during a session, and with a few follow-up
// reads while a step is still to be taken. No clock of its own reads while
// nothing changes.
class PlayStateModel {
  public:
    // How many follow-up reads one change may take before it gives up on the
    // steps it is waiting for (about two seconds at the view's interval).
    static constexpr int FollowReadLimit = 40;

    // What play_busy turned back, to send again once the scene is idle.
    enum class Action : std::uint8_t { None, Play, Stop, Step };

    // The engine answers editor requests. A reading already in hand (the
    // readiness probe's) is kept; without one, get_status is asked once.
    void connect();
    // The engine went away or is being restarted: nothing it said holds for
    // the next one, and nothing marked Keep outlives the session it was in.
    void disconnect();

    // A get_status result, whoever asked for it. True when it carried play.
    bool observeStatus(const nlohmann::json &status);

    // ▶ Play in edit (a session begins, playing) or ■ Stop while playing or
    // paused (the session ends; keep = the rows marked Keep). Ctrl+P.
    bool playOrStop();
    // ▶ Play: edit or paused -> playing.
    bool play();
    // ■ Stop: set_play_state {state: edit, keep}. With a row marked Keep, the
    // recorded edits are read first, so an edit undone since is not named.
    bool stop();
    // ❚❚ Pause, pressed or released: set_play_state {state: paused|playing}.
    // Not in edit. False, and nothing queued, when not pressable now.
    bool togglePause();
    // ▶| Step: advance_frames {count: 1} while paused; from edit, first
    // set_play_state {state: paused} (the session begins on frame 0), then
    // the step.
    bool step();
    // View: Game | Editor while paused or in edit (set_view_camera).
    bool setViewCamera(ViewCamera camera);
    // Reload game logic (reload_game_logic), in any state.
    bool reloadGameLogic();

    // play_busy turned an action back; it can be sent again once the scene
    // edit or preview in flight has finished.
    [[nodiscard]] Action busyAction() const noexcept { return busy_action_; }
    // "Send it when done": the action waits for retryWhenIdle.
    void armRetry();
    [[nodiscard]] bool retryArmed() const noexcept { return retry_armed_; }
    // The scene is idle now: send the waiting action again. False when none
    // waits or it cannot be sent now.
    bool retryWhenIdle();

    // PM-3b. An edit Studio's session committed. During a session this reads
    // the recorded edits again, so the panel follows it.
    void noteCommittedEdit(const CommittedEdit &edit);
    // Keep marks. Only rows of the session count.
    void setKeep(const std::string &transaction_id, bool keep);
    void keepAll();
    void keepNone();
    [[nodiscard]] bool kept(const std::string &transaction_id) const {
        return keep_.contains(transaction_id);
    }
    [[nodiscard]] std::vector<PlayChangeRow> changeRows() const;
    // The name the panel gives a transaction ("" when Studio did not send
    // it).
    [[nodiscard]] std::string labelOf(const std::string &transaction_id) const;
    [[nodiscard]] const std::optional<StopOutcome> &lastStop() const noexcept { return last_stop_; }
    // Moves once per Stop the engine answered.
    [[nodiscard]] std::uint64_t stopSerial() const noexcept { return stop_serial_; }

    [[nodiscard]] std::vector<PlayRpcRequest> takeRequests();
    [[nodiscard]] bool owns(std::uint64_t request_id) const noexcept;
    void receiveResult(std::uint64_t request_id, const nlohmann::json &result);
    // The engine refused: `error` as playRefusalDescription reads it.
    void receiveRefusal(std::uint64_t request_id, const nlohmann::json &error);
    // The request never got an answer (transport), or was refused with no
    // data to read.
    void receiveFailure(std::uint64_t request_id, std::string message);

    // A follow-up get_status is wanted: a step has not landed yet, and no
    // read is in flight.
    [[nodiscard]] bool followWanted() const noexcept;
    // Queues that read. False when none is wanted.
    bool follow();

    // The engine reports play (PM-1a or later). Without it there are no
    // buttons at all.
    [[nodiscard]] bool supported() const noexcept { return status_.has_value(); }
    [[nodiscard]] bool connected() const noexcept { return connected_; }
    [[nodiscard]] const std::optional<PlayStatus> &status() const noexcept { return status_; }
    [[nodiscard]] bool inState(PlayState state) const noexcept {
        return status_ && status_->state == state;
    }
    [[nodiscard]] bool editing() const noexcept { return inState(PlayState::edit); }
    [[nodiscard]] bool playing() const noexcept { return inState(PlayState::playing); }
    [[nodiscard]] bool paused() const noexcept { return inState(PlayState::paused); }
    // A play session is running (get_status.play.session): Save, Load and
    // Import wait for Stop, and the scene edits made now go back on Stop.
    [[nodiscard]] bool session() const noexcept { return status_ && status_->session; }
    // A set_play_state, advance_frames, set_view_camera or reload is waiting
    // for its answer.
    [[nodiscard]] bool busy() const noexcept { return change_request_ != 0; }
    [[nodiscard]] bool canPlayOrStop() const noexcept {
        return connected_ && supported() && !busy();
    }
    [[nodiscard]] bool canTogglePause() const noexcept {
        return canPlayOrStop() && !editing();
    }
    [[nodiscard]] bool canStep() const noexcept {
        return canPlayOrStop() && (paused() || editing());
    }
    // The view can be chosen: paused or in edit, on an engine that has one.
    [[nodiscard]] bool viewChoosable() const noexcept {
        return status_ && status_->view_camera && status_->state != PlayState::playing;
    }
    [[nodiscard]] bool canSetView() const noexcept { return canPlayOrStop() && viewChoosable(); }
    [[nodiscard]] bool canReload() const noexcept { return canPlayOrStop(); }

    // "Edit", "Playing · frame N", "Paused · frame N", "Stepping (k left) ·
    // frame N"; empty while there is nothing to show.
    [[nodiscard]] std::string statusText() const;
    // The last refusal, failure or Stop summary, as one line; cleared by the
    // next change that lands.
    [[nodiscard]] const std::string &notice() const noexcept { return notice_; }
    // Moves once per notice, so the same line twice is said twice.
    [[nodiscard]] std::uint64_t noticeSerial() const noexcept { return notice_serial_; }
    // Moves whenever anything above can read differently.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

  private:
    enum class Kind : std::uint8_t {
        Read,
        Change,
        Step,
        // From edit: set_play_state paused, then the step.
        EnterStep,
        // Stop with Keep marks: the recorded edits, read first.
        StopRead,
        Stop,
        View,
        Reload,
    };
    struct Pending {
        std::uint64_t id = 0;
        Kind kind = Kind::Read;
        // What a play_busy refusal of it lets be sent again.
        Action action = Action::None;
    };
    struct Label {
        // The edit an undo or a redo stands for, down to the first one.
        std::string base;
        CommittedEditKind kind = CommittedEditKind::edit;
    };

    bool connected_ = false;
    std::optional<PlayStatus> status_;
    std::uint64_t next_request_ = 1;
    std::vector<PlayRpcRequest> outgoing_;
    std::vector<Pending> pending_;
    std::uint64_t change_request_ = 0;
    std::uint64_t read_request_ = 0;
    // The game frame the steps asked for end on, until a reading reaches it.
    std::optional<std::uint64_t> step_until_;
    int follow_reads_left_ = 0;
    std::string notice_;
    std::uint64_t notice_serial_ = 0;
    std::uint64_t revision_ = 1;
    Action busy_action_ = Action::None;
    bool retry_armed_ = false;
    std::map<std::string, Label> labels_;
    std::set<std::string> keep_;
    // What the Stop in flight asked to keep, and how many edits it ends.
    std::vector<std::string> stop_keep_;
    std::size_t stop_recorded_ = 0;
    std::optional<StopOutcome> last_stop_;
    std::uint64_t stop_serial_ = 0;

    std::uint64_t queue(std::string method, nlohmann::json params, Kind kind,
                        Action action = Action::None);
    void read();
    void sendStop();
    void say(std::string line);
    std::optional<Pending> take(std::uint64_t request_id);
    void settle(const Pending &pending);
    void changed() noexcept { ++revision_; }
};

} // namespace PelicanStudio
