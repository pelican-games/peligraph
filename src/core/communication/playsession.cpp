#include "playsession.hpp"

#include "../loader/basicconfig.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Pelican::internal {

PlaySessionUnknownTransaction::PlaySessionUnknownTransaction(
    std::string transaction_id, std::vector<PlaySessionEdit> recorded)
    : std::runtime_error{"set_play_state: keep names '" + transaction_id +
                         "', which this play session did not record"},
      transaction_id_{std::move(transaction_id)},
      recorded_{std::move(recorded)} {}

void PlaySession::begin(const ProjectBasicConfig &config,
                        const EditorEditCoordinator *editor) {
    Saved saved{
        .document = config.sceneDocument(),
        .scene_id = config.currentSceneId(),
    };
    if (editor != nullptr) {
        saved.history = editor->snapshotHistory();
        saved.journal_start = saved.history->journal.size();
    }
    saved_ = std::move(saved);
}

std::vector<PlaySessionEdit>
PlaySession::recordedEdits(const EditorEditCoordinator *editor) const {
    std::vector<PlaySessionEdit> result;
    if (!saved_ || editor == nullptr) return result;
    const auto &journal = editor->journal();
    for (auto index = saved_->journal_start; index < journal.size(); ++index) {
        const auto &record = journal[index];
        if ((record.operation_kind == "undo" || record.operation_kind == "redo") &&
            record.source_transaction_id) {
            const auto reverted = std::find_if(
                result.begin(), result.end(), [&](const PlaySessionEdit &edit) {
                    return edit.transaction_id == *record.source_transaction_id;
                });
            if (reverted != result.end()) {
                result.erase(reverted);
                continue;
            }
        }
        result.push_back({record.transaction_id, record.actor_id});
    }
    return result;
}

std::vector<EditorJournalRecord>
PlaySession::keptRecords(const PlaySessionKeep &keep,
                         const EditorEditCoordinator *editor) const {
    const auto recorded = recordedEdits(editor);
    for (const auto &id : keep.transaction_ids) {
        const bool known = std::any_of(recorded.begin(), recorded.end(),
                                       [&](const PlaySessionEdit &edit) {
                                           return edit.transaction_id == id;
                                       });
        if (!known) throw PlaySessionUnknownTransaction{id, recorded};
    }
    std::vector<EditorJournalRecord> result;
    if (recorded.empty() || editor == nullptr) return result;
    const auto &journal = editor->journal();
    for (const auto &edit : recorded) {
        const bool named =
            keep.all || std::find(keep.transaction_ids.begin(),
                                  keep.transaction_ids.end(),
                                  edit.transaction_id) != keep.transaction_ids.end();
        if (!named) continue;
        const auto record = std::find_if(
            journal.begin() + static_cast<std::ptrdiff_t>(saved_->journal_start),
            journal.end(), [&](const EditorJournalRecord &candidate) {
                return candidate.transaction_id == edit.transaction_id;
            });
        if (record == journal.end()) {
            throw std::logic_error("a recorded play session edit left the journal: " +
                                   edit.transaction_id);
        }
        result.push_back(*record);
    }
    return result;
}

const AuthoringSceneDocument &PlaySession::savedDocument() const {
    if (!saved_) throw std::logic_error("no play session is running");
    return saved_->document;
}

const SceneId &PlaySession::savedSceneId() const {
    if (!saved_) throw std::logic_error("no play session is running");
    return saved_->scene_id;
}

std::optional<EditorHistorySnapshot> PlaySession::takeHistory() {
    if (!saved_) return std::nullopt;
    return std::exchange(saved_->history, std::nullopt);
}

} // namespace Pelican::internal
