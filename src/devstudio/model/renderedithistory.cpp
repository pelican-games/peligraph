#include "renderedithistory.hpp"

#include "renderconfigdocument.hpp"
#include "rendereditcompletion.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <queue>
#include <set>
#include <string_view>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

// How long a ticket is left before it is asked about again: the panels' own
// poll, so the engine sees nothing new.
constexpr int TicketPollMs = 25;

// The render edits: the ones that replace what documents hold (RD-1), and
// the ones that create or remove files (RD-1b), which RU-1 could not take
// back.
constexpr std::array<std::string_view, 25> RecordedMethods{
    "edit_preset_pass",
    "decompose_material_pass",
    // LP-4f2: a shipped graph template in place of a feature (GT-1).
    "apply_graph_template",
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
    "add_authored_pass",
    "remove_authored_pass",
    "fork_render_pipeline",
    "import_render_feature",
    // RSX: the rendering section's own keys (EL-3).
    "edit_rendering_keys",
    // GL-3a: a fold group of the root (FG-1), and a graph-wide planning
    // decision written from the Frame Plan (EL-2).
    "fold_nodes",
    "edit_target_planning",
    // UX-15b: a backend pin and a frozen physical plan (EL-2), from the
    // Frame Plan's Backends and Physical plan tabs.
    "edit_vulkan_plan_pins",
    "put_vulkan_physical_fragment",
    // GL-4a: the verified mark verify_fold writes (FG-2), after it runs.
    "verify_fold",
    // UX-12d: several edits in one ticket (FR-3), taken back as one.
    "edit_batch",
    // RTL-2s: the ray layout and its bindings (RTL-1a), render documents.
    "edit_ray_layout",
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

// RSX: the reading every panel uses.
bool committed(const Json &result) { return readRenderEditCompletion(result).committed; }

std::map<std::string, RenderEditHistory::Document> documentsOf(const Json &result) {
    std::map<std::string, RenderEditHistory::Document> documents;
    if (!result.is_object()) return documents;
    const auto found = result.find("documents");
    if (found == result.end() || !found->is_array()) return documents;
    for (const auto &document : *found) {
        const auto reference = stringAt(document, "reference");
        if (reference.empty()) continue;
        documents[reference] = RenderEditHistory::Document{
            stringAt(document, "digest"), stringAt(document, "content"),
            stringAt(document, "kind")};
    }
    return documents;
}

Json writtenOf(const Json &commit) {
    if (!commit.is_object()) return Json::object();
    const auto found = commit.find("documents");
    return found != commit.end() && found->is_object() ? *found : Json::object();
}

Json stringOrNull(const std::optional<std::string> &value) {
    return value ? Json(*value) : Json(nullptr);
}

// --- decision 3: placement.

// A root's scopes' placement: the root's own `layout` and each definition's
// (graphcomposition's set_layout -- "the one op that writes nothing the
// engine reads"). A graph.loop's branch bodies are definitions, and a
// definition holds no definitions of its own.
template <typename J> void eraseLayout(J &document) {
    document.erase("layout");
    const auto definitions = document.find("definitions");
    if (definitions == document.end() || !definitions->is_object()) return;
    for (auto &body : definitions->items())
        if (body.value().is_object()) body.value().erase("layout");
}

std::optional<Json> parsedObject(const std::string &content) {
    auto document = Json::parse(content, nullptr, false);
    if (document.is_discarded() || !document.is_object()) return std::nullopt;
    return document;
}

// A scope's `layout`, or null when it has none.
const Json *layoutOf(const Json *scope) {
    if (scope == nullptr || !scope->is_object()) return nullptr;
    const auto found = scope->find("layout");
    return found != scope->end() && found->is_object() ? &*found : nullptr;
}

const Json *definitionOf(const Json &document, const std::string &name) {
    const auto definitions = document.find("definitions");
    if (definitions == document.end() || !definitions->is_object()) return nullptr;
    const auto found = definitions->find(name);
    return found != definitions->end() ? &*found : nullptr;
}

// Per member of one scope's layout: a member `now` holds as `written` did
// (both absent counts) is `restored`'s, one that changed is `now`'s. nullopt:
// the scope has no layout (and `restored` had none).
std::optional<Json> mergedLayout(const Json *restored, const Json *written, const Json *now) {
    std::set<std::string> keys;
    for (const auto *layout : {restored, written, now})
        if (layout != nullptr)
            for (const auto &[key, value] : layout->items()) {
                (void)value;
                keys.insert(key);
            }
    const auto member = [](const Json *layout, const std::string &key) -> const Json * {
        if (layout == nullptr) return nullptr;
        const auto found = layout->find(key);
        return found != layout->end() ? &*found : nullptr;
    };
    Json merged = Json::object();
    for (const auto &key : keys) {
        const auto *before = member(restored, key);
        const auto *then = member(written, key);
        const auto *later = member(now, key);
        const bool unchanged =
            (then == nullptr) == (later == nullptr) && (then == nullptr || *then == *later);
        if (const auto *chosen = unchanged ? before : later; chosen != nullptr)
            merged[key] = *chosen;
    }
    if (merged.empty() && restored == nullptr) return std::nullopt;
    return merged;
}

} // namespace

