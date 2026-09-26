#include "renderedithistory.hpp"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

// How long a ticket is left before it is asked about again: the panels' own
// poll, so the engine sees nothing new.
constexpr int TicketPollMs = 25;

// The render edits that replace what documents hold; RD-1 puts them back.
constexpr std::array<std::string_view, 12> RecordedMethods{
    "edit_preset_pass",
    "decompose_material_pass",
    "edit_compute_task",
    "add_compute_task",
    "remove_compute_task",
    "edit_render_targets",
    "edit_render_features",
    "edit_frame_node_order",
    "set_material_route_default",
    "edit_ray_callable_records",
    "edit_scene_acceleration_structures",
    "edit_region_replacement",
};

// The ones that create or remove files: RD-1 cannot put those back (RD-1b).
constexpr std::array<std::string_view, 4> BarrierMethods{
    "add_authored_pass",
    "remove_authored_pass",
    "fork_render_pipeline",
    "import_render_feature",
};

std::string stringAt(const Json &object, const char *key) {
    if (!object.is_object()) return {};
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>()
                                                        : std::string{};
}

std::string named(std::string prefix, const std::string &name, std::string fallback) {
    return name.empty() ? std::move(fallback) : std::move(prefix) + name;
}

std::string status(const Json &result) { return stringAt(result, "status"); }

bool committed(const Json &result) {
    return status(result) == "committed" && result.is_object() &&
           result.value("committed", false);
}

std::map<std::string, RenderEditHistory::Document> documentsOf(const Json &result) {
    std::map<std::string, RenderEditHistory::Document> documents;
    if (!result.is_object()) return documents;
    const auto found = result.find("documents");
    if (found == result.end() || !found->is_array()) return documents;
    for (const auto &document : *found) {
        const auto reference = stringAt(document, "reference");
        if (reference.empty()) continue;
        documents[reference] = RenderEditHistory::Document{
            stringAt(document, "digest"), stringAt(document, "content")};
    }
    return documents;
}

Json writtenOf(const Json &commit) {
    if (!commit.is_object()) return Json::object();
    const auto found = commit.find("documents");
    return found != commit.end() && found->is_object() ? *found : Json::object();
}

} // namespace

RenderEditHistory::RenderEditHistory(Transport transport)
    : transport_(std::move(transport)) {}

bool RenderEditHistory::recordsMethod(const std::string &method) {
    return std::ranges::find(RecordedMethods, method) != RecordedMethods.end();
}

bool RenderEditHistory::followsMethod(const std::string &method) {
    return recordsMethod(method) ||
           std::ranges::find(BarrierMethods, method) != BarrierMethods.end();
}

bool RenderEditHistory::waitingFor(std::int64_t id) const noexcept {
    if (id == 0) return false;
    if (operation_ && operation_->request == id) return true;
    return std::ranges::any_of(pending_, [id](const PendingEdit &edit) {
        return edit.snapshot_request == id || edit.edit_request == id ||
               edit.poll_request == id;
    });
}

bool RenderEditHistory::barrierMethod(const std::string &method, const Json &params) {
    if (std::ranges::find(BarrierMethods, method) != BarrierMethods.end()) return true;
    // Make editable writes a new feature file and switches to it.
    if (method == "edit_render_features" && params.is_object()) {
        const auto ops = params.find("ops");
        if (ops != params.end() && ops->is_array())
            return std::ranges::any_of(*ops, [](const Json &op) {
                return stringAt(op, "op") == "materialize";
            });
    }
    return false;
}

