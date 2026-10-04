#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {

// RL-1s. Studio picks up a render document rewritten outside it -- the root
// or a fragment a person fixes by hand in an editor of their own -- through
// RL-1's reload_render_documents.
//
//   1. Watch. Once the engine answers, get_render_documents names the project
//      render documents (engine:// is not among them); their files are what
//      the window watches. A burst of writes (an editor's save) is one check,
//      ReloadDebounceMs after the last of them.
//   2. Our own writes. Every committed render edit says what it wrote
//      (RD-1's `documents`, {reference: sha256}); a reload says what it read.
//      A file whose sha256 is the digest last seen for it is Studio's own
//      write, and nothing is asked.
//   3. Ask. Anything else is a reload_render_documents. One is in flight at a
//      time; reload_busy (a render edit's ticket still pending, or a scene
//      preview open) asks again BusyRetryMs later.
//   4. The answer. Committed: "Picked up the change to <file>", and the owner
//      reads the panels again and cuts the render undo history (`reloaded`).
//      no_change: nothing. A refusal (the file does not parse, the pipeline
//      does not compose, a shader is missing): which file and why, with the
//      engine keeping its previous version until the file is fixed and saved.
//      restart_required: the owner's restart, through RSX's one reader.
//   5. While the refusal stands, an edit the engine refuses as
//      external_modification reads as renderDiskRefusalSentence (the Graph's
//      refusals read it through knownRefusalDescription), and "Reload from
//      disk" (reloadNow) asks whatever the digests say.
//
// IA-1s, decision 5: the assets the document's images start from
// (`initial.asset`, setInitialAssets) are watched beside the documents. A
// change to one asks reload_render_documents as a document's does; an
// engine that compares them too (IA-2) says `changed_assets` and the notice
// is "Picked up the change to <asset>.", one that does not answers
// no_change and the notice says "<asset> changed -- the engine reads it on its
// next start." (Information).
//
// Qt-free: the transport sends a request and answers its id, and every reply
// comes back through receiveResult / receiveFailure.
class RenderDocumentReload {
  public:
    using Json = nlohmann::json;

    // An editor's save is often several writes; they are one check.
    static constexpr int ReloadDebounceMs = 300;
    // reload_busy: try again this much later.
    static constexpr int BusyRetryMs = 500;
    // The ticket's poll.
    static constexpr int TicketPollMs = 100;

    struct Transport {
        // Sends one request; the id its reply comes back under, 0 when it
        // could not be sent (`error` says why).
        std::function<std::int64_t(const std::string &method, const Json &params,
                                   std::string *error)>
            request;
        // Runs `task` after `milliseconds`.
        std::function<void(int milliseconds, std::function<void()> task)> schedule;
        // The sha256 (hex) of the file as it is now; nullopt when it cannot be
        // read (not there, in the middle of being replaced).
        std::function<std::optional<std::string>(const std::filesystem::path &)> file_digest;
    };

    // One line for the panel's notice row; `detail` is the rest (tooltip,
    // Copy message). Kind None clears what an earlier one said.
    struct Notice {
        enum class Kind { None, Information, Success, Error };
        Kind kind = Kind::None;
        std::string line;
        std::string detail;
    };

    // A render document Studio watches: its file, and the digest last seen
    // for it (what the listing read, what a commit wrote, what a reload read).
    struct Document {
        std::filesystem::path path;
        std::string digest;
    };

    explicit RenderDocumentReload(Transport transport);

    // The file a render document reference names in `project_root`
    // (project://x and a bare relative path are the project's), or nullopt
    // for one Studio does not watch: engine://, a fragment (#), another scheme.
    [[nodiscard]] static std::optional<std::filesystem::path>
    documentPath(const std::filesystem::path &project_root, std::string_view reference);
    // The last segment of a reference, as the notices name it.
    [[nodiscard]] static std::string fileName(std::string_view reference);

    // Another project (an empty root: none). Nothing watched, nothing known.
    void open(std::filesystem::path project_root);
    // The engine answers: what it reads is listed (get_render_documents).
    void engineReady();
    // The engine went away: what was in flight went with it.
    void engineGone();

    // A watched file was written (the watcher's signal). Checked once the
    // writes stop for ReloadDebounceMs.
    void fileChanged();
    // "Reload from disk": reload_render_documents whatever the digests say.
    void reloadNow();