RenderEditHistory::RenderEditHistory(Transport transport)
    : transport_(std::move(transport)) {}

bool RenderEditHistory::rewritesProceduralRecords(const std::string &method, const Json &params) {
    const auto rewrites = [](const Json &edit_params) {
        if (!edit_params.is_object()) return false;
        const auto found = edit_params.find("procedural_records");
        return found != edit_params.end() && found->is_object() && !found->empty();
    };
    if (method == "edit_ray_layout") return rewrites(params);
    if (method != "edit_batch" || !params.is_object()) return false;
    const auto edits = params.value("edits", Json::array());
    if (!edits.is_array()) return false;
    for (const auto &edit : edits)
        if (edit.is_object() && stringAt(edit, "method") == "edit_ray_layout" &&
            rewrites(edit.value("params", Json::object())))
            return true;
    return false;
}

bool RenderEditHistory::recordsMethod(const std::string &method) {
    return std::ranges::find(RecordedMethods, method) != RecordedMethods.end();
}

bool RenderEditHistory::followsMethod(const std::string &method) {
    return recordsMethod(method);
}

bool RenderEditHistory::differsOnlyInLayout(const std::string &left,
                                            const std::string &right) {
    auto a = parsedObject(left);
    auto b = parsedObject(right);
    if (!a || !b) return false;
    eraseLayout(*a);
    eraseLayout(*b);
    return *a == *b;
}

