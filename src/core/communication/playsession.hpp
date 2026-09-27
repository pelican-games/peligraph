#pragma once

#include "editorjournal.hpp"
#include "../loader/authoringscenedocument.hpp"
#include "../loader/sceneload.hpp"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Pelican {

class ProjectBasicConfig;

namespace internal {

// PM-2. A play session (docs/design_play_mode.md 6 and 7): what edit ->
// playing / paused saves so that stopping can put the scene's authoring and
// the editor's history back, and which edits were committed while it ran.
//
// The recorded edits are read from the editor journal past the point the
// session began, not kept beside it: an edit is recorded when it commits, an
// undo or redo takes out the recorded entry it reverts (by its
// source_transaction_id) and is recorded itself only when that entry is not
// one of the session's (an undo of an edit made before the session).
struct PlaySessionEdit {
    std::string transaction_id;
    EditorActorId actor_id{};
};

// What `keep` named: every recorded edit, or these transaction ids.
struct PlaySessionKeep {
    bool all = false;
    std::vector<std::string> transaction_ids;
};

// A `keep` naming a transaction the session did not record.
class PlaySessionUnknownTransaction final : public std::runtime_error {
    std::string transaction_id_;
    std::vector<PlaySessionEdit> recorded_;

  public:
    PlaySessionUnknownTransaction(std::string transaction_id,
                                  std::vector<PlaySessionEdit> recorded);
    const std::string &transactionId() const noexcept { return transaction_id_; }
    const std::vector<PlaySessionEdit> &recorded() const noexcept { return recorded_; }
};

class PlaySession {
    struct Saved {
        AuthoringSceneDocument document;
        SceneId scene_id;
        std::optional<EditorHistorySnapshot> history;
        std::size_t journal_start = 0;
    };
    std::optional<Saved> saved_;

  public:
    bool active() const noexcept { return saved_.has_value(); }

    // edit -> playing / paused: saves the authoring document (all its scenes
    // and AuthoringObjectIds), the current scene and the editor's history.
    // The authority is not kept: the load that puts the authoring back gives
    // a fresh one, whose epoch is what closes the editor gate. `editor` may
    // be null (no editor surface).
    void begin(const ProjectBasicConfig &config,
               const EditorEditCoordinator *editor);
    void discard() noexcept { saved_.reset(); }

    // The edits committed since the session began, in the order they stand.
    // Empty with no session.
    std::vector<PlaySessionEdit>
    recordedEdits(const EditorEditCoordinator *editor) const;

    // The journal records `keep` names, copied in the recorded order. Throws
    // PlaySessionUnknownTransaction when it names one that is not recorded
    // (with no session, nothing is).
    std::vector<EditorJournalRecord>
    keptRecords(const PlaySessionKeep &keep,
                const EditorEditCoordinator *editor) const;

    const AuthoringSceneDocument &savedDocument() const;
    const SceneId &savedSceneId() const;
    std::optional<EditorHistorySnapshot> takeHistory();
};

} // namespace internal
} // namespace Pelican
