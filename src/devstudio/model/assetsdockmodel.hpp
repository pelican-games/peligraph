#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {

// HW-2s / AV-1s. The Assets dock: the project's asset folders as a tree, a
// "Reload when saved outside" switch on every row, and what the engine's
// check of the asset contents against a manifest found.
//
//   * The switches are project.json's `hot_reload` (engine HW-1), written by
//     edit_project_settings (engine PJ-1) and applied in place. What is shown
//     is get_status.reload.watch; a row inside a watched folder is watched
//     through it (component-wise prefix), which the row says in grey.
//   * "Track asset contents" declares a manifest in one edit -- the store
//     `main` at assets/ when there is none, else a manifest for each store
//     that has none -- and the engine writes the manifest and checks against
//     it (engine AV-1). Nothing is declared until someone presses it.
//   * Each edit is one entry of the dock's Undo, which sends the reply's
//     `previous` values of the keys it changed. Undo restores values, not the
//     file's digest: project.json, once written, is spelled with a 2-space
//     indent.
//
// Qt-free: the transport sends a request and answers its id, and every reply
// comes back through observeStatus / receiveResult / receiveRefusal /
// receiveFailure.

// A project path as hot_reload holds it: '/' separators, no "./", no trailing
// '/' ("assets/textures/" -> "assets/textures").
[[nodiscard]] std::string normalizedProjectPath(std::string_view path);

// Whether `logical` is watched: listed itself, or inside a listed folder
// (`through` names it), or not at all.
enum class AssetWatchMark : std::uint8_t { None, Direct, Inherited };
struct AssetWatchState {
    AssetWatchMark mark = AssetWatchMark::None;
    std::string through;
};
[[nodiscard]] AssetWatchState assetWatchState(std::string_view logical,
                                              const std::vector<std::string> &watch);
// The watch with `logical` switched on (what it covers is no longer listed on
// its own) or off.
[[nodiscard]] std::vector<std::string> watchWithPath(const std::vector<std::string> &watch,
                                                     std::string_view logical, bool on);

// One top folder of the tree: `logical` as the project names it ("assets"),
// `physical` where it is on disk.
struct AssetRoot {
    std::string logical;
    std::filesystem::path physical;
};
// assets/, and each asset store's mount that is not inside it (on disk where
// get_status.stores says, else under the project root).
[[nodiscard]] std::vector<AssetRoot> assetRoots(const std::filesystem::path &project_root,
                                                const nlohmann::json &asset_stores,
                                                const nlohmann::json &status_stores);

struct AssetEntry {
    std::string logical;
    std::string name;
    std::filesystem::path physical;
    bool directory = false;
    std::int64_t modified = 0;
    std::uintmax_t size = 0;
    std::vector<AssetEntry> children;
};
// The roots that exist, folders first, then by name. .gitkeep is not shown.
[[nodiscard]] std::vector<AssetEntry> scanAssetRoots(const std::vector<AssetRoot> &roots);
// The files of `after` that are new or written since `before`, the newest
// last.
[[nodiscard]] std::vector<std::string> changedAssetFiles(const std::vector<AssetEntry> &before,
                                                         const std::vector<AssetEntry> &after);
// A file's identity for matching what the engine names with what the tree
// lists: lexically normal, '/' separators, lower case on Windows.
[[nodiscard]] std::string assetPathKey(const std::filesystem::path &path);

// --- AV-1s: the check of the contents against the manifest.

// get_status.assets_verification.
struct AssetsVerificationSummary {
    enum class State : std::uint8_t { Unknown, NotRun, Running, Verified };
    State state = State::Unknown;
    std::string reason;
    std::uint64_t stores = 0;
    std::uint64_t matched = 0;
    std::uint64_t info = 0;
    std::uint64_t warnings = 0;
    std::uint64_t errors = 0;
    [[nodiscard]] std::uint64_t issues() const noexcept { return info + warnings + errors; }
    bool operator==(const AssetsVerificationSummary &) const = default;
};
[[nodiscard]] AssetsVerificationSummary parseAssetsVerificationSummary(const nlohmann::json &status);

