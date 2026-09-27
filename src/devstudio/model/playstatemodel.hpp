#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {

// PM-3a. The game's play state as the engine reports it (PM-1a,
// docs/design_play_mode.md sections 2, 3, 8 and 12). This build of the engine
// has playing and paused; edit and Stop come with PM-1b / PM-2 (PM-3b).
enum class PlayState : std::uint8_t {
    playing,
    paused,
};

// get_status.play. `game_frame` counts game steps; it is not get_status.frame,
// which is the evaluation number and goes on rising while the game is paused.
struct PlayStatus {
    PlayState state = PlayState::playing;
    std::uint64_t game_frame = 0;
    double game_time = 0.0;
    std::uint64_t pending_steps = 0;

    bool operator==(const PlayStatus &) const = default;
};

// The `play` member of a get_status result. Empty when there is none -- an
// engine from before PM-1a -- or when it is not the shape PM-1a writes.
[[nodiscard]] std::optional<PlayStatus> parsePlayStatus(const nlohmann::json &status);

// The play refusals of section 10, one sentence each: "<sentence> (<code>)."
// `error` is the refusal's code and message with its data's fields beside
// them ({"code":..., "message":..., "state":..., "required":...}). Empty for
// any other refusal. knownRefusalDescription answers these too.
[[nodiscard]] std::string playRefusalDescription(const nlohmann::json &error);

struct PlayRpcRequest {
    std::uint64_t request_id = 0;
    std::string method;
    nlohmann::json params;
};

// What the Pause / Step buttons and the status line show, and what they send.
//
// Studio reads get_status only when something asks (Q-2 stopped the idle
// clocks), so the reading here moves with every get_status reply any panel
// receives (observeStatus), with one read after each change of state, and
// with a few follow-up reads while a step is still to be taken. No clock of
// its own reads while nothing changes.
class PlayStateModel {
  public:
    // How many follow-up reads one change may take before it gives up on the
    // steps it is waiting for (about two seconds at the view's interval).
    static constexpr int FollowReadLimit = 40;

    // The engine answers editor requests. A reading already in hand (the
    // readiness probe's) is kept; without one, get_status is asked once.
    void connect();
    // The engine went away or is being restarted: nothing it said holds for
    // the next one, which starts playing.
    void disconnect();

    // A get_status result, whoever asked for it. True when it carried play.
    bool observeStatus(const nlohmann::json &status);

    // ❚❚ Pause, pressed or released: set_play_state {state: paused|playing}.
    // False, and nothing queued, when the button is not pressable now.
    bool togglePause();
    // ▶| Step: advance_frames {count: 1}, paused only.
    bool step();

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
    [[nodiscard]] bool paused() const noexcept {
        return status_ && status_->state == PlayState::paused;
    }
    // A set_play_state or advance_frames is waiting for its answer.
    [[nodiscard]] bool busy() const noexcept { return change_request_ != 0; }
    [[nodiscard]] bool canTogglePause() const noexcept {
        return connected_ && supported() && !busy();
    }
    [[nodiscard]] bool canStep() const noexcept { return canTogglePause() && paused(); }

    // "Playing · frame N", "Paused · frame N", "Stepping (k left) · frame N";
    // empty while there is nothing to show.
    [[nodiscard]] std::string statusText() const;
    // The last refusal or failure, as one line; cleared by the next change
    // that lands.
    [[nodiscard]] const std::string &notice() const noexcept { return notice_; }
    // Moves once per refusal or failure, so the same line twice is said twice.
    [[nodiscard]] std::uint64_t noticeSerial() const noexcept { return notice_serial_; }
    // Moves whenever anything above can read differently.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

  private:
    enum class Kind : std::uint8_t { Read, Change, Step };
    struct Pending {
        std::uint64_t id = 0;
        Kind kind = Kind::Read;
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

    std::uint64_t queue(std::string method, nlohmann::json params, Kind kind);
    void read();
    std::optional<Pending> take(std::uint64_t request_id);
    void settle(const Pending &pending);
    void changed() noexcept { ++revision_; }
};

} // namespace PelicanStudio
