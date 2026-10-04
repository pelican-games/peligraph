#pragma once

#include "inspectormodel.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace PelicanStudio {

// BS-1s. The engine Studio starts with --render-fallback keeps running when
// the project's rendering configuration is refused at start: it draws one
// flat colour and says why in get_status.render_pipeline (engine BS-1).
//
//   2. The notice. A reading of `fallback` is one sentence (the code's, from
//      the tables every panel reads refusals with) and where to fix it: the
//      file the refusal names (Open file) and the node (Open in Graph).
//   3. Reload from disk asks reload_render_documents. Any reply that may
//      have published another configuration -- a reload or a render edit
//      that committed -- is followed by a get_status while the fallback
//      lasts, and a reading of `ready` takes the notice away.
//   4. A fixed configuration whose feature needs a module this engine was
//      started without is refused by the reload: "Restart the engine to
//      enable <feature>", with the restart.
//
// Qt-free: the owner hands it every get_status reply, the reload's refusals
// and the replies of render edits, and shows notice().
class RenderFallbackSession {
  public:
    using Json = nlohmann::json;

    struct Notice {
        // Nothing to say: the row is hidden.
        bool shown = false;
        std::string line;
        // The engine's own words, for the tooltip and Copy message.
        std::string detail;
        // The file the refusal names, as the project names it; empty: none.
        std::string document;
        // The node the refusal names; empty: none.
        std::string node;
        // Decision 4: what only a restart enables; empty: no restart asked.
        std::string restart_feature;
        // Decision 8 (engine BS-1b): the refusal is render_document_syntax@1,
        // and where the parser stopped (0: not given). Show the document.
        bool syntax = false;
        std::int64_t syntax_line = 0;
        std::int64_t syntax_column = 0;

        bool operator==(const Notice &) const = default;
    };

    // A get_status (or get_frame_plan) reply. True when the notice changed.
    bool observeStatus(const Json &reply);
    // A reload_render_documents refusal ({code, message, ...}). True when the
    // notice changed.
    bool observeReloadRefusal(const Json &error);
    // Whether `reply` (a render edit's or a reload's) may have published
    // another configuration while the fallback lasts: get_status is read
    // again after it.
    [[nodiscard]] bool wantsStatusAfter(const Json &reply) const;
    // The engine went away (stopped, restarted, another project).
    void engineGone();

    [[nodiscard]] bool fallback() const noexcept {
        return reading_.state == RenderPipelineReading::State::Fallback;
    }
    [[nodiscard]] const RenderPipelineReading &reading() const noexcept { return reading_; }
    [[nodiscard]] const Notice &notice() const noexcept { return notice_; }
    // The reason the restart is asked with (requestEngineRestart's line).
    [[nodiscard]] std::string restartReason() const;

  private:
    RenderPipelineReading reading_;
    std::string restart_feature_;
    std::string restart_message_;
    Notice notice_;

    bool compose();
};

// BS-1s, decision 8: where the parser stops in `text`; nullopt when it is
// JSON. line and column are 1-based (0 when the parser named none).
struct RenderDocumentSyntaxSpot {
    std::int64_t line = 0;
    std::int64_t column = 0;
    std::string reason;
};
[[nodiscard]] std::optional<RenderDocumentSyntaxSpot> renderDocumentSyntaxOf(std::string_view text);

// BS-1s, decision 8 (engine BS-1b). The way out when a render document is not
// JSON at all: its bytes as get_render_documents lists them (`content`, the
// raw string, and `digest`, their sha256), fixed by hand and written back
// whole with restore_render_documents {documents: [{reference, base_digest,
// content}]} -- the engine writes the file too. A restore is a ticket; its
// commit is not taken here, so the owner reads it as every commit (and reads
// get_status again). Not the way documents are edited otherwise.
class RenderDocumentRepair {
  public:
    using Json = nlohmann::json;

    static constexpr int TicketPollMs = 100;

    struct Transport {
        // Sends one request; the id its reply comes back under, 0 when it
        // could not be sent (`error` says why).
        std::function<std::int64_t(const std::string &method, const Json &params, std::string *error)> request;
        // Runs `task` after `milliseconds`.
        std::function<void(int milliseconds, std::function<void()> task)> schedule;
        // PB-1s: writes the document's file (`reference` as the engine names
        // it) whole, as an editor of the person's own would; the error, empty
        // when it was written.
        std::function<std::string(const std::string &reference, const std::string &content)> write_file;
    };

    enum class State { Closed, Reading, Editing, Saving };

    explicit RenderDocumentRepair(Transport transport);

    // Show the document: get_render_documents, then the one `document` names
    // (as the project names it, or project://...). line / column: where the
    // editor puts the cursor.
    // PB-1s: `to_file` -- the engine runs the version before the file was
    // saved (a reload of it was refused), and restore_render_documents
    // compares against that version and the file being the same: Save
    // writes the file itself (Transport::write_file), and Studio's reload of
    // saved files (RL-1s) takes it in.
    bool open(const std::string &document, std::int64_t line, std::int64_t column, bool to_file = false);
    // Save: refused (false, with notice()) while `content` is not JSON;
    // otherwise restore_render_documents against the digest read.
    bool save(const std::string &content);
    void close();
    // The engine went away: nothing in flight is answered.
    void engineGone();

    // True when the reply was this object's and nothing else should read it.
    // A committed restore is answered false: the owner reads it as a commit.
    bool receiveResult(std::int64_t id, const Json &result);
    bool receiveFailure(std::int64_t id, const std::string &message);
    [[nodiscard]] bool owns(std::int64_t id) const noexcept {
        return id != 0 && (id == list_request_ || id == restore_request_ || id == poll_request_);
    }

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] const std::string &reference() const noexcept { return reference_; }
    [[nodiscard]] const std::string &digest() const noexcept { return digest_; }
    [[nodiscard]] const std::string &content() const noexcept { return content_; }
    [[nodiscard]] std::int64_t line() const noexcept { return line_; }
    [[nodiscard]] std::int64_t column() const noexcept { return column_; }
    [[nodiscard]] const std::string &notice() const noexcept { return notice_; }

    // Anything shown may read differently.
    std::function<void()> changed;
    // The restore committed (its result).
    std::function<void(const Json &commit)> restored;
    // PB-1s: the file was written (open's `to_file`).
    std::function<void(const std::string &reference)> written;

  private:
    Transport transport_;
    State state_ = State::Closed;
    std::string wanted_;
    std::string reference_;
    std::string digest_;
    std::string content_;
    std::int64_t line_ = 0;
    std::int64_t column_ = 0;
    bool to_file_ = false;
    std::string notice_;
    std::string ticket_;
    std::int64_t list_request_ = 0;
    std::int64_t restore_request_ = 0;
    std::int64_t poll_request_ = 0;
    std::uint64_t epoch_ = 0;

    std::int64_t send(const std::string &method, const Json &params);
    void answered(const Json &result, bool *taken);
    void notify();
};

} // namespace PelicanStudio