std::optional<std::string> RenderEditHistory::withLaterLayout(const std::string &restored,
                                                              const std::string &written,
                                                              const std::string &now) {
    const auto before = parsedObject(restored);
    const auto then = parsedObject(written);
    const auto later = parsedObject(now);
    if (!before || !then || !later) return std::nullopt;
    try {
        auto document = Pelican::AuthoredRenderConfigDocument::inspect(restored);
        // The root's own placement.
        const auto *before_root = layoutOf(&*before);
        const auto root = mergedLayout(before_root, layoutOf(&*then), layoutOf(&*later));
        if (root && (before_root == nullptr || *root != *before_root))
            document = document.withRootValue("layout", root->dump(2));
        // Each definition's, written back as the engine writes a definition
        // body it edited: the whole `definitions` member, and only when one
        // of them moved. Its own key order is kept.
        auto definitions = OrderedJson::parse(restored).value("definitions", OrderedJson{});
        bool moved = false;
        if (definitions.is_object()) {
            for (auto &body : definitions.items()) {
                if (!body.value().is_object()) continue;
                const auto *before_body = definitionOf(*before, body.key());
                const auto *before_layout = layoutOf(before_body);
                const auto merged =
                    mergedLayout(before_layout, layoutOf(definitionOf(*then, body.key())),
                                 layoutOf(definitionOf(*later, body.key())));
                if (!merged || (before_layout != nullptr && *merged == *before_layout))
                    continue;
                body.value()["layout"] = OrderedJson::parse(merged->dump());
                moved = true;
            }
        }
        if (moved) document = document.withRootValue("definitions", definitions.dump(2));
        return document.bytes();
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

bool RenderEditHistory::waitingFor(std::int64_t id) const noexcept {
    if (id == 0) return false;
    if (operation_ && operation_->request == id) return true;
    if (std::ranges::any_of(after_reads_,
                            [id](const AfterRead &read) { return read.request == id; }))
        return true;
    return std::ranges::any_of(pending_, [id](const PendingEdit &edit) {
        return edit.snapshot_request == id || edit.named_request == id ||
               edit.edit_request == id || edit.poll_request == id;
    });
}

std::string RenderEditHistory::labelFor(const std::string &method, const Json &params) {
    if (method == "edit_preset_pass")
        return named("Edit pass ", stringAt(params, "pass_name"), "Edit pass");
    if (method == "decompose_material_pass")
        return named("Decompose ", stringAt(params, "pass"), "Decompose");
    // LP-4f2: the feature is not among the params; the accepted answer names
    // it (applied.feature), and the entry is named again then.
    if (method == "apply_graph_template")
        return named("Replace with ", graphTemplateTitle(stringAt(params, "template")),
                     "Replace with loop version");
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
    if (method == "edit_ray_layout") return "Edit ray layout";
    if (method == "edit_region_replacement")
        return named("Replace region ", stringAt(params, "region"), "Replace region");
    if (method == "add_authored_pass")
        return named("Add pass ", stringAt(params.is_object() ? params.value("pass", Json{}) : Json{}, "name"),
                     "Add pass");
    if (method == "remove_authored_pass")
        return named("Remove pass ", stringAt(params, "fragment_reference"), "Remove pass");
    if (method == "fork_render_pipeline") return "Fork render pipeline";
    if (method == "import_render_feature") return "Import feature";
    // RSX: one key named, more than one as the panel's own name.
    if (method == "edit_rendering_keys") {
        const auto set = params.is_object() ? params.value("set", Json::object()) : Json::object();
        const auto remove = params.is_object() ? params.value("remove", Json::array()) : Json::array();
        const auto set_count = set.is_object() ? set.size() : 0;
        const auto remove_count = remove.is_array() ? remove.size() : 0;
        if (set_count == 1 && remove_count == 0) return "Set " + set.begin().key();
        if (set_count == 0 && remove_count == 1 && remove.front().is_string())
            return "Remove " + remove.front().get<std::string>();
        return "Edit rendering settings";
    }
    // GL-3a: a group written, or deleted (folded null).
    if (method == "fold_nodes") {
        const auto group = stringAt(params, "group");
        const bool removed = params.is_object() && params.contains("folded") &&
                             params.at("folded").is_null();
        return named(removed ? "Unfold " : "Fold group ", group, removed ? "Unfold" : "Fold group");
    }
    if (method == "edit_target_planning") return "Edit planning decisions";
    // UX-15b: a null package takes the decision away.
    if (method == "edit_vulkan_plan_pins") {
        const auto pin = params.is_object() ? params.value("pin", Json{}) : Json{};
        if (!pin.is_object()) return "Unpin backend";
        return named("Pin backend ", stringAt(pin.value("pins", Json::object()), "backend_candidate"),
                     "Pin backend");
    }
    if (method == "put_vulkan_physical_fragment")
        return params.is_object() && params.value("fragment", Json{}).is_object()
                   ? "Freeze physical plan"
                   : "Remove frozen physical plan";
    if (method == "verify_fold") return named("Verify ", stringAt(params, "group"), "Verify");
    // UX-12d: a batch is named by its last edit the history names itself
    // (the feature it adds, the change it sends again).
    if (method == "edit_batch") {
        const auto edits = params.is_object() ? params.value("edits", Json::array()) : Json::array();
        if (edits.is_array())
            for (auto edit = edits.rbegin(); edit != edits.rend(); ++edit)
                if (edit->is_object() && recordsMethod(stringAt(*edit, "method")))
                    return labelFor(stringAt(*edit, "method"), edit->value("params", Json::object()));
        return "Edit batch";
    }
    return method;
}

std::string RenderEditHistory::graphTemplateTitle(const std::string &reference) {
    // The engine's own rule when a template names no title
    // (renderconfigeditor.cpp): the file's name, spaces for underscores, the
    // first letter capital -- "engine://graphs/bloom_loop.json" is "Bloom loop".
    auto name = reference.substr(reference.rfind('/') == std::string::npos ? 0
                                                                           : reference.rfind('/') + 1);
    if (name.ends_with(".json")) name.resize(name.size() - 5);
    std::ranges::replace(name, '_', ' ');
    if (!name.empty() && name.front() >= 'a' && name.front() <= 'z')
        name.front() = static_cast<char>(name.front() - 'a' + 'A');
    return name;
}

std::string RenderEditHistory::featureName(const std::string &reference) {
    // "engine://features/bloom.json" -> "bloom".
    auto name = reference.substr(reference.rfind('/') == std::string::npos ? 0
                                                                           : reference.rfind('/') + 1);
    if (name.ends_with(".json")) name.resize(name.size() - 5);
    return name;
}

std::string RenderEditHistory::graphTemplateLabel(const Json &accepted) {
    const auto applied = accepted.is_object() ? accepted.value("applied", Json::object()) : Json{};
    const auto feature = stringAt(applied, "feature");
    const auto reference = stringAt(applied, "template");
    if (feature.empty() || reference.empty()) return {};
    return "Replace " + featureName(feature) + " with " + graphTemplateTitle(reference);
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

std::shared_ptr<RenderEditHistory::Movement> RenderEditHistory::observeMoves(Movement moved) {
    auto connection = std::make_shared<Movement>(std::move(moved));
    movement_observers_.push_back(connection);
    return connection;
}

void RenderEditHistory::notifyMoved(std::uint64_t serial, Move direction, bool ok, Completion completed) {
    // Bind the originating submission as part of Push, before subscribers
    // inspect the committed entry. Request identity ends at this point.
    if (direction == Move::Push && ok && completed) completed(serial);
    std::erase_if(movement_observers_, [](const auto &observer) { return observer.expired(); });
    const auto observers = movement_observers_;
    for (const auto &observer : observers)
        if (const auto moved = observer.lock()) (*moved)(serial, direction, ok);
    if (historyMoved) historyMoved(serial, direction, ok);
}

bool RenderEditHistory::followRequest(std::int64_t id, Completion completed) {
    const auto found = std::ranges::find(pending_, id, &PendingEdit::edit_request);
    if (found == pending_.end()) return false;
    found->completed = std::move(completed);
    return true;
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
    // RTL-2s-c: the files handed over for this request, whether it is
    // recorded or not.
    auto files = std::exchange(next_files_, {});
    if (!recordsMethod(method)) return;
    PendingEdit edit;
    edit.method = method;
    edit.base_source_digest = stringAt(params, "base_source_digest");
    edit.submitted = next_submission_++;
    // RTL-3s, decision 3 (engine EB-2): asset_data it writes is restored by
    // its digest (Entry::records_only).
    edit.records = rewritesProceduralRecords(method, params);
    edit.label = labelFor(method, params);
    edit.files = std::move(files);
    // The documents as they are before the edit: asked for first, so the
    // engine answers it before it takes the edit. Read without names, the
    // engine answers the root and the preset and features the composition
    // writes -- a fragment add_authored_pass wrote among them, since the
    // root's features name it.
    edit.snapshot_request = send("get_render_documents", Json::object(), nullptr);
    // What remove_authored_pass deletes is read by name as well, so its
    // content is at hand even if the composition did not list it.
    if (method == "remove_authored_pass")
        if (const auto fragment = stringAt(params, "fragment_reference"); !fragment.empty())
            edit.named_request = send("get_render_documents",
                                      Json{{"references", Json::array({fragment})}}, nullptr);
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
    for (std::size_t index = 0; index < after_reads_.size(); ++index) {
        if (after_reads_[index].request != id) continue;
        const auto serial = after_reads_[index].serial;
        after_reads_.erase(after_reads_.begin() + static_cast<std::ptrdiff_t>(index));
        receiveAfterRead(serial, result);
        return true;
    }
    for (std::size_t index = 0; index < pending_.size(); ++index) {
        auto &edit = pending_[index];
        if (id == edit.snapshot_request || id == edit.named_request) {
            const bool snapshot = id == edit.snapshot_request;
            (snapshot ? edit.snapshot_request : edit.named_request) = 0;
            if (status(result) != "rejected" && result.is_object() &&
                result.contains("documents")) {
                for (auto &[reference, document] : documentsOf(result))
                    edit.snapshot.try_emplace(reference, std::move(document));
                if (snapshot) edit.snapshot_received = true;
            }
            return true;
        }
        if (id == edit.edit_request && edit.ticket.empty() && edit.poll_request == 0) {
            const auto ticket = stringAt(result, "ticket");
            if (status(result) == "accepted" && !ticket.empty()) {
                edit.ticket = ticket;
                // LP-4f2: "Replace <feature> with <title>", from what the
                // engine says it applied.
                if (edit.method == "apply_graph_template")
                    if (const auto label = graphTemplateLabel(result); !label.empty())
                        edit.label = label;
                pollEdit(edit.edit_request);
                return true;
            }
            auto finished = std::move(edit);
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(index));
            if (committed(result)) finishEdit(std::move(finished), result);
            else if (finished.completed) finished.completed(0);
            return true;
        }
        if (id == edit.poll_request) {
            edit.poll_request = 0;
            // GL-4a: verify_fold says "running" while its runs draw.
            if (status(result) == "pending" || status(result) == "running") {
                const auto key = edit.edit_request;
                const auto epoch = epoch_;
                if (transport_.schedule)
                    // A run of seconds or minutes is asked about less often.
                    transport_.schedule(status(result) == "running" ? 500 : TicketPollMs, [this, key, epoch] {
                        if (epoch == epoch_) pollEdit(key);
                    });
                return true;
            }
            auto finished = std::move(edit);
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(index));
            if (committed(result)) finishEdit(std::move(finished), result);
            else if (finished.completed) finished.completed(0);
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
    for (std::size_t index = 0; index < after_reads_.size(); ++index) {
        if (after_reads_[index].request != id) continue;
        // Without it the entry is undone as RU-1 did: a later placement is
        // refused like any later change.
        after_reads_.erase(after_reads_.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }
    for (std::size_t index = 0; index < pending_.size(); ++index) {
        auto &edit = pending_[index];
        if (id == edit.snapshot_request) {
            // The edit is still followed; without its documents it can only
            // stand as a barrier.
            edit.snapshot_request = 0;
            return true;
        }
        if (id == edit.named_request) {
            edit.named_request = 0;
            return true;
        }
        if (id == edit.edit_request || id == edit.poll_request) {
            auto completed = std::move(edit.completed);
            if (completed) completed(0);
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
    if (found->poll_request == 0) {
        auto completed = std::move(found->completed);
        pending_.erase(found);
        if (completed) completed(0);
    }
}

void RenderEditHistory::finishEdit(PendingEdit edit, const Json &commit) {
    // Only an explicit no_change authorizes forgetting this submission.
    // A committed reply without its write set leaves an unconfirmed edit.
    if (commit.contains("no_change") && commit.at("no_change") == Json(true)) {
        if (edit.completed) edit.completed(0);
        return;
    }
    Entry entry;
    entry.label = edit.label;
    entry.serial = next_serial_++;
    entry.base_source_digest = std::move(edit.base_source_digest);
    entry.source_digest = stringAt(commit.value("source_digest", Json::object()), "hex");
    entry.submitted = edit.submitted;
    entry.files = std::move(edit.files);
    entry.procedural_records_before = commit.value("procedural_records_before", Json::object());
    entry.restart_required = readRenderEditCompletion(commit).restart_required;
    const auto documents = commit.find("documents");
    const bool valid_documents = documents != commit.end() && documents->is_object();
    const auto written = valid_documents ? *documents : Json::object();
    if (!valid_documents || written.empty()) {
        entry.barrier = true;
        entry.barrier_reason = "render_history.invalid_commit_response@1: " +
            std::string{documents == commit.end() ? "committed reply is missing documents"
                        : !valid_documents ? "committed reply documents must be an object"
                                           : "committed reply documents is empty without no_change: true"};
    } else if (!edit.snapshot_received) {
        entry.barrier = true;
        entry.barrier_reason = "its documents could not be read before it";
    }
    auto references = Json::array();
    for (const auto &[reference, digest] : written.items()) {
        const auto before = edit.snapshot.find(reference);
        // RTL-3s, decision 3: not a render document -- the asset_data the
        // edit's procedural_records wrote (no render document is made by
        // one). Restored by digest, content null; never read.
        if (edit.records && before == edit.snapshot.end() && digest.is_string()) {
            entry.records_only.insert(reference);
            entry.before[reference] = std::nullopt;
            entry.written[reference] = Written{digest.get<std::string>(), std::nullopt};
            continue;
        }
        // Not read before the edit: the edit created it (RD-1b: Undo deletes
        // it). Erased (null): the edit removed it, and Undo writes it again
        // from what was read -- if it was.
        entry.before[reference] =
            before != edit.snapshot.end() ? std::optional<Document>{before->second} : std::nullopt;
        if (digest.is_string()) {
            entry.written[reference] = Written{digest.get<std::string>(), std::nullopt};
            references.push_back(reference);
        } else {
            entry.written[reference] = std::nullopt;
            if (before == edit.snapshot.end() && !entry.barrier) {
                entry.barrier = true;
                entry.barrier_reason = "what it removed could not be read before it";
            }
        }
    }
    // Decision 3: what the commit wrote, read now, so a later change of
    // placement alone can be told from any other.
    if (!entry.barrier && !references.empty()) {
        const auto request =
            send("get_render_documents", Json{{"references", std::move(references)}}, nullptr);
        if (request != 0) after_reads_.push_back(AfterRead{request, entry.serial});
    }
    const auto serial = entry.serial;
    insertUndo(std::move(entry));
    redo_.clear();
    notifyMoved(serial, Move::Push, true, std::move(edit.completed));
    notify();
}

void RenderEditHistory::insertUndo(Entry entry) {
    // A digest link orders commits, even when independent pollers observe
    // the successor first. File-only entries commit synchronously, so their
    // relation to requests already sent is known without an engine digest.
    const auto precedes = [](const Entry &before, const Entry &after) {
        // Equal bytes may recur (A -> B -> A). Only the earlier submission
        // can be a predecessor: same-document stale candidates are refused
        // by the engine's CAS, not silently rebased into a different order.
        if (before.submitted >= after.submitted) return false;
        const auto files_only = [](const Entry &item) {
            return item.before.empty() && !item.files.empty() && !item.barrier;
        };
        if (files_only(before) || files_only(after)) return true;
        if (!after.base_source_digest.empty() &&
            after.base_source_digest != after.source_digest &&
            before.base_source_digest != before.source_digest &&
            before.source_digest == after.base_source_digest)
            return true;
        // The root digest need not move for an edit to a preset/feature.
        // Match that document's write to the successor's before snapshot.
        for (const auto &[reference, written] : before.written) {
            const auto snapshot = after.before.find(reference);
            if (written && !written->digest.empty() && snapshot != after.before.end() &&
                snapshot->second && snapshot->second->digest == written->digest)
                return true;
        }
        return false;
    };
    undo_.push_back(std::move(entry));
    // The normal in-order push costs one scan. With no known successor,
    // leave independent documents in observation order.
    const auto successor = std::find_if(undo_.begin(), std::prev(undo_.end()), [&](const Entry &other) {
        return precedes(undo_.back(), other);
    });
    if (successor == std::prev(undo_.end())) return;

    // A late middle link can join two previously disconnected chains (R3,
    // R1, then R2). Stable topological order fixes the entire joined chain,
    // not just the new entry. Observation order breaks ties. Nothing in the
    // affected suffix precedes the already ordered prefix, so leave it alone.
    const auto offset = static_cast<std::size_t>(std::distance(undo_.begin(), successor));
    const auto size = undo_.size() - offset;
    std::vector<std::size_t> incoming(size, 0);
    for (std::size_t i = 0; i < size; ++i)
        for (std::size_t j = 0; j < size; ++j)
            if (precedes(undo_[offset + i], undo_[offset + j])) ++incoming[j];
    std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>> ready;
    for (std::size_t i = 0; i < size; ++i)
        if (incoming[i] == 0) ready.push(i);
    std::vector<std::size_t> order;
    order.reserve(size);
    while (!ready.empty()) {
        const auto i = ready.top();
        ready.pop();
        order.push_back(i);
        for (std::size_t j = 0; j < size; ++j)
            if (precedes(undo_[offset + i], undo_[offset + j]) && --incoming[j] == 0) ready.push(j);
    }
    std::vector<Entry> sorted;
    sorted.reserve(size);
    for (const auto i : order) sorted.push_back(std::move(undo_[offset + i]));
    std::move(sorted.begin(), sorted.end(), successor);
}

void RenderEditHistory::receiveAfterRead(std::uint64_t serial, const Json &result) {
    if (status(result) == "rejected") return;
    for (auto *stack : {&undo_, &redo_}) {
        const auto entry = std::ranges::find(*stack, serial, &Entry::serial);
        if (entry == stack->end()) continue;
        for (auto &[reference, document] : documentsOf(result)) {
            const auto written = entry->written.find(reference);
            // Only the bytes that commit wrote: a later edit that landed
            // before this read is not the edit's.
            if (written != entry->written.end() && written->second &&
                written->second->digest == document.digest && !written->second->content)
                written->second->content = std::move(document.content);
        }
        return;
    }
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
        say("Cannot undo past \"" + undo_.back().label + "\": " + undo_.back().barrier_reason +
            ".");
    } else {
        return startRestore(Direction::Undo);
    }
    if (!undo_.empty()) notifyMoved(undo_.back().serial, Move::Undo, false);
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
    } else if (redo_.back().barrier) {
        say("Cannot redo past \"" + redo_.back().label + "\": " + redo_.back().barrier_reason +
            ".");
    } else {
        return startRestore(Direction::Redo);
    }
    if (!redo_.empty()) notifyMoved(redo_.back().serial, Move::Redo, false);
    notify();
    return false;
}

RenderEditHistory::Entry &RenderEditHistory::operationEntry() {
    return operation_->direction == Direction::Undo ? undo_.back() : redo_.back();
}

std::string RenderEditHistory::verb() const {
    return operation_ && operation_->direction == Direction::Redo ? "redone" : "undone";
}

void RenderEditHistory::recordFiles(std::string label, std::vector<FileChange> files, Completion completed) {
    if (files.empty()) {
        if (completed) completed(0);
        return;
    }
    Entry entry;
    entry.label = std::move(label);
    entry.serial = next_serial_++;
    entry.submitted = next_submission_++;
    entry.files = std::move(files);
    undo_.push_back(std::move(entry));
    redo_.clear();
    notifyMoved(undo_.back().serial, Move::Push, true, std::move(completed));
    notify();
}

bool RenderEditHistory::startRestore(Direction direction) {
    // RTL-3s, decision 4: files alone -- written now, nothing to restore.
    if (auto &files_only = direction == Direction::Undo ? undo_.back() : redo_.back();
        files_only.written.empty() && files_only.before.empty() && !files_only.files.empty()) {
        const bool undoing = direction == Direction::Undo;
        writeFiles(files_only, undoing);
        auto entry = std::move(files_only);
        const auto serial = entry.serial;
        (undoing ? undo_ : redo_).pop_back();
        say(std::string{undoing ? "Undid" : "Redid"} + " \"" + entry.label + "\".");
        (undoing ? redo_ : undo_).push_back(std::move(entry));
        notifyMoved(serial, undoing ? Move::Undo : Move::Redo, true);
        notify();
        return true;
    }
    operation_ = Operation{};
    operation_->direction = direction;
    operation_->stage = Stage::Read;
    const auto &entry = operationEntry();
    // What the documents hold now is what the other direction puts back. A
    // document the last commit removed is not there to read.
    auto references = Json::array();
    for (const auto &[reference, written] : entry.written)
        if (written && !entry.records_only.contains(reference)) references.push_back(reference);
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
    const auto &contents = undoing ? entry.before : entry.after;
    auto documents = Json::array();
    operation_->sent.clear();
    for (const auto &[reference, document] : contents) {
        const auto written = entry.written.find(reference);
        std::optional<std::string> base;
        if (written != entry.written.end() && written->second) base = written->second->digest;
        std::optional<std::string> content;
        if (document) content = document->content;
        // Not there on either side: nothing to do.
        if (!base && !content) continue;
        // Decision 3: the root changed since, in placement alone. The earlier
        // content goes back with the later placement, on the digest the root
        // has now; CAS still holds the whole document to what was read.
        const auto now = operation_->current.find(reference);
        if (base && content && now != operation_->current.end() && now->second.digest != *base &&
            now->second.kind == "root" && written->second->content &&
            differsOnlyInLayout(*written->second->content, now->second.content)) {
            if (auto moved = withLaterLayout(*content, *written->second->content,
                                             now->second.content)) {
                content = std::move(moved);
                base = now->second.digest;
            }
        }
        Json restore{{"reference", reference},
                     {"base_digest", stringOrNull(base)},
                     {"content", stringOrNull(content)}};
        if (entry.records_only.contains(reference) && entry.procedural_records_before.contains(reference))
            restore["procedural_records"] = entry.procedural_records_before.at(reference);
        documents.push_back(std::move(restore));
        operation_->sent[reference] = std::move(content);
    }
    operation_->stage = Stage::Restore;
    // RTL-2s-c: the files first -- the restore's commit compiles the shaders
    // the documents name.
    writeFiles(entry, undoing);
    std::string error;
    operation_->request =
        send("restore_render_documents", Json{{"documents", std::move(documents)}}, &error);
    if (operation_->request == 0)
        failOperation(error.empty() ? std::string{"the request could not be sent"} : error);
}

void RenderEditHistory::writeFiles(const Entry &entry, bool undoing) {
    for (const auto &file : entry.files) {
        const std::filesystem::path path{file.path};
        const std::optional<std::string> content = undoing ? file.before : std::optional<std::string>{file.after};
        std::error_code error;
        if (!content) {
            std::filesystem::remove(path, error);
            continue;
        }
        std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream output{path, std::ios::binary | std::ios::trunc};
        output.write(content->data(), static_cast<std::streamsize>(content->size()));
    }
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
    const auto serial = entry.serial;
    (undoing ? undo_ : redo_).pop_back();
    // What this restore replaced is what the other direction puts back.
    auto &replaced = undoing ? entry.after : entry.before;
    const auto &restored_side = undoing ? entry.before : entry.after;
    std::map<std::string, std::optional<Written>> next;
    for (const auto &[reference, document] : restored_side) {
        (void)document;
        const auto now = operation_->current.find(reference);
        replaced[reference] = now != operation_->current.end()
                                  ? std::optional<Document>{now->second}
                                  : std::nullopt;
        const auto sent = operation_->sent.find(reference);
        const auto digest = written.find(reference);
        if (digest != written.end()) {
            // Written (the content it sent), or deleted (null).
            if (digest->is_string())
                next[reference] = Written{digest->get<std::string>(),
                                          sent != operation_->sent.end() ? sent->second
                                                                         : std::nullopt};
            else
                next[reference] = std::nullopt;
        } else if (now != operation_->current.end()) {
            // Not listed: it already held what was restored.
            next[reference] = Written{now->second.digest, now->second.content};
        } else {
            next[reference] = std::nullopt;
        }
    }
    entry.written = std::move(next);
    entry.procedural_records_before = commit.value("procedural_records_before", Json::object());
    say(std::string{undoing ? "Undid" : "Redid"} + " \"" + entry.label + "\".");
    // RSX: the restore's own answer counts too -- taking an edit back can
    // be what changes a material's physical contract.
    const bool restart =
        entry.restart_required || readRenderEditCompletion(commit).restart_required;
    (undoing ? redo_ : undo_).push_back(std::move(entry));
    operation_.reset();
    notifyMoved(serial, undoing ? Move::Undo : Move::Redo, true);
    if (restored) restored(commit, restart);
    notify();
}

void RenderEditHistory::refuseOperation(const Json &result) {
    const auto error =
        result.is_object() ? result.value("error", Json::object()) : Json::object();
    const auto code = stringAt(error, "code");
    const auto message = stringAt(error, "message");
    auto &entry = operationEntry();
    const auto serial = entry.serial;
    const auto direction = operation_->direction == Direction::Undo ? Move::Undo : Move::Redo;
    const auto label = entry.label;
    const auto reason = stringAt(error, "reason");
    if (code == "external_modification") {
        auto reference = stringAt(error, "reference");
        if (reference.empty() && error.contains("mismatches") &&
            error.at("mismatches").is_array() && !error.at("mismatches").empty())
            reference = stringAt(error.at("mismatches").front(), "reference");
        if (reference.empty()) reference = "a render document";
        say("\"" + label + "\" cannot be " + verb() + ": " + reference +
                " changed since. Undo the later edit first, or refresh.");
    } else if (code == "render_document_not_writable" &&
               (reason == "reserved_location" || reason == "root_document")) {
        // RD-1b: a place the engine does not create or delete a document in.
        // Asking again would be refused again: the entry is where Undo (or
        // Redo) stops, and it stays in the history.
        auto reference = stringAt(error, "reference");
        if (reference.empty()) reference = "a render document";
        entry.barrier = true;
        entry.barrier_reason =
            reason == "reserved_location"
                ? "it would create or delete " + reference + " under " +
                      stringAt(error, "location") +
                      ", where the project keeps documents that are not render documents"
                : "it would delete the root " + reference;
        say("\"" + label + "\" cannot be " + verb() + ": " + entry.barrier_reason + ".");
    } else if (code == "render_document_not_writable" && reason == "procedural_records_only") {
        // RTL-3s, decision 3 (engine EB-2): the engine keeps the procedural
        // records an edit replaced only while it runs.
        entry.barrier = true;
        entry.barrier_reason = "the engine started again since, and it keeps the procedural records an edit "
                               "replaced only while it runs; set them again in the Ray layout section";
        say("\"" + label + "\" cannot be " + verb() + ": " + entry.barrier_reason + ".");
    } else if (code == "render_pipeline_preflight_failed" && !message.empty()) {
        say(message);
    } else {
        say("\"" + label + "\" cannot be " + verb() + ": " +
                (message.empty() ? (code.empty() ? std::string{"the engine refused it"} : code)
                                 : message));
    }
    // RTL-2s-c: not restored: the files back as the documents still are.
    if (operation_->stage != Stage::Read) writeFiles(entry, operation_->direction != Direction::Undo);
    operation_.reset();
    notifyMoved(serial, direction, false);
    notify();
}

void RenderEditHistory::failOperation(const std::string &message) {
    const auto serial = operationEntry().serial;
    const auto direction = operation_->direction == Direction::Undo ? Move::Undo : Move::Redo;
    say("\"" + operationEntry().label + "\" cannot be " + verb() + ": " + message);
    if (operation_->stage != Stage::Read)
        writeFiles(operationEntry(), operation_->direction != Direction::Undo);
    operation_.reset();
    notifyMoved(serial, direction, false);
    notify();
}

void RenderEditHistory::abandonInFlight() {
    ++epoch_;
    const bool had = operation_.has_value() || !pending_.empty() || unsent_.has_value() ||
                     !after_reads_.empty();
    operation_.reset();
    auto abandoned = std::exchange(pending_, {});
    for (auto &edit : abandoned)
        if (edit.completed) edit.completed(0);
    unsent_.reset();
    after_reads_.clear();
    if (had) notify();
}

void RenderEditHistory::clear() {
    ++epoch_;
    operation_.reset();
    auto abandoned = std::exchange(pending_, {});
    for (auto &edit : abandoned)
        if (edit.completed) edit.completed(0);
    unsent_.reset();
    after_reads_.clear();
    undo_.clear();
    redo_.clear();
    line_.clear();
    notify();
}

void RenderEditHistory::cut(std::string line) {
    undo_.clear();
    redo_.clear();
    say(std::move(line));
    notify();
}

} // namespace PelicanStudio