std::string RenderEditHistory::labelFor(const std::string &method, const Json &params) {
    if (method == "edit_preset_pass")
        return named("Edit pass ", stringAt(params, "pass_name"), "Edit pass");
    if (method == "decompose_material_pass")
        return named("Decompose ", stringAt(params, "pass"), "Decompose");
    if (method == "edit_compute_task")
        return named("Edit task ", stringAt(params, "task_name"), "Edit task");
    if (method == "add_compute_task")
        return named("Add task ", stringAt(params.is_object() ? params.value("task", Json{}) : Json{}, "name"),
                     "Add task");
    if (method == "remove_compute_task")
        return named("Remove task ", stringAt(params, "task_name"), "Remove task");
    if (method == "edit_render_targets") {
        const auto ops = params.is_object() ? params.value("ops", Json::array()) : Json::array();
        if (ops.is_array() && ops.size() == 1) {
            const auto &op = ops.front();
            const auto kind = stringAt(op, "op");
            const auto target = stringAt(op.is_object() ? op.value("target", Json{}) : Json{}, "name");
            if (kind == "add") return named("Add render target ", target, "Add render target");
            if (kind == "replace")
                return named("Edit render target ", stringAt(op, "name"), "Edit render target");
            if (kind == "remove")
                return named("Remove render target ", stringAt(op, "name"), "Remove render target");
        }
        return "Edit render targets";
    }
    if (method == "edit_render_features") {
        const auto ops = params.is_object() ? params.value("ops", Json::array()) : Json::array();
        if (ops.is_array() && !ops.empty()) {
            const auto kind = stringAt(ops.front(), "op");
            if (kind == "materialize") return "Make feature editable";
            if (kind == "set_parameters") return "Edit feature parameters";
            if (ops.size() == 1 && kind == "add")
                return named("Add feature ", stringAt(ops.front(), "feature"), "Add feature");
            if (ops.size() == 1 && kind == "remove")
                return named("Remove feature ", stringAt(ops.front(), "feature"), "Remove feature");
        }
        return "Edit render features";
    }
    if (method == "edit_frame_node_order") return "Reorder passes";
    if (method == "set_material_route_default") return "Set material route default";
    if (method == "edit_ray_callable_records") return "Edit callable records";
    if (method == "edit_scene_acceleration_structures") return "Edit acceleration structures";
    if (method == "edit_region_replacement")
        return named("Replace region ", stringAt(params, "region"), "Replace region");
    if (method == "add_authored_pass")
        return named("Add pass ", stringAt(params.is_object() ? params.value("pass", Json{}) : Json{}, "name"),
                     "Add pass");
    if (method == "remove_authored_pass")
        return named("Remove pass ", stringAt(params, "fragment_reference"), "Remove pass");
    if (method == "fork_render_pipeline") return "Fork render pipeline";
    if (method == "import_render_feature") return "Import feature";
    return method;
}

std::int64_t RenderEditHistory::send(const std::string &method, const Json &params,
                                     std::string *error) {
    if (!transport_.request) {
        if (error != nullptr) *error = "the engine is not connected";
        return 0;
    }
    own_request_ = true;
    std::int64_t id = 0;
    try {
        id = transport_.request(method, params, error);
    } catch (...) {
        own_request_ = false;
        throw;
    }
    own_request_ = false;
    return id;
}

void RenderEditHistory::notify() {
    if (changed) changed();
}

void RenderEditHistory::say(std::string line) {
    line_ = std::move(line);
    ++line_serial_;
}

void RenderEditHistory::beforeRequest(const std::string &method, const Json &params) {
    if (own_request_) return;
    const bool barrier = barrierMethod(method, params);
    if (!barrier && !recordsMethod(method)) return;
    PendingEdit edit;
    edit.method = method;
    edit.label = labelFor(method, params);
    edit.barrier = barrier;
    // The documents as they are before the edit: asked for first, so the
    // engine answers it before it takes the edit.
    if (!barrier) edit.snapshot_request = send("get_render_documents", Json::object(), nullptr);
    unsent_ = std::move(edit);
}

void RenderEditHistory::requestSent(std::int64_t id, const std::string &method,
                                    const Json &) {
    if (own_request_ || !unsent_ || unsent_->method != method) return;
    auto edit = std::move(*unsent_);
    unsent_.reset();
    if (id == 0) return;
    edit.edit_request = id;
    pending_.push_back(std::move(edit));
}

bool RenderEditHistory::receiveResult(std::int64_t id, const Json &result) {
    if (id == 0) return false;
    for (std::size_t index = 0; index < pending_.size(); ++index) {
        auto &edit = pending_[index];
        if (id == edit.snapshot_request) {
            edit.snapshot_request = 0;
            if (status(result) != "rejected" && result.is_object() &&
                result.contains("documents")) {
                edit.snapshot = documentsOf(result);
                edit.snapshot_received = true;
            }
            return true;
        }
        if (id == edit.edit_request && edit.ticket.empty() && edit.poll_request == 0) {
            const auto ticket = stringAt(result, "ticket");
            if (status(result) == "accepted" && !ticket.empty()) {
                edit.ticket = ticket;
                pollEdit(edit.edit_request);
                return true;
            }
            auto finished = std::move(edit);
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(index));
            if (committed(result)) finishEdit(std::move(finished), result);
            return true;
        }
        if (id == edit.poll_request) {
            edit.poll_request = 0;
            if (status(result) == "pending") {
                const auto key = edit.edit_request;
                const auto epoch = epoch_;
                if (transport_.schedule)
                    transport_.schedule(TicketPollMs, [this, key, epoch] {
                        if (epoch == epoch_) pollEdit(key);
                    });
                return true;
            }
            auto finished = std::move(edit);
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(index));
            if (committed(result)) finishEdit(std::move(finished), result);
            return true;
        }
    }
    if (!operation_ || id != operation_->request) return false;
    operation_->request = 0;
    switch (operation_->stage) {
    case Stage::Read:
        if (status(result) == "rejected") {
            refuseOperation(result);
            return true;
        }
        operation_->current = documentsOf(result);
        sendRestore();
        return true;
    case Stage::Restore: {
        const auto ticket = stringAt(result, "ticket");
        if (status(result) == "accepted" && !ticket.empty()) {
            operation_->stage = Stage::Poll;
            operation_->ticket = ticket;
            pollOperation();
        } else if (committed(result)) {
            finishOperation(result);
        } else {
            refuseOperation(result);
        }
        return true;
    }
    case Stage::Poll:
        if (status(result) == "pending") {
            const auto epoch = epoch_;
            if (transport_.schedule)
                transport_.schedule(TicketPollMs, [this, epoch] {
                    if (epoch == epoch_ && operation_ && operation_->stage == Stage::Poll &&
                        operation_->request == 0)
                        pollOperation();
                });
        } else if (committed(result)) {
            finishOperation(result);
        } else {
            refuseOperation(result);
        }
        return true;
    }
    return true;
}