struct AssetIssue {
    std::string kind;
    std::string severity;
    std::string file;
    std::string message;
    // store root / file, where the tree finds its row.
    std::filesystem::path physical;
};
struct AssetStoreVerification {
    std::string name;
    std::filesystem::path root;
    // Empty when the store's manifest could not be resolved.
    std::filesystem::path manifest;
    std::uint64_t matched = 0;
    std::vector<AssetIssue> issues;
};
// get_assets_verification's stores.
[[nodiscard]] std::vector<AssetStoreVerification>
parseAssetsVerificationDetail(const nlohmann::json &detail);
// "content differs", "missing", "extra", "case differs", "unreadable",
// "manifest unreadable"; the kind itself for one Studio does not know.
[[nodiscard]] std::string assetIssueKindLabel(std::string_view kind);
// The line under the tree: empty when nothing was checked (not_run),
// "Checking asset contents…", "Asset contents match the manifest (N files).",
// or "N files differ from the manifest: 1 extra, 2 content differs."
[[nodiscard]] std::string assetsVerificationLine(const AssetsVerificationSummary &summary,
                                                 const std::vector<AssetStoreVerification> &detail);
// Asked before Update manifest: "N files will change in <manifest>."
[[nodiscard]] std::string manifestUpdateLine(const AssetStoreVerification &store);

// The line above the tree: "Watching 2 folders · last reload: textures/rock.ktx2";
// empty when nothing is watched. `is_file` tells a listed file from a folder.
[[nodiscard]] std::string assetsWatchLine(const std::vector<std::string> &watch,
                                          const std::function<bool(const std::string &)> &is_file,
                                          const std::string &last_reload);

// Whether any store declares a manifest.
[[nodiscard]] bool assetStoresDeclareManifest(const nlohmann::json &asset_stores);
// asset_stores with a manifest for every store that has none: `main` at
// assets/ with assets.manifest.json when there is no store at all.
[[nodiscard]] nlohmann::json assetStoresWithManifest(const nlohmann::json &asset_stores);

class AssetsDockSession {
  public:
    using Json = nlohmann::json;

    // While a check runs, get_status is read this often, at most this many
    // times (a check reads every file once; a large project takes a while).
    static constexpr int VerificationPollMs = 300;
    static constexpr int VerificationPollLimit = 600;
    // After a file under the watch was written, get_status is read until its
    // hashed_files moves: the engine's watcher waits out a debounce first.
    static constexpr int ReloadFollowMs = 400;
    static constexpr int ReloadFollowReads = 20;

    struct Transport {
        // Sends one request; the id its reply comes back under, 0 when it
        // could not be sent (`error` says why).
        std::function<std::int64_t(const std::string &method, const Json &params,
                                   std::string *error)>
            request;
        // Runs `task` after `milliseconds`.
        std::function<void(int milliseconds, std::function<void()> task)> schedule;
    };

    // One entry of the dock's Undo: the keys an edit changed, and their
    // values before it (the reply's `previous`).
    struct UndoEntry {
        std::string description;
        Json previous = Json::object();
    };

    explicit AssetsDockSession(Transport transport);

    // Anything shown may read differently.
    std::function<void()> changed;

    // Another project (an empty root: none). Nothing known, no Undo.
    void open(std::filesystem::path project_root);
    void engineReady();
    void engineGone();

    // The edits. False when one cannot start now (no engine, another edit in
    // flight); `notice` says why when there is something to say.
    bool setWatched(const std::string &logical, bool on);
    bool trackContents();
    bool undo();
    bool verifyAgain();
    bool updateManifest(const std::string &store);

    // Files the tree saw written (logical paths, newest last).
    void filesChanged(const std::vector<std::string> &logical);
    // A line of the dock's own (a switch that cannot be turned that way).
    void say(std::string line);

    [[nodiscard]] bool owns(std::int64_t id) const noexcept;
    // Every get_status result, whoever asked.
    void observeStatus(std::int64_t id, const Json &status);
    bool receiveResult(std::int64_t id, const Json &result);
    // A JSON-RPC error: data.error_code and data.error_fields.
    bool receiveRefusal(std::int64_t id, const std::string &error_code, const std::string &message,
                        const Json &error_fields);
    bool receiveFailure(std::int64_t id, const std::string &message);