    // Whether a reply under `id` is one of this object's.
    [[nodiscard]] bool owns(std::int64_t id) const noexcept {
        return id != 0 && (id == list_request_ || id == reload_request_ || id == poll_request_);
    }
    // Every reply. False when the id is not one of this object's.
    bool receiveResult(std::int64_t id, const Json &result);
    bool receiveFailure(std::int64_t id, const std::string &message);
    // Any other reply: a committed render edit's `documents` is what Studio
    // wrote (decision 2). Anything else is ignored.
    void observeReply(const Json &result);

    [[nodiscard]] const std::map<std::string, Document> &documents() const noexcept {
        return documents_;
    }
    // The files to watch, one per document and one per initial asset.
    [[nodiscard]] std::vector<std::filesystem::path> watchedFiles() const;
    // IA-1s, decision 5: the initial assets to watch, as the document
    // writes them (project-relative or project://). One already watched
    // keeps the digest last seen for it; a new one starts from what is on
    // disk now. A project open keeps the list for the new root.
    void setInitialAssets(const std::vector<std::string> &references);
    [[nodiscard]] const std::map<std::string, Document> &initialAssets() const noexcept {
        return assets_;
    }
    [[nodiscard]] bool reloadInFlight() const noexcept {
        return reload_request_ != 0 || poll_request_ != 0 || !ticket_.empty() || retrying_;
    }
    // The last reload was refused and nothing on disk has been taken since.
    [[nodiscard]] bool refused() const noexcept { return refused_; }

    // The set of files to watch changed -- or is to be put back into the
    // watch (an editor that saves by replacing a file takes it out).
    std::function<void()> watchChanged;
    // What the notice row says.
    std::function<void(const Notice &)> notice;
    // A reload committed with a change: its commit result (source_digest,
    // documents, changed, restart_required).
    std::function<void(const Json &commit)> reloaded;
    // BS-1s, decision 4: a refused reload, with the engine's error as it
    // came ({code, message, ...}), after the notice said it.
    std::function<void(const Json &error)> refusedWith;

  private:
    Transport transport_;
    std::filesystem::path project_root_;
    std::map<std::string, Document> documents_;
    // IA-1s, decision 5: the initial assets, by reference as written, and
    // those that differed when the reload in flight was asked for.
    std::vector<std::string> asset_references_;
    std::map<std::string, Document> assets_;
    std::vector<std::string> differing_assets_;
    void placeAssets();
    std::int64_t list_request_ = 0;
    std::int64_t reload_request_ = 0;
    std::int64_t poll_request_ = 0;
    std::string ticket_;
    // A reload_busy retry is scheduled.
    bool retrying_ = false;
    // The reload in flight (or its retry) was asked for by hand.
    bool forced_ = false;
    // A check found a change while a reload was in flight: one more.
    bool again_ = false;
    bool refused_ = false;
    bool busy_said_ = false;
    std::uint64_t change_serial_ = 0;
    // Checks in a row that found a file it could not read.
    int unreadable_ = 0;
    // Bumped by open / engineGone: a scheduled task from before is stale.
    std::uint64_t epoch_ = 0;
    // The files that differed when the reload was asked for.
    std::vector<std::string> differing_;

    std::int64_t send(const std::string &method, const Json &params, std::string *error);
    void say(Notice::Kind kind, std::string line, std::string detail = {});
    void check(bool forced);
    void sendReload();
    void poll();
    void readListing(const Json &result);
    void committed(const Json &commit);
    void refuse(const Json &error);
    void busy(const Json &error);
    void setRefused(bool refused);
    // The documents whose file is not the digest last seen (or cannot be
    // read); counts unreadable_.
    std::vector<std::string> differingDocuments();
};

// Decision 5. The sentence an edit refused as external_modification reads as
// while the reload of the documents on disk stands refused, and whether it
// does now. One process, one engine: the flag is the window's reload's.
inline constexpr std::string_view renderDiskRefusalSentence =
    "The file on disk changed and could not be loaded \xE2\x80\x94 fix it or use "
    "Reload from disk.";
[[nodiscard]] bool renderDocumentsRefusedOnDisk() noexcept;
void setRenderDocumentsRefusedOnDisk(bool refused) noexcept;

} // namespace PelicanStudio
