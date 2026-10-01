#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace PelicanStudio {

// RU-1. The one history of Studio's render edits -- the Frame Plan's canvas,
// the Pass Editor, the Task Editor and Render Features all send them, and all
// share this. The Graph tab keeps its own history (op inverses) and the
// Inspector the engine's scene journal; neither is merged in here.
//
// It rides on RD-1 and on nothing the panels do. Every request a panel sends
// passes beforeRequest / requestSent: a render edit is preceded by a
// get_render_documents of its own (the documents as they are before the edit,
// read before the edit is on the wire), and the history follows the edit's
// ticket to its commit. The commit's `documents` ({reference: digest}, what
// it wrote) picks the documents the entry keeps and is the base_digest its
// Undo sends. Undo reads the documents as they are now (what Redo will put
// back) and restores the ones before the edit under that CAS; Redo is the
// same the other way.
//
// RU-1b, on RD-1b: an edit that created a document (add_authored_pass, fork,
// Make editable, import, a first render target) is taken back by deleting it
// (content null, base the digest the commit named), and one that removed a
// document (remove_authored_pass, a removed feature) by writing it again
// (base_digest null, the content read before the edit). Redo is the reverse.
// An edit whose documents could not be read before it, or whose restore the
// engine refuses as a place it does not create or delete in, is where Undo
// stops.
//
// RU-1b, decision 3: the placement a panel saves (`layout`, of the root and
// of each definition) is something the engine carries and never reads, and
// the Frame Plan and the Graph tab write it as a node is moved. The history
// keeps the content each commit wrote; when the root now differs from it only
// in placement, Undo restores the earlier root with the later placement, on
// the digest the root has now. Any other difference is refused as before.
//
// Qt-free: the transport sends a request and answers its id, and every reply
// comes back through receiveResult / receiveFailure.
class RenderEditHistory {
  public:
    using Json = nlohmann::json;

    struct Transport {
        // Sends one request; the id its reply comes back under, 0 when it
        // could not be sent (`error` says why).
        std::function<std::int64_t(const std::string &method, const Json &params,
                                   std::string *error)>
            request;
        // Runs `task` after `milliseconds` (the ticket poll).
        std::function<void(int milliseconds, std::function<void()> task)> schedule;
    };

    // One render document as get_render_documents answers it.
    struct Document {
        std::string digest;
        std::string content;
        // root, preset or feature.
        std::string kind;
    };
    // {reference: document} on one side of an edit; nullopt where the
    // document is not there on that side.
    using Side = std::map<std::string, std::optional<Document>>;
    // What a commit wrote to one document: its digest, and its content when
    // the history knows it (read right after the edit's commit, or the
    // content an Undo or a Redo sent).
    struct Written {
        std::string digest;
        std::optional<std::string> content;
    };

    // One item of the history, as the UI names it.
    struct Entry {
        std::string label;
        // Undo (or Redo) stops here; `barrier_reason` says why.
        bool barrier = false;
        std::string barrier_reason;
        // The documents as they were before the edit: what Undo puts back.
        Side before;
        // The documents as Undo found them: what Redo puts back.
        Side after;
        // What the last commit on this entry wrote (the edit or its Redo for
        // an entry on the undo stack, its Undo for one on the redo stack):
        // the base the next restore sends. nullopt: it removed the document.
        std::map<std::string, std::optional<Written>> written;
        // The edit's commit asked for an engine restart (a setting read at
        // start); restoring it asks again.
        bool restart_required = false;
        // Names the entry for the read that follows the edit's commit.
        std::uint64_t serial = 0;
    };

    explicit RenderEditHistory(Transport transport);

    // The render edits this history records -- those that replace what
    // documents hold, and those that create or remove them. Anything else it
    // ignores.
    [[nodiscard]] static bool recordsMethod(const std::string &method);
    // A request the history has to hear about (the same set).
    [[nodiscard]] static bool followsMethod(const std::string &method);
    // "Edit pass present", "Decompose deferred_geometry", ...
    [[nodiscard]] static std::string labelFor(const std::string &method, const Json &params);
    // LP-4f2. apply_graph_template's entry: "Replace <feature> with <title>",
    // from the accepted answer's applied.feature and applied.template; empty
    // when it names neither. The title is the template file's name as the
    // engine makes it ("Bloom loop"), the feature its file's ("bloom").
    [[nodiscard]] static std::string graphTemplateLabel(const Json &accepted);
    [[nodiscard]] static std::string graphTemplateTitle(const std::string &reference);
    [[nodiscard]] static std::string featureName(const std::string &reference);
    // RU-1b, decision 3: whether two contents of a root differ in nothing
    // but placement -- the root's `layout` and each definition's -- which
    // the engine carries and never reads (graphcomposition's set_layout).
    // False when either is not a JSON object.
    [[nodiscard]] static bool differsOnlyInLayout(const std::string &left,
                                                  const std::string &right);
    // `restored` (the root a restore puts back) with the placement that
    // changed between `written` (what the last commit wrote) and `now`: per
    // scope (the root, each definition `restored` holds) and per member of
    // its layout, a member `now` holds as `written` did keeps `restored`'s,
    // and one that changed is `now`'s. The rest of `restored` stays byte for
    // byte. nullopt when a content does not read as a root.
    [[nodiscard]] static std::optional<std::string>
    withLaterLayout(const std::string &restored, const std::string &written,
                    const std::string &now);