    [[nodiscard]] const std::filesystem::path &projectRoot() const noexcept { return root_; }
    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] bool settingsRead() const noexcept { return settings_read_; }
    [[nodiscard]] bool editPending() const noexcept { return edit_.has_value(); }
    // project.json's values, as get_project_settings or the last edit said.
    [[nodiscard]] const Json &hotReload() const noexcept { return hot_reload_; }
    [[nodiscard]] const Json &assetStores() const noexcept { return asset_stores_; }
    // get_status.reload.watch (empty when null) and get_status.stores.
    [[nodiscard]] const std::vector<std::string> &watch() const noexcept { return watch_; }
    [[nodiscard]] const Json &statusStores() const noexcept { return status_stores_; }
    // The watch as it will be once the edit in flight lands.
    [[nodiscard]] std::vector<std::string> shownWatch() const;
    [[nodiscard]] const AssetsVerificationSummary &verification() const noexcept {
        return summary_;
    }
    [[nodiscard]] const std::vector<AssetStoreVerification> &verificationDetail() const noexcept {
        return detail_;
    }
    // A check, an update or a Track is under way: Verify again and Update
    // manifest wait for it.
    [[nodiscard]] bool verificationRunning() const noexcept;
    [[nodiscard]] const std::string &lastReload() const noexcept { return last_reload_; }
    [[nodiscard]] const std::string &notice() const noexcept { return notice_; }
    [[nodiscard]] const std::string &noticeDetail() const noexcept { return notice_detail_; }
    // restart_required's reason, when an edit needs the engine started again.
    [[nodiscard]] const std::string &restartReason() const noexcept { return restart_reason_; }
    [[nodiscard]] const std::vector<UndoEntry> &undoEntries() const noexcept { return undo_; }
    // Track asset contents is offered: the settings were read and no store
    // declares a manifest.
    [[nodiscard]] bool trackOffered() const noexcept;
    // The watch was switched on in this run of the engine: what was saved
    // before it is not read again.
    [[nodiscard]] bool watchSwitchedOn() const noexcept { return watch_switched_on_; }

  private:
    struct PendingEdit {
        enum class Kind : std::uint8_t { Watch, Track, Undo };
        Kind kind = Kind::Watch;
        std::string logical;
        bool on = false;
    };

    void notify();
    void setNotice(std::string line, std::string detail = {});
    std::int64_t send(const std::string &method, const Json &params);
    bool startEdit(PendingEdit edit);
    void readSettings();
    void sendEdit();
    void readStatus();
    void scheduleStatusRead(int milliseconds);
    void readDetail();
    void readSettingsResult(const Json &result);
    void editResult(const Json &result);
    void startVerificationFollow();
    void forgetRequests();

    Transport transport_;
    std::filesystem::path root_;
    std::uint64_t generation_ = 0;
    bool ready_ = false;
    bool settings_read_ = false;
    std::string digest_;
    Json hot_reload_;
    Json asset_stores_;
    std::vector<std::string> watch_;
    Json status_stores_ = Json::object();
    std::optional<std::uint64_t> hashed_files_;
    AssetsVerificationSummary summary_;
    std::vector<AssetStoreVerification> detail_;
    bool detail_wanted_ = false;
    int verification_reads_left_ = 0;
    bool verification_started_ = false;
    std::vector<std::string> reload_candidates_;
    int reload_reads_left_ = 0;
    std::string last_reload_;
    std::string notice_;
    std::string notice_detail_;
    std::string restart_reason_;
    std::vector<UndoEntry> undo_;
    std::optional<PendingEdit> edit_;
    bool watch_switched_on_ = false;
    // The requests in flight.
    std::int64_t settings_request_ = 0;
    std::int64_t edit_request_ = 0;
    std::int64_t status_request_ = 0;
    bool status_again_ = false;
    bool status_scheduled_ = false;
    std::int64_t detail_request_ = 0;
    std::int64_t verify_request_ = 0;
    std::int64_t update_request_ = 0;
};

} // namespace PelicanStudio
