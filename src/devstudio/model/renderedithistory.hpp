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
// same the other way. An edit that created or removed a file cannot be
// restored by RD-1 and stands in the history as a barrier.
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
    };

    // One item of the history, as the UI names it.
    struct Entry {
        std::string label;
        // The edit created or removed a file (or is one of the edits RD-1
        // cannot put back): Undo stops here.
        bool barrier = false;
        // {reference: document} as it was before the edit.
        std::map<std::string, Document> before;
        // {reference: digest} as the edit's commit wrote them: the base an
        // Undo sends.
        std::map<std::string, std::string> after;
        // What Undo read just before it restored (Redo's content), and the
        // digests Undo's commit wrote (Redo's base).
        std::map<std::string, Document> redo;
        std::map<std::string, std::string> undone;
        // The edit's commit asked for an engine restart (a setting read at
        // start); restoring it asks again.
        bool restart_required = false;
    };

    explicit RenderEditHistory(Transport transport);

    // The render edits this history records, and those it records as a
    // barrier (they create or remove files). Anything else it ignores.
    [[nodiscard]] static bool recordsMethod(const std::string &method);
    // Either of the two: a request the history has to hear about.
    [[nodiscard]] static bool followsMethod(const std::string &method);
    [[nodiscard]] static bool barrierMethod(const std::string &method, const Json &params);
    // "Edit pass present", "Decompose deferred_geometry", ...
    [[nodiscard]] static std::string labelFor(const std::string &method, const Json &params);

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

    // The stacks, the busy state or the line changed.
    std::function<void()> changed;
    // A restore committed: its commit result (source_digest, documents) and
    // whether the entry asks for an engine restart.
    std::function<void(const Json &commit, bool restart_required)> restored;

  private:
    struct PendingEdit {
        std::string method;
        std::string label;
        bool barrier = false;
        std::int64_t snapshot_request = 0;
        bool snapshot_received = false;
        std::map<std::string, Document> snapshot;
        std::int64_t edit_request = 0;
        std::string ticket;
        std::int64_t poll_request = 0;
    };
    enum class Direction { Undo, Redo };
    enum class Stage { Read, Restore, Poll };
    struct Operation {
        Direction direction = Direction::Undo;
        Stage stage = Stage::Read;
        std::int64_t request = 0;
        std::string ticket;
        std::map<std::string, Document> current;
    };

    Transport transport_;
    std::vector<Entry> undo_;
    std::vector<Entry> redo_;
    std::string line_;
    std::uint64_t line_serial_ = 0;
    // An edit whose beforeRequest ran and whose requestSent has not.
    std::optional<PendingEdit> unsent_;
    std::vector<PendingEdit> pending_;
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