    // Around every request anybody sends through the transport. beforeRequest
    // runs before the request is written, requestSent after (id 0: it was not
    // sent). The history's own requests pass here too and are ignored.
    void beforeRequest(const std::string &method, const Json &params);
    void requestSent(std::int64_t id, const std::string &method, const Json &params);

    // Whether a reply under `id` is one the history is waiting for.
    [[nodiscard]] bool waitingFor(std::int64_t id) const noexcept;
    // Every reply. False when the id is not one the history is waiting for.
    bool receiveResult(std::int64_t id, const Json &result);
    bool receiveFailure(std::int64_t id, const std::string &message);

    // False when nothing was sent; line() says why.
    bool undo();
    bool redo();

    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    [[nodiscard]] bool busy() const noexcept { return operation_.has_value(); }
    // The item the next Undo / Redo is about; empty when there is none.
    [[nodiscard]] std::string undoLabel() const;
    [[nodiscard]] std::string redoLabel() const;
    [[nodiscard]] const std::vector<Entry> &undoStack() const noexcept { return undo_; }
    [[nodiscard]] const std::vector<Entry> &redoStack() const noexcept { return redo_; }
    // The one line about the last Undo / Redo (or why it was not sent).
    [[nodiscard]] const std::string &line() const noexcept { return line_; }
    // Counts the lines said, so the same line said twice is shown twice.
    [[nodiscard]] std::uint64_t lineSerial() const noexcept { return line_serial_; }

    // The engine went away: whatever was in flight is dropped, the history
    // stays (its documents are on disk, and CAS guards them).
    void abandonInFlight();
    // Another project: nothing of this history applies to it.
    void clear();
    // RL-1s: the documents were read again from disk (reload_render_documents,
    // which is not an entry). What the stacks hold was written before it, so
    // Undo does not reach back past it; `line` marks where it was cut.
    void cut(std::string line);

    // The stacks, the busy state or the line changed.
    std::function<void()> changed;
    // A restore committed: its commit result (source_digest, documents) and
    // whether the entry asks for an engine restart.
    std::function<void(const Json &commit, bool restart_required)> restored;

  private:
    struct PendingEdit {
        std::string method;
        std::string label;
        std::int64_t snapshot_request = 0;
        bool snapshot_received = false;
        // remove_authored_pass: the fragment it removes, read by name as
        // well, in case the documents the composition writes do not list it.
        std::int64_t named_request = 0;
        Side snapshot;
        std::int64_t edit_request = 0;
        std::string ticket;
        std::int64_t poll_request = 0;
    };
    // The read of what an edit's commit wrote, for decision 3.
    struct AfterRead {
        std::int64_t request = 0;
        std::uint64_t serial = 0;
    };
    enum class Direction { Undo, Redo };
    enum class Stage { Read, Restore, Poll };
    struct Operation {
        Direction direction = Direction::Undo;
        Stage stage = Stage::Read;
        std::int64_t request = 0;
        std::string ticket;
        // The documents as they are now (those there to read).
        std::map<std::string, Document> current;
        // The content the restore sent for each document (nullopt: deleted).
        std::map<std::string, std::optional<std::string>> sent;
    };

    Transport transport_;
    std::vector<Entry> undo_;
    std::vector<Entry> redo_;
    std::string line_;
    std::uint64_t line_serial_ = 0;
    std::uint64_t next_serial_ = 1;
    // An edit whose beforeRequest ran and whose requestSent has not.
    std::optional<PendingEdit> unsent_;
    std::vector<PendingEdit> pending_;
    std::vector<AfterRead> after_reads_;
    std::optional<Operation> operation_;
    // The history's own request is on its way through the transport.
    bool own_request_ = false;
    // Bumped by clear / abandon: a scheduled poll from before is stale.
    std::uint64_t epoch_ = 0;

    std::int64_t send(const std::string &method, const Json &params, std::string *error);
    void notify();
    void say(std::string line);
    void pollEdit(std::int64_t edit_request);
    void finishEdit(PendingEdit edit, const Json &commit);
    void receiveAfterRead(std::uint64_t serial, const Json &result);
    bool startRestore(Direction direction);
    void sendRestore();
    void pollOperation();
    void finishOperation(const Json &commit);
    void refuseOperation(const Json &result);
    void failOperation(const std::string &message);
    [[nodiscard]] Entry &operationEntry();
    [[nodiscard]] std::string verb() const;
};

} // namespace PelicanStudio