bool RenderEditHistory::receiveFailure(std::int64_t id, const std::string &message) {
    if (id == 0) return false;
    for (std::size_t index = 0; index < pending_.size(); ++index) {
        auto &edit = pending_[index];
        if (id == edit.snapshot_request) {
            // The edit is still followed; without its documents it can only
            // stand as a barrier.
            edit.snapshot_request = 0;
            return true;
        }
        if (id == edit.edit_request || id == edit.poll_request) {
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(index));
            return true;
        }
    }
    if (!operation_ || id != operation_->request) return false;
    operation_->request = 0;
    failOperation(message);
    return true;
}

void RenderEditHistory::pollEdit(std::int64_t edit_request) {
    const auto found = std::ranges::find(pending_, edit_request, &PendingEdit::edit_request);
    if (found == pending_.end() || found->ticket.empty() || found->poll_request != 0) return;
    found->poll_request = send("get_edit_result", Json{{"ticket", found->ticket}}, nullptr);
    if (found->poll_request == 0) pending_.erase(found);
}

void RenderEditHistory::finishEdit(PendingEdit edit, const Json &commit) {
    const auto written = writtenOf(commit);
    // Nothing was written: there is nothing to take back.
    if (commit.value("no_change", false) || written.empty()) return;
    Entry entry;
    entry.label = edit.label;
    entry.barrier = edit.barrier || !edit.snapshot_received;
    entry.restart_required = commit.value("restart_required", false);
    if (!entry.barrier) {
        for (const auto &[reference, digest] : written.items()) {
            const auto before = edit.snapshot.find(reference);
            // Erased (null), or not there before: a file came or went.
            if (!digest.is_string() || before == edit.snapshot.end()) {
                entry.barrier = true;
                break;
            }
            entry.before[reference] = before->second;
            entry.after[reference] = digest.get<std::string>();
        }
    }
    if (entry.barrier) {
        entry.before.clear();
        entry.after.clear();
    }
    undo_.push_back(std::move(entry));
    redo_.clear();
    notify();
}

bool RenderEditHistory::canUndo() const noexcept { return !undo_.empty() && !busy(); }

bool RenderEditHistory::canRedo() const noexcept { return !redo_.empty() && !busy(); }

std::string RenderEditHistory::undoLabel() const {
    return undo_.empty() ? std::string{} : undo_.back().label;
}

std::string RenderEditHistory::redoLabel() const {
    return redo_.empty() ? std::string{} : redo_.back().label;
}

bool RenderEditHistory::undo() {
    if (busy()) {
        say("Wait for the Undo or Redo in flight.");
    } else if (!pending_.empty() || unsent_) {
        say("Wait for the render edit in flight.");
    } else if (undo_.empty()) {
        say("Nothing to undo.");
    } else if (undo_.back().barrier) {
        say("Cannot undo past \"" + undo_.back().label +
                "\": it created or removed files. (RD-1b)");
    } else {
        return startRestore(Direction::Undo);
    }
    notify();
    return false;
}

bool RenderEditHistory::redo() {
    if (busy()) {
        say("Wait for the Undo or Redo in flight.");
    } else if (!pending_.empty() || unsent_) {
        say("Wait for the render edit in flight.");
    } else if (redo_.empty()) {
        say("Nothing to redo.");
    } else {
        return startRestore(Direction::Redo);
    }
    notify();
    return false;
}

RenderEditHistory::Entry &RenderEditHistory::operationEntry() {
    return operation_->direction == Direction::Undo ? undo_.back() : redo_.back();
}

std::string RenderEditHistory::verb() const {
    return operation_ && operation_->direction == Direction::Redo ? "redone" : "undone";
}

