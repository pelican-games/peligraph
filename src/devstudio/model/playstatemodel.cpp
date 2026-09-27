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
    if (name == "edit") return PlayState::edit;
    if (name == "playing") return PlayState::playing;
    if (name == "paused") return PlayState::paused;
    return std::nullopt;
}

std::optional<ViewCamera> viewNamed(const Json &value) {
    if (!value.is_string()) return std::nullopt;
    const auto &name = value.get_ref<const std::string &>();
    if (name == "editor") return ViewCamera::editor;
    if (name == "game") return ViewCamera::game;
    return std::nullopt;
}

std::string field(const Json &error, const char *key) {
    const auto found = error.find(key);
    return found != error.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

// A state as the end of "while it is ...": "paused", "in edit".
std::string stateWords(const std::string &name) {
    return name == "edit" ? "in edit" : name;
}

// A field that is a state or a list of states ("paused", ["edit","paused"]),
// as words: "paused", "in edit or paused".
std::string states(const Json &error, const char *key) {
    const auto found = error.find(key);
    if (found == error.end()) return {};
    if (found->is_string()) return stateWords(found->get<std::string>());
    if (!found->is_array()) return {};
    std::string joined;
    for (std::size_t index = 0; index < found->size(); ++index) {
        if (!found->at(index).is_string()) return {};
        if (index > 0) joined += index + 1 == found->size() ? " or " : ", ";
        joined += stateWords(found->at(index).get<std::string>());
    }
    return joined;
}

// Section 10's codes. A new one reads as the engine's own message until a row
// is added here.
struct PlayRefusalText {
    std::string_view code;
    std::string_view sentence;
};
constexpr std::array<PlayRefusalText, 5> PlayRefusalTexts{{
    {"step_frame_windowed",
     "step_frame steps a headless engine only; pause the game and use Step"},
    {"play_busy",
     "An edit or a preview is still being sent; press again once it has finished"},
    {"play_keep_unknown_transaction",
     "An edit marked Keep is not one this run recorded (it was undone); nothing was stopped"},
    {"view_camera_unavailable",
     "This engine has no editor camera; it was started without --start-state edit or "
     "--free-camera"},
    {"offline.mode_conflict@1",
     "This engine was started in edit, which does not record or replay input"},
}};

// The game logic DLL, when Play reloads it (PM-2) or Reload game logic does.
constexpr std::array<PlayRefusalText, 10> GameLogicReloadTexts{{
    {"game_logic_reload_failed@1", "The game logic DLL did not reload"},
    {"game_logic_abi_stale@1", "The game logic DLL was built for another engine version"},
    {"game_logic_abi_unsupported_import@1",
     "The game logic DLL links to the engine in a way it cannot load"},
    {"game_logic_abi_unreadable@1", "The game logic DLL could not be read"},
    {"game_logic_load_failed@1", "The game logic DLL did not load"},
    {"game_logic_registration_rejected@1",
     "The engine refused what the game logic DLL registers"},
    {"game_logic_unavailable@1", "This engine cannot reload game logic"},
    {"game_logic_source_busy@1", "The game logic DLL is still being written; build it again"},
    {"game_logic_shadow_copy_failed@1",
     "The engine could not copy the game logic DLL to load it"},
    {"game_logic_not_configured@1", "No game logic DLL is configured"},
}};

// error_fields as words: "found v4, host v3". Nested values are written as
// the engine wrote them.
std::string fieldsText(const Json &fields) {
    if (!fields.is_object()) return {};
    std::string text;
    for (const auto &[key, value] : fields.items()) {
        if (!text.empty()) text += ", ";
        text += key;
        text += ' ';
        if (value.is_string()) {
            text += value.get<std::string>();
        } else if (value.is_array()) {
            for (std::size_t index = 0; index < value.size(); ++index) {
                if (index > 0) text += " ";
                text += value[index].is_string() ? value[index].get<std::string>()
                                                 : value[index].dump();
            }
        } else {
            text += value.dump();
        }
    }
    return text;
}

// The edit an error names in not_kept (EditorEditErrorCode's names), as why
// it could not be applied again on the scene Stop put back.
struct NotKeptText {
    std::string_view code;
    std::string_view reason;
};
constexpr std::array<NotKeptText, 8> NotKeptTexts{{
    {"closure_unresolvable",
     "it needs an edit that was not kept (the object it changes was made during the run)"},
    {"target_missing", "what it changes is not in the scene Stop put back"},
    {"object_missing", "what it changes is not in the scene Stop put back"},
    {"stale_revision", "the scene changed under it"},
    {"precondition_failed", "the value it started from is not the one Stop put back"},
    {"conflict", "another kept edit changed the same thing"},
    {"gate_closed", "the scene could not be edited when Stop applied it"},
    {"validation_failed", "the engine refused it on the scene Stop put back"},
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
    PlayStatus reading{
        .state = *state,
        .game_frame = *frame,
        .game_time = time->get<double>(),
        .pending_steps = *pending,
    };
    if (const auto view = play->find("view_camera"); view != play->end()) {
        reading.view_camera = viewNamed(*view);
        if (!reading.view_camera) return std::nullopt;
    }
    if (const auto session = play->find("session"); session != play->end()) {
        if (!session->is_boolean()) return std::nullopt;
        reading.session = session->get<bool>();
    }
    if (const auto edits = play->find("recorded_edits"); edits != play->end()) {
        if (!edits->is_array()) return std::nullopt;
        for (const auto &edit : *edits) {
            if (!edit.is_object()) return std::nullopt;
            const auto id = edit.find("transaction_id");
            const auto actor = unsignedField(edit, "actor");
            if (id == edit.end() || !id->is_string() || !actor) return std::nullopt;
            reading.recorded_edits.push_back({id->get<std::string>(), *actor});
        }
    }
    return reading;
}

std::string playRefusalDescription(const Json &error) {
    if (!error.is_object()) return {};
    const auto code = field(error, "code");
    if (code.empty()) return {};
    std::string sentence;
    if (code == "play_state_mismatch") {
        // advance_frames and set_view_camera say what they require.
        const auto state = field(error, "state");
        const auto required = states(error, "required");
        if (!required.empty())
            sentence = "The game is " +
                       (state.empty() ? std::string{"not " + required} : stateWords(state)) +
                       "; this works only while it is " + required;
        else
            sentence = "The game is not in the state this needs";
    } else if (code == "play_session_active") {
        const auto method = field(error, "method");
        sentence = "The game is running, and Stop puts the scene back as it was when Play "
                   "was pressed; stop it first" +
                   (method.empty() ? std::string{} : " (" + method + ")");
    } else {
        for (const auto &entry : PlayRefusalTexts)
            if (entry.code == code) sentence = entry.sentence;
        for (const auto &entry : GameLogicReloadTexts) {
            if (entry.code != code) continue;
            sentence = entry.sentence;
            // set_play_state names the state it was asked for; Play did not
            // start the game.
            if (!field(error, "requested").empty()) sentence += ", so the game did not start";
            if (const auto fields = error.find("error_fields"); fields != error.end()) {
                const auto words = fieldsText(*fields);
                if (!words.empty()) sentence += ": " + words;
            }
        }
    }
    if (sentence.empty()) return {};
    return sentence + " (" + code + ").";
}

std::string notKeptReason(std::string_view code) {
    for (const auto &entry : NotKeptTexts)
        if (entry.code == code) return std::string{entry.reason} + " (" + std::string{code} + ")";
    return "the engine could not apply it again (" + std::string{code} + ")";
}

void PlayStateModel::connect() {
    if (connected_) return;
    connected_ = true;
    changed();
    if (!status_) read();
}

void PlayStateModel::disconnect() {
    // Whatever the engine owed is gone with it, and so is its session: the
    // next engine starts in edit, and says so when it is asked.
    connected_ = false;
    status_.reset();
    outgoing_.clear();
    pending_.clear();
    change_request_ = 0;
    read_request_ = 0;
    step_until_.reset();
    follow_reads_left_ = 0;
    notice_.clear();
    busy_action_ = Action::None;
    retry_armed_ = false;
    keep_.clear();
    stop_keep_.clear();
    stop_recorded_ = 0;
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
        // A session that has ended keeps no marks for the next.
        if (!reading->session) keep_.clear();
        status_ = std::move(reading);
        changed();
    }
    return true;
}

bool PlayStateModel::playOrStop() {
    if (editing()) return play();
    return stop();
}

bool PlayStateModel::play() {
    if (!canPlayOrStop() || playing()) return false;
    const bool starting = editing();
    change_request_ =
        queue("set_play_state", Json{{"state", "playing"}}, Kind::Change,
              starting ? Action::Play : Action::None);
    if (starting) last_stop_.reset();
    busy_action_ = Action::None;
    retry_armed_ = false;
    changed();
    return true;
}

bool PlayStateModel::stop() {
    if (!canPlayOrStop() || editing()) return false;
    busy_action_ = Action::None;
    retry_armed_ = false;
    if (keep_.empty()) {
        stop_keep_.clear();
        sendStop();
    } else {
        // A mark on an edit undone since the last reading would make the
        // engine refuse the whole Stop (play_keep_unknown_transaction): read
        // the recorded edits first, and name only those.
        change_request_ = queue("get_status", Json::object(), Kind::StopRead, Action::Stop);
    }
    changed();
    return true;
}

void PlayStateModel::sendStop() {
    stop_keep_.clear();
    stop_recorded_ = status_ ? status_->recorded_edits.size() : 0;
    if (status_)
        for (const auto &edit : status_->recorded_edits)
            if (keep_.contains(edit.transaction_id)) stop_keep_.push_back(edit.transaction_id);
    Json params{{"state", "edit"}};
    // Nothing marked: keep is left out, and every edit goes back.
    if (!stop_keep_.empty()) params["keep"] = stop_keep_;
    change_request_ = queue("set_play_state", std::move(params), Kind::Stop, Action::Stop);
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
    busy_action_ = Action::None;
    retry_armed_ = false;
    if (editing()) {
        // 2 / 12.1: a session begins paused on game frame 0, and the step
        // follows once it has.
        last_stop_.reset();
        change_request_ = queue("set_play_state", Json{{"state", "paused"}}, Kind::EnterStep,
                                Action::Step);
    } else {
        change_request_ = queue("advance_frames", Json{{"count", 1}}, Kind::Step);
    }
    changed();
    return true;
}

bool PlayStateModel::setViewCamera(ViewCamera camera) {
    if (!canSetView()) return false;
    if (status_->view_camera == camera) return false;
    change_request_ = queue("set_view_camera",
                            Json{{"camera", camera == ViewCamera::editor ? "editor" : "game"}},
                            Kind::View);
    changed();
    return true;
}

bool PlayStateModel::reloadGameLogic() {
    if (!canReload()) return false;
    change_request_ = queue("reload_game_logic", Json::object(), Kind::Reload);
    changed();
    return true;
}

void PlayStateModel::armRetry() {
    if (busy_action_ == Action::None || retry_armed_) return;
    retry_armed_ = true;
    changed();
}

bool PlayStateModel::retryWhenIdle() {
    if (!retry_armed_ || busy()) return false;
    const auto action = busy_action_;
    retry_armed_ = false;
    busy_action_ = Action::None;
    changed();
    switch (action) {
    case Action::Play: return editing() && play();
    case Action::Stop: return !editing() && stop();
    case Action::Step: return step();
    case Action::None: break;
    }
    return false;
}

void PlayStateModel::noteCommittedEdit(const CommittedEdit &edit) {
    if (edit.transaction_id.empty()) return;
    Label label{.base = edit.label, .kind = edit.kind};
    if (edit.kind != CommittedEditKind::edit) {
        // An undo stands for the edit it takes back; a redo for what the undo
        // it takes back stood for.
        const auto source = labels_.find(edit.source_transaction_id);
        label.base = source != labels_.end() ? source->second.base : std::string{};
    }
    labels_[edit.transaction_id] = std::move(label);
    if (edit.keep) keep_.insert(edit.transaction_id);
    changed();
    // The panel follows the session's record, which this edit just changed.
    if (session()) read();
}

void PlayStateModel::setKeep(const std::string &transaction_id, bool keep) {
    if (transaction_id.empty()) return;
    const bool changed_mark = keep ? keep_.insert(transaction_id).second
                                   : keep_.erase(transaction_id) > 0;
    if (changed_mark) changed();
}

void PlayStateModel::keepAll() {
    if (!status_) return;
    for (const auto &edit : status_->recorded_edits) keep_.insert(edit.transaction_id);
    changed();
}

void PlayStateModel::keepNone() {
    if (keep_.empty()) return;
    keep_.clear();
    changed();
}

std::string PlayStateModel::labelOf(const std::string &transaction_id) const {
    const auto found = labels_.find(transaction_id);
    if (found == labels_.end()) return {};
    const auto &label = found->second;
    switch (label.kind) {
    case CommittedEditKind::edit: return label.base;
    case CommittedEditKind::undo:
        return label.base.empty() ? std::string{"Undo"} : "Undo " + label.base;
    case CommittedEditKind::redo:
        return label.base.empty() ? std::string{"Redo"} : "Redo " + label.base;
    }
    return label.base;
}

std::vector<PlayChangeRow> PlayStateModel::changeRows() const {
    std::vector<PlayChangeRow> rows;
    if (!status_) return rows;
    for (const auto &edit : status_->recorded_edits) {
        PlayChangeRow row{.transaction_id = edit.transaction_id,
                          .actor = edit.actor,
                          .text = labelOf(edit.transaction_id),
                          .keep = keep_.contains(edit.transaction_id)};
        row.studio = !row.text.empty();
        if (!row.studio)
            row.text = "Actor " + std::to_string(edit.actor) + " · " + edit.transaction_id;
        rows.push_back(std::move(row));
    }
    return rows;
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
    case Kind::StopRead:
        observeStatus(result);
        if (!status_ || status_->state == PlayState::edit) {
            // Stopped already, by someone else: nothing is left to stop.
            read();
            break;
        }
        sendStop();
        break;
    case Kind::Change:
    case Kind::EnterStep: {
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
        if (pending->kind == Kind::EnterStep && state == PlayState::paused) {
            // The session is on frame 0; now the step.
            change_request_ = queue("advance_frames", Json{{"count", 1}}, Kind::Step);
            break;
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
    case Kind::Stop: {
        StopOutcome outcome;
        if (result.is_object())
            if (const auto not_kept = result.find("not_kept");
                not_kept != result.end() && not_kept->is_array()) {
                for (const auto &entry : *not_kept) {
                    if (!entry.is_object()) continue;
                    StopOutcome::NotKept row{.transaction_id = field(entry, "transaction_id"),
                                             .code = field(entry, "code")};
                    row.text = labelOf(row.transaction_id);
                    if (row.text.empty()) row.text = row.transaction_id;
                    row.reason = notKeptReason(row.code);
                    outcome.not_kept.push_back(std::move(row));
                }
            }
        outcome.kept = stop_keep_.size() > outcome.not_kept.size()
                           ? stop_keep_.size() - outcome.not_kept.size()
                           : 0;
        outcome.reverted = stop_recorded_ > outcome.kept ? stop_recorded_ - outcome.kept : 0;
        std::string line = "Stopped: " + std::to_string(outcome.reverted) + " reverted, " +
                           std::to_string(outcome.kept) + " kept";
        if (!outcome.not_kept.empty())
            line += ", " + std::to_string(outcome.not_kept.size()) + " could not be kept";
        last_stop_ = std::move(outcome);
        ++stop_serial_;
        keep_.clear();
        stop_keep_.clear();
        stop_recorded_ = 0;
        if (status_) {
            status_->state = PlayState::edit;
            status_->session = false;
            status_->recorded_edits.clear();
            status_->pending_steps = 0;
        }
        step_until_.reset();
        say(std::move(line));
        read();
        break;
    }
    case Kind::View: {
        notice_.clear();
        if (status_ && result.is_object() && result.contains("camera"))
            if (const auto camera = viewNamed(result.at("camera"))) status_->view_camera = camera;
        read();
        break;
    }
    case Kind::Reload:
        say("Game logic reloaded" +
            std::string{editing() ? "; the scene waits in edit, its behaviors asleep."
                                  : "; the game goes on from the scene as it is now."});
        read();
        break;
    }
    changed();
}

void PlayStateModel::receiveRefusal(std::uint64_t request_id, const Json &error) {
    const auto pending = take(request_id);
    if (!pending) return;
    settle(*pending);
    if (pending->kind != Kind::Read) {
        auto line = playRefusalDescription(error);
        if (line.empty()) {
            const auto message = field(error, "message");
            const auto code = field(error, "code");
            line = message.empty() ? "The engine refused the request" : message;
            if (!code.empty()) line += " (" + code + ")";
        }
        // play_busy: the same action can go again once the scene is idle.
        if (field(error, "code") == "play_busy" && pending->action != Action::None) {
            busy_action_ = pending->action;
            retry_armed_ = false;
        }
        stop_keep_.clear();
        say(std::move(line));
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
        stop_keep_.clear();
        say(std::move(message));
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
    if (status_->state == PlayState::edit) return "Edit";
    const auto frame = " · frame " + std::to_string(status_->game_frame);
    if (status_->state == PlayState::playing) return "Playing" + frame;
    if (status_->pending_steps > 0)
        return "Stepping (" + std::to_string(status_->pending_steps) + " left)" + frame;
    return "Paused" + frame;
}

std::uint64_t PlayStateModel::queue(std::string method, Json params, Kind kind, Action action) {
    const auto id = next_request_++;
    outgoing_.push_back({.request_id = id, .method = std::move(method), .params = std::move(params)});
    pending_.push_back({.id = id, .kind = kind, .action = action});
    return id;
}

void PlayStateModel::read() {
    // One reading at a time: a second one in the air would say the same.
    if (!connected_ || read_request_ != 0) return;
    read_request_ = queue("get_status", Json::object(), Kind::Read);
}

void PlayStateModel::say(std::string line) {
    notice_ = std::move(line);
    ++notice_serial_;
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
