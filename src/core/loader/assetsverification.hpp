#pragma once

#include "../container.hpp"
#include "../../project/assetsmanifest.hpp"
#include "pathresolver.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Pelican {

// AV-1: one store's verification as it was run: the manifest the store
// declares (empty when it declares one that cannot be resolved; the issue
// list then holds a single invalid_manifest), what matched, every issue, and
// how many files the content check hashed and how many the hash cache
// answered.
struct AssetStoreVerification {
    std::string name;
    std::filesystem::path root;
    std::optional<std::filesystem::path> manifest;
    int matched = 0;
    int hashed = 0;
    int cached = 0;
    std::vector<AssetManifestIssue> issues;
};

struct StartupAssetsVerificationSummary {
    int stores = 0;
    int matched = 0;
    int info = 0;
    int warnings = 0;
    int errors = 0;
    // AV-1: the stores counted above, in the order verified.
    std::vector<AssetStoreVerification> results;

    bool shouldContinueLoading() const { return errors == 0; }
};

StartupAssetsVerificationSummary verifyAssetsAtStartup(
    const std::vector<AssetStoreStatus> &stores,
    const std::filesystem::path &cache_path,
    bool strict_assets);

std::string_view assetManifestIssueKindName(AssetManifestIssueKind kind);

// AV-1: the update_assets_manifest refusals, by name.
inline constexpr std::string_view assetsStoreUnknownErrorCode = "assets_store_unknown@1";
inline constexpr std::string_view assetsManifestUndeclaredErrorCode = "assets_manifest_undeclared@1";
inline constexpr std::string_view assetsManifestWriteFailedErrorCode = "assets_manifest_write_failed@1";
// EB-1: a verification run (verify_assets, edit_project_settings) is reading
// the stores; update_assets_manifest {store} does not wait for it.
inline constexpr std::string_view assetsVerificationRunningErrorCode = "assets_verification_running@1";
// A host that runs no startup verification (no AssetsVerification module).
inline constexpr std::string_view assetsVerificationUnavailableErrorCode =
    "assets_verification_unavailable@1";

class AssetsVerificationError : public std::runtime_error {
    std::string code_;
    nlohmann::json fields_;

  public:
    AssetsVerificationError(std::string_view code, const std::string &message, nlohmann::json fields)
        // fields_(...), not fields_{...}: a braced json of one json is an array.
        : std::runtime_error{message}, code_{code}, fields_(std::move(fields)) {}
    const std::string &code() const { return code_; }
    const nlohmann::json &fields() const { return fields_; }
};

// AV-1: the startup verification's result kept for RPC, and its reruns.
// The player verifies at startup (verifyAtStartup) and keeps every store's
// result; get_status reads a summary, get_assets_verification all of it.
// verify() reruns the same check on a worker thread of its own (it reads
// only the stores, their manifests and the hash cache, never the frame);
// a call while one runs joins it. updateManifest() writes one store's
// manifest from the disk as `pelican_cli assets manifest` does, then reruns.
DECLARE_MODULE(AssetsVerification) {
    struct Record {
        std::string verified_at;
        std::vector<AssetStoreVerification> stores;
    };

    mutable std::mutex mutex;
    std::condition_variable changed;
    // Held while the stores, manifests and hash cache are read or written:
    // by a run, and by a manifest update.
    std::mutex disk;
    std::thread worker;
    std::atomic_bool stop_requested{false};
    bool held_for_testing = false;

    bool configured = false;
    std::vector<AssetStoreStatus> stores;
    std::filesystem::path cache_path;
    bool strict = false;
    std::optional<std::string> skipped_reason;
    std::optional<Record> record;
    bool running = false;
    // PJ-1: the stores changed while a run read the previous ones; the
    // worker runs once more before it reports.
    bool stale = false;

    void run();
    void startLocked();

  public:
    AssetsVerification() = default;
    ~AssetsVerification();
    AssetsVerification(const AssetsVerification &) = delete;
    AssetsVerification &operator=(const AssetsVerification &) = delete;

    // The startup check (verifyAssetsAtStartup, its log lines and its
    // strict refusal unchanged); the stores and the result are kept. A
    // PELICAN_WITH_HOT_RELOAD=OFF build skips it unless strict.
    StartupAssetsVerificationSummary verifyAtStartup(const std::vector<AssetStoreStatus> &stores,
                                                     const std::filesystem::path &cache_path,
                                                     bool strict_assets);

    // get_status.assets_verification.
    nlohmann::json statusJson() const;
    // get_assets_verification.
    nlohmann::json detailJson() const;
    // verify_assets: starts a rerun (or joins the one running).
    nlohmann::json verify();
    // update_assets_manifest {store}. Throws AssetsVerificationError; EB-1:
    // assets_verification_running@1 {store} while a run is in progress
    // (refused at once rather than waiting for the run to finish).
    nlohmann::json updateManifest(const std::string &store);
    // PJ-1: project.json's asset stores changed in place
    // (edit_project_settings). Writes the manifest of each store named in
    // `generate` whose manifest file does not exist yet, from the disk as
    // update_assets_manifest does, then verifies the new stores: starts a
    // rerun, or has the one running read them once it is done. Returns
    // {state, generated:[{store, manifest, files, hashed, cached}],
    //  failed:[{store, manifest, reason}]}. Throws AssetsVerificationError
    // (unavailable) in a host that never ran the startup check.
    nlohmann::json redeclareStores(std::vector<AssetStoreStatus> stores,
                                   const std::vector<std::string> &generate);

    // Joins the worker; a run in progress finishes first.
    void stop();
    // Tests: the worker waits, holding the stores as a run that reads them
    // does, before it starts reading while held; wait returns once no run is
    // in progress.
    void holdForTesting(bool held);
    void waitForTesting();
};

} // namespace Pelican