bool RenderEditHistory::startRestore(Direction direction) {
    operation_ = Operation{};
    operation_->direction = direction;
    operation_->stage = Stage::Read;
    const auto &entry = operationEntry();
    // What the documents hold now is what the other direction puts back.
    auto references = Json::array();
    for (const auto &[reference, digest] :
         direction == Direction::Undo ? entry.after : entry.undone) {
        (void)digest;
        references.push_back(reference);
    }
    std::string error;
    operation_->request =
        send("get_render_documents", Json{{"references", std::move(references)}}, &error);
    if (operation_->request == 0) {
        failOperation(error.empty() ? std::string{"the request could not be sent"} : error);
        return false;
    }
    say(std::string{direction == Direction::Undo ? "Undoing" : "Redoing"} + " \"" +
        entry.label + "\"...");
    notify();
    return true;
}

void RenderEditHistory::sendRestore() {
    const auto &entry = operationEntry();
    const bool undoing = operation_->direction == Direction::Undo;
    const auto &contents = undoing ? entry.before : entry.redo;
    const auto &bases = undoing ? entry.after : entry.undone;
    auto documents = Json::array();
    for (const auto &[reference, document] : contents) {
        const auto base = bases.find(reference);
        if (base == bases.end()) continue;
        documents.push_back(Json{{"reference", reference},
                                 {"base_digest", base->second},
                                 {"content", document.content}});
    }
    operation_->stage = Stage::Restore;
    std::string error;
    operation_->request =
        send("restore_render_documents", Json{{"documents", std::move(documents)}}, &error);
    if (operation_->request == 0)
        failOperation(error.empty() ? std::string{"the request could not be sent"} : error);
}

void RenderEditHistory::pollOperation() {
    if (!operation_ || operation_->ticket.empty()) return;
    std::string error;
    operation_->request = send("get_edit_result", Json{{"ticket", operation_->ticket}}, &error);
    if (operation_->request == 0)
        failOperation(error.empty() ? std::string{"the request could not be sent"} : error);
}

void RenderEditHistory::finishOperation(const Json &commit) {
    const auto written = writtenOf(commit);
    const bool undoing = operation_->direction == Direction::Undo;
    auto entry = std::move(operationEntry());
    if (undoing) {
        undo_.pop_back();
        entry.redo.clear();
        entry.undone.clear();
        for (const auto &[reference, before] : entry.before) {
            const auto now = operation_->current.find(reference);
            if (now != operation_->current.end()) entry.redo[reference] = now->second;
            // The next base is what this commit wrote; a document it did not
            // list was already what was restored.
            const auto digest = written.find(reference);
            entry.undone[reference] = digest != written.end() && digest->is_string()
                                          ? digest->get<std::string>()
                                          : before.digest;
        }
    } else {
        redo_.pop_back();
        for (const auto &[reference, content] : entry.redo) {
            const auto digest = written.find(reference);
            entry.after[reference] = digest != written.end() && digest->is_string()
                                         ? digest->get<std::string>()
                                         : content.digest;
        }
    }
    say(std::string{undoing ? "Undid" : "Redid"} + " \"" + entry.label + "\".");
    const bool restart = entry.restart_required;
    (undoing ? redo_ : undo_).push_back(std::move(entry));
    operation_.reset();
    if (restored) restored(commit, restart);
    notify();
}

void RenderEditHistory::refuseOperation(const Json &result) {
    const auto error =
        result.is_object() ? result.value("error", Json::object()) : Json::object();
    const auto code = stringAt(error, "code");
    const auto message = stringAt(error, "message");
    const auto label = operationEntry().label;
    if (code == "external_modification") {
        auto reference = stringAt(error, "reference");
        if (reference.empty() && error.contains("mismatches") &&
            error.at("mismatches").is_array() && !error.at("mismatches").empty())
            reference = stringAt(error.at("mismatches").front(), "reference");
        if (reference.empty()) reference = "a render document";
        say("\"" + label + "\" cannot be " + verb() + ": " + reference +
                " changed since. Undo the later edit first, or refresh.");
    } else if (code == "render_pipeline_preflight_failed" && !message.empty()) {
        say(message);
    } else {
        say("\"" + label + "\" cannot be " + verb() + ": " +
                (message.empty() ? (code.empty() ? std::string{"the engine refused it"} : code)
                                 : message));
    }
    operation_.reset();
    notify();
}

void RenderEditHistory::failOperation(const std::string &message) {
    say("\"" + operationEntry().label + "\" cannot be " + verb() + ": " + message);
    operation_.reset();
    notify();
}

void RenderEditHistory::abandonInFlight() {
    ++epoch_;
    const bool had = operation_.has_value() || !pending_.empty() || unsent_.has_value();
    operation_.reset();
    pending_.clear();
    unsent_.reset();
    if (had) notify();
}

void RenderEditHistory::clear() {
    ++epoch_;
    operation_.reset();
    pending_.clear();
    unsent_.reset();
    undo_.clear();
    redo_.clear();
    line_.clear();
    notify();
}

} // namespace PelicanStudio
