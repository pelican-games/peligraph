#include "playstatemodel.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

std::optional<std::uint64_t> unsignedField(const Json &object, const char *key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number_integer()) return std::nullopt;
    // A parser reads a non-negative number as unsigned; one built by hand may
    // hold it signed.
    if (found->is_number_unsigned()) return found->get<std::uint64_t>();
    const auto value = found->get<std::int64_t>();
    if (value < 0) return std::nullopt;
    return static_cast<std::uint64_t>(value);
}

std::optional<PlayState> stateNamed(const Json &value) {
    if (!value.is_string()) return std::nullopt;
    const auto &name = value.get_ref<const std::string &>();
    if (name == "playing") return PlayState::playing;
    if (name == "paused") return PlayState::paused;
    return std::nullopt;
}

std::string field(const Json &error, const char *key) {
    const auto found = error.find(key);
    return found != error.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

// Section 10's codes. A new one reads as the engine's own message until a row
// is added here.
struct PlayRefusalText {
    std::string_view code;
    std::string_view sentence;
};
constexpr std::array<PlayRefusalText, 4> PlayRefusalTexts{{
    {"step_frame_windowed",
     "step_frame steps a headless engine only; pause the game and use Step"},
    {"play_busy",
     "An edit or a preview is still being sent; press again once it has finished"},
    {"play_session_active", "The game is running; stop it first"},
    {"play_keep_unknown_transaction", "An edit marked Keep is not one this run recorded"},
}};

} // namespace

std::optional<PlayStatus> parsePlayStatus(const Json &status) {
    if (!status.is_object()) return std::nullopt;
    const auto play = status.find("play");
    if (play == status.end() || !play->is_object()) return std::nullopt;
    const auto state = play->contains("state") ? stateNamed(play->at("state")) : std::nullopt;
    const auto frame = unsignedField(*play, "game_frame");
    const auto pending = unsignedField(*play, "pending_steps");
    const auto time = play->find("game_time");
    if (!state || !frame || !pending || time == play->end() || !time->is_number())
        return std::nullopt;
    return PlayStatus{
        .state = *state,
        .game_frame = *frame,
        .game_time = time->get<double>(),
        .pending_steps = *pending,
    };
}

std::string playRefusalDescription(const Json &error) {
    if (!error.is_object()) return {};
    const auto code = field(error, "code");
    if (code.empty()) return {};
    std::string sentence;
    if (code == "play_state_mismatch") {
        // advance_frames while playing says what it requires; set_play_state
        // edit says what was requested (PM-1a has no edit state yet).
        const auto state = field(error, "state");
        const auto required = field(error, "required");
        const auto requested = field(error, "requested");
        if (!required.empty())
            sentence = "The game is " + (state.empty() ? std::string{"not " + required} : state) +
                       "; this works only while it is " + required;
        else if (!requested.empty())
            sentence = "This engine has no " + requested + " state yet; the game stays " +
                       (state.empty() ? std::string{"as it is"} : state);
        else
            sentence = "The game is not in the state this needs";
    } else {
        for (const auto &entry : PlayRefusalTexts)
            if (entry.code == code) sentence = entry.sentence;
    }
    if (sentence.empty()) return {};
    return sentence + " (" + code + ").";
}

void PlayStateModel::connect() {
    if (connected_) return;
    connected_ = true;
    changed();
    if (!status_) read();
}

void PlayStateModel::disconnect() {
    // Whatever the engine owed is gone with it. The engine that comes next
    // starts playing, and says so when it is asked.
    connected_ = false;
    status_.reset();
    outgoing_.clear();
    pending_.clear();
    change_request_ = 0;
    read_request_ = 0;
    step_until_.reset();
    follow_reads_left_ = 0;
    notice_.clear();
    changed();
}

bool PlayStateModel::observeStatus(const Json &status) {
    auto reading = parsePlayStatus(status);
    if (!reading) return false;
    if (step_until_ && reading->state == PlayState::paused && reading->pending_steps == 0 &&
        reading->game_frame >= *step_until_)
        step_until_.reset();
    if (reading->state != PlayState::paused) step_until_.reset();
    if (status_ != reading) {
        status_ = std::move(reading);
        changed();
    }
    return true;
}

bool PlayStateModel::togglePause() {
    if (!canTogglePause()) return false;
    const bool pause = !paused();
    change_request_ = queue("set_play_state", Json{{"state", pause ? "paused" : "playing"}},
                            Kind::Change);
    changed();
    return true;
}

bool PlayStateModel::step() {
    if (!canStep()) return false;
    change_request_ = queue("advance_frames", Json{{"count", 1}}, Kind::Step);
    changed();
    return true;
}

std::vector<PlayRpcRequest> PlayStateModel::takeRequests() {
    return std::exchange(outgoing_, {});
}

bool PlayStateModel::owns(std::uint64_t request_id) const noexcept {
    return std::ranges::any_of(pending_,
                               [request_id](const Pending &p) { return p.id == request_id; });
}

void PlayStateModel::receiveResult(std::uint64_t request_id, const Json &result) {
    const auto pending = take(request_id);
    if (!pending) return;
    settle(*pending);
    switch (pending->kind) {
    case Kind::Read:
        observeStatus(result);
        break;
    case Kind::Change: {
        notice_.clear();
        const auto state =
            result.is_object() && result.contains("state") ? stateNamed(result.at("state"))
                                                           : std::nullopt;
        if (state && status_) {
            status_->state = *state;
            // Leaving paused drops the steps not taken yet (PM-1a).
            if (*state == PlayState::playing) {
                status_->pending_steps = 0;
                step_until_.reset();
            }
        }
        // The answer names the state, not the frame it stopped on: read it.
        read();
        break;
    }
    case Kind::Step: {
        notice_.clear();
        if (const auto until = result.is_object() ? unsignedField(result, "until_game_frame")
                                                  : std::nullopt) {
            step_until_ = *until;
            if (status_ && status_->game_frame < *until)
                status_->pending_steps = std::max<std::uint64_t>(
                    status_->pending_steps, *until - status_->game_frame);
        }
        // The step lands on a frame to come: read until it has.
        follow_reads_left_ = FollowReadLimit;
        read();
        break;
    }
    }
    changed();
}

void PlayStateModel::receiveRefusal(std::uint64_t request_id, const Json &error) {
    const auto pending = take(request_id);
    if (!pending) return;
    settle(*pending);
    if (pending->kind != Kind::Read) {
        notice_ = playRefusalDescription(error);
        if (notice_.empty()) {
            const auto message = field(error, "message");
            const auto code = field(error, "code");
            notice_ = message.empty() ? "The engine refused the request" : message;
            if (!code.empty()) notice_ += " (" + code + ")";
        }
        ++notice_serial_;
        // A refusal says the state is not the one this panel believed: ask.
        read();
    }
    changed();
}

void PlayStateModel::receiveFailure(std::uint64_t request_id, std::string message) {
    const auto pending = take(request_id);
    if (!pending) return;
    settle(*pending);
    if (pending->kind != Kind::Read) {
        notice_ = std::move(message);
        ++notice_serial_;
    }
    changed();
}

bool PlayStateModel::followWanted() const noexcept {
    return connected_ && read_request_ == 0 && step_until_.has_value() && follow_reads_left_ > 0;
}

bool PlayStateModel::follow() {
    if (!followWanted()) return false;
    --follow_reads_left_;
    read();
    return true;
}

std::string PlayStateModel::statusText() const {
    if (!status_) return {};
    const auto frame = " · frame " + std::to_string(status_->game_frame);
    if (status_->state == PlayState::playing) return "Playing" + frame;
    if (status_->pending_steps > 0)
        return "Stepping (" + std::to_string(status_->pending_steps) + " left)" + frame;
    return "Paused" + frame;
}

std::uint64_t PlayStateModel::queue(std::string method, Json params, Kind kind) {
    const auto id = next_request_++;
    outgoing_.push_back({.request_id = id, .method = std::move(method), .params = std::move(params)});
    pending_.push_back({.id = id, .kind = kind});
    return id;
}

void PlayStateModel::read() {
    // One reading at a time: a second one in the air would say the same.
    if (!connected_ || read_request_ != 0) return;
    read_request_ = queue("get_status", Json::object(), Kind::Read);
}

std::optional<PlayStateModel::Pending> PlayStateModel::take(std::uint64_t request_id) {
    const auto found = std::ranges::find_if(
        pending_, [request_id](const Pending &p) { return p.id == request_id; });
    if (found == pending_.end()) return std::nullopt;
    const auto pending = *found;
    pending_.erase(found);
    return pending;
}

void PlayStateModel::settle(const Pending &pending) {
    if (pending.id == change_request_) change_request_ = 0;
    if (pending.id == read_request_) read_request_ = 0;
}

} // namespace PelicanStudio
