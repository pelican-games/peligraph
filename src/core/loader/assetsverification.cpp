#include "assetsverification.hpp"

#include "../../project/assetsmanifest.hpp"
#include "../log.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <sstream>
#include <string>

namespace Pelican {

namespace {

// AV-1: get_status / get_assets_verification's reasons for not_run.
constexpr std::string_view noManifestStoreReason = "no asset store declares a manifest";

StartupAssetsVerificationSummary verifyAssetStores(
    const std::vector<AssetStoreStatus> &stores,
    const std::filesystem::path &cache_path,
    bool strict_assets,
    bool log) {
    StartupAssetsVerificationSummary summary;
    std::ostringstream resolved;
    for (const auto &store : stores) {
        if (!store.manifest && !store.manifest_error) {
            continue;
        }
        if (summary.stores > 0) {
            resolved << ", ";
        }
        resolved << store.name << "=" << store.root.string();
        ++summary.stores;
        auto &kept = summary.results.emplace_back(
            AssetStoreVerification{.name = store.name, .root = store.root, .manifest = store.manifest});

        if (store.manifest_error) {
            const auto detail = "assets manifest [" + store.name + "] " + *store.manifest_error;
            const auto severity =
                strict_assets ? AssetManifestIssueSeverity::error : AssetManifestIssueSeverity::warning;
            kept.issues.push_back({severity, AssetManifestIssueKind::invalid_manifest, {},
                                   *store.manifest_error});
            if (strict_assets) {
                ++summary.errors;
                if (log) LOG_ERROR(logger, "{}", detail);
            } else {
                ++summary.warnings;
                if (log) LOG_WARNING(logger, "{}", detail);
            }
            continue;
        }

        const auto log_hash_plan = [&store, &kept, log](const AssetManifestHashPlan &plan) {
            kept.hashed = plan.to_hash;
            kept.cached = plan.cached;
            if (!log || plan.to_hash == 0) {
                return;
            }
            LOG_INFO(logger, "assets manifest [{}] verifying {} assets ({:.1f} MB), {} cached",
                     store.name, plan.to_hash,
                     static_cast<double>(plan.bytes_to_hash) / (1024.0 * 1024.0), plan.cached);
        };
        auto verified = verifyAssetsManifest(
            store.root, *store.manifest, cache_path, store.name,
            {.include_content = true,
             .force_rehash = false,
             .strict = strict_assets,
             .before_hashing = log_hash_plan});
        summary.matched += verified.matched;
        summary.info += countAssetManifestIssues(verified, AssetManifestIssueSeverity::info);
        summary.warnings += countAssetManifestIssues(verified, AssetManifestIssueSeverity::warning);
        summary.errors += countAssetManifestIssues(verified, AssetManifestIssueSeverity::error);

        if (log) {
            for (const auto &issue : verified.issues) {
                const auto detail = "assets manifest [" + store.name + "] " + issue.message;
                switch (issue.severity) {
                case AssetManifestIssueSeverity::info:
                    LOG_INFO(logger, "{}", detail);
                    break;
                case AssetManifestIssueSeverity::warning:
                    LOG_WARNING(logger, "{}", detail);
                    break;
                case AssetManifestIssueSeverity::error:
                    LOG_ERROR(logger, "{}", detail);
                    break;
                }
            }
        }
        kept.matched = verified.matched;
        kept.issues = std::move(verified.issues);
    }

    if (summary.stores == 0 || !log) {
        return summary;
    }

    // quill のログマクロは format 文字列の compile-time 評価を要求する
    // (VS2026 の MSVC は const auto では C2131)。
    static constexpr auto message =
        "assets manifest: {} stores, {} matched, {} info, {} warnings, {} errors; stores={}";
    if (summary.errors > 0) {
        LOG_ERROR(logger, message, summary.stores, summary.matched, summary.info, summary.warnings,
                  summary.errors, resolved.str());
    } else {
        LOG_INFO(logger, message, summary.stores, summary.matched, summary.info, summary.warnings,
                 summary.errors, resolved.str());
    }
    return summary;
}

std::string utcNow() {
    return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

std::string_view issueSeverityName(AssetManifestIssueSeverity severity) {
    switch (severity) {
    case AssetManifestIssueSeverity::info:
        return "info";
    case AssetManifestIssueSeverity::warning:
        return "warning";
    case AssetManifestIssueSeverity::error:
        return "error";
    }
    return "unknown";
}

nlohmann::json storeJson(const AssetStoreVerification &store) {
    auto issues = nlohmann::json::array();
    for (const auto &issue : store.issues) {
        issues.push_back({{"kind", assetManifestIssueKindName(issue.kind)},
                          {"severity", issueSeverityName(issue.severity)},
                          {"file", issue.file},
                          {"message", issue.message}});
    }
    return {{"name", store.name},
            {"root", store.root.generic_string()},
            {"manifest", store.manifest ? nlohmann::json(store.manifest->generic_string()) : nlohmann::json(nullptr)},
            {"matched", store.matched},
            {"hashed", store.hashed},
            {"cached", store.cached},
            {"issues", std::move(issues)}};
}

} // namespace

StartupAssetsVerificationSummary verifyAssetsAtStartup(
    const std::vector<AssetStoreStatus> &stores,
    const std::filesystem::path &cache_path,
    bool strict_assets) {
    return verifyAssetStores(stores, cache_path, strict_assets, true);
}

std::string_view assetManifestIssueKindName(AssetManifestIssueKind kind) {
    switch (kind) {
    case AssetManifestIssueKind::content_mismatch:
        return "content_mismatch";
    case AssetManifestIssueKind::missing:
        return "missing";
    case AssetManifestIssueKind::extra:
        return "extra";
    case AssetManifestIssueKind::case_mismatch:
        return "case_mismatch";
    case AssetManifestIssueKind::unreadable:
        return "unreadable";
    case AssetManifestIssueKind::invalid_manifest:
        return "invalid_manifest";
    }
    return "unknown";
}

AssetsVerification::~AssetsVerification() { stop(); }

StartupAssetsVerificationSummary AssetsVerification::verifyAtStartup(
    const std::vector<AssetStoreStatus> &store_list,
    const std::filesystem::path &cache,
    bool strict_assets) {
    {
        std::scoped_lock lock{mutex};
        configured = true;
        stores = store_list;
        cache_path = cache;
        strict = strict_assets;
        skipped_reason.reset();
        record.reset();
    }
#if !PELICAN_WITH_HOT_RELOAD
    // A distribution build checks its assets when they are baked; at startup
    // only when --strict-assets asks.
    if (!strict_assets) {
        std::scoped_lock lock{mutex};
        skipped_reason = "PELICAN_WITH_HOT_RELOAD=OFF";
        return {};
    }
#endif
    std::scoped_lock disk_lock{disk};
    auto summary = verifyAssetsAtStartup(store_list, cache, strict_assets);
    std::scoped_lock lock{mutex};
    record = Record{.verified_at = utcNow(), .stores = summary.results};
    return summary;
}

nlohmann::json AssetsVerification::statusJson() const {
    std::scoped_lock lock{mutex};
    if (running) return {{"state", "running"}};
    if (!configured) return {{"state", "not_run"}, {"reason", "this host does not verify assets"}};
    if (skipped_reason) return {{"state", "not_run"}, {"reason", *skipped_reason}};
    if (!record || record->stores.empty()) return {{"state", "not_run"}, {"reason", noManifestStoreReason}};
    int matched = 0;
    int info = 0;
    int warnings = 0;
    int errors = 0;
    for (const auto &store : record->stores) {
        matched += store.matched;
        for (const auto &issue : store.issues) {
            switch (issue.severity) {
            case AssetManifestIssueSeverity::info:
                ++info;
                break;
            case AssetManifestIssueSeverity::warning:
                ++warnings;
                break;
            case AssetManifestIssueSeverity::error:
                ++errors;
                break;
            }
        }
    }
    return {{"state", "verified"},
            {"stores", record->stores.size()},
            {"matched", matched},
            {"info", info},
            {"warnings", warnings},
            {"errors", errors}};
}

nlohmann::json AssetsVerification::detailJson() const {
    auto answer = statusJson();
    std::scoped_lock lock{mutex};
    auto list = nlohmann::json::array();
    // While a rerun runs, the previous result is still what there is.
    if (record) {
        for (const auto &store : record->stores) list.push_back(storeJson(store));
    }
    nlohmann::json detail{{"state", answer.at("state")}};
    if (answer.contains("reason")) detail["reason"] = answer.at("reason");
    detail["verified_at"] = record ? nlohmann::json(record->verified_at) : nlohmann::json(nullptr);
    detail["stores"] = std::move(list);
    return detail;
}

void AssetsVerification::run() {
    {
        std::unique_lock lock{mutex};
        changed.wait(lock, [&] { return stop_requested.load() || !held_for_testing; });
        if (stop_requested.load()) {
            running = false;
            changed.notify_all();
            return;
        }
    }
    std::scoped_lock disk_lock{disk};
    for (;;) {
        std::vector<AssetStoreStatus> store_list;
        std::filesystem::path cache;
        bool strict_assets = false;
        {
            std::scoped_lock lock{mutex};
            store_list = stores;
            cache = cache_path;
            strict_assets = strict;
            stale = false;
        }
        std::optional<Record> done;
        try {
            auto summary = verifyAssetStores(store_list, cache, strict_assets, false);
            done = Record{.verified_at = utcNow(), .stores = std::move(summary.results)};
        } catch (const std::exception &failure) {
            LOG_WARNING(logger, "assets manifest: verification failed: {}", failure.what());
        }
        std::scoped_lock lock{mutex};
        // PJ-1: the stores changed while this read the previous ones.
        if (stale && !stop_requested.load()) continue;
        if (done) {
            record = std::move(done);
            skipped_reason.reset();
        }
        running = false;
        break;
    }
    changed.notify_all();
}

void AssetsVerification::startLocked() {
    if (running) return;
    if (worker.joinable()) worker.join();
    running = true;
    worker = std::thread{[this] { run(); }};
}

nlohmann::json AssetsVerification::verify() {
    std::scoped_lock lock{mutex};
    if (!configured) {
        throw AssetsVerificationError{assetsVerificationUnavailableErrorCode,
                                      "verify_assets: this host does not verify assets",
                                      nlohmann::json::object()};
    }
    const bool any = std::any_of(stores.begin(), stores.end(), [](const AssetStoreStatus &store) {
        return store.manifest || store.manifest_error;
    });
    if (!any) return {{"state", "not_run"}, {"reason", noManifestStoreReason}};
    startLocked();
    return {{"state", "running"}};
}

nlohmann::json AssetsVerification::updateManifest(const std::string &name) {
    std::optional<AssetStoreStatus> store;
    std::filesystem::path cache;
    {
        std::scoped_lock lock{mutex};
        if (!configured) {
            throw AssetsVerificationError{assetsVerificationUnavailableErrorCode,
                                          "update_assets_manifest: this host does not verify assets",
                                          nlohmann::json::object()};
        }
        const auto found = std::find_if(stores.begin(), stores.end(),
                                        [&](const AssetStoreStatus &candidate) { return candidate.name == name; });
        if (found != stores.end()) store = *found;
        cache = cache_path;
    }
    if (!store) {
        throw AssetsVerificationError{assetsStoreUnknownErrorCode,
                                      "update_assets_manifest: no asset store is named '" + name + "'",
                                      nlohmann::json::object({{"store", name}})};
    }
    if (store->manifest_error) {
        throw AssetsVerificationError{assetsManifestWriteFailedErrorCode,
                                      "update_assets_manifest: cannot write the manifest of asset store '" +
                                          name + "': " + *store->manifest_error,
                                      nlohmann::json::object({{"store", name}, {"reason", *store->manifest_error}})};
    }
    if (!store->manifest) {
        throw AssetsVerificationError{assetsManifestUndeclaredErrorCode,
                                      "update_assets_manifest: asset store '" + name +
                                          "' declares no manifest in project.json",
                                      nlohmann::json::object({{"store", name}})};
    }
    AssetManifestGenerationResult generated;
    {
        // A run in progress reads the hash cache this writes: it finishes
        // first.
        std::scoped_lock disk_lock{disk};
        try {
            generated = generateAssetsManifest(store->root, *store->manifest, cache, store->name, false);
        } catch (const std::exception &failure) {
            throw AssetsVerificationError{assetsManifestWriteFailedErrorCode,
                                          "update_assets_manifest: cannot write the manifest of asset store '" +
                                              name + "': " + failure.what(),
                                          {{"store", name},
                                           {"manifest", store->manifest->generic_string()},
                                           {"reason", failure.what()}}};
        }
    }
    LOG_INFO(logger, "manifest {}: {} files ({} hashed, {} cached) -> {}", store->name, generated.files,
             generated.hashed, generated.cache_hits, store->manifest->string());
    nlohmann::json answer{{"store", name},
                          {"manifest", store->manifest->generic_string()},
                          {"files", generated.files},
                          {"hashed", generated.hashed},
                          {"cached", generated.cache_hits}};
    answer["state"] = verify().at("state");
    return answer;
}

nlohmann::json AssetsVerification::redeclareStores(std::vector<AssetStoreStatus> store_list,
                                                   const std::vector<std::string> &generate) {
    std::filesystem::path cache;
    {
        std::scoped_lock lock{mutex};
        if (!configured) {
            throw AssetsVerificationError{assetsVerificationUnavailableErrorCode,
                                          "edit_project_settings: this host does not verify assets",
                                          nlohmann::json::object()};
        }
        cache = cache_path;
    }
    auto generated = nlohmann::json::array();
    auto failed = nlohmann::json::array();
    std::vector<const AssetStoreStatus *> to_write;
    for (const auto &name : generate) {
        const auto found = std::find_if(store_list.begin(), store_list.end(),
                                        [&](const AssetStoreStatus &store) { return store.name == name; });
        if (found == store_list.end() || !found->manifest) continue;
        std::error_code ec;
        if (std::filesystem::exists(*found->manifest, ec) || ec) continue;
        to_write.push_back(&*found);
    }
    if (!to_write.empty()) {
        // A run in progress reads the hash cache this writes: it finishes
        // first.
        std::scoped_lock disk_lock{disk};
        for (const auto *store : to_write) {
            try {
                const auto written =
                    generateAssetsManifest(store->root, *store->manifest, cache, store->name, false);
                LOG_INFO(logger, "manifest {}: {} files ({} hashed, {} cached) -> {}", store->name, written.files,
                         written.hashed, written.cache_hits, store->manifest->string());
                generated.push_back({{"store", store->name},
                                     {"manifest", store->manifest->generic_string()},
                                     {"files", written.files},
                                     {"hashed", written.hashed},
                                     {"cached", written.cache_hits}});
            } catch (const std::exception &failure) {
                LOG_WARNING(logger, "manifest {}: cannot write {}: {}", store->name, store->manifest->string(),
                            failure.what());
                failed.push_back({{"store", store->name},
                                  {"manifest", store->manifest->generic_string()},
                                  {"reason", failure.what()}});
            }
        }
    }
    {
        std::scoped_lock lock{mutex};
        stores = std::move(store_list);
        const bool any = std::any_of(stores.begin(), stores.end(), [](const AssetStoreStatus &store) {
            return store.manifest || store.manifest_error;
        });
        // No store declares a manifest any more: there is nothing the last
        // result stands for.
        if (!any) record.reset();
        if (running) stale = true;
    }
    auto answer = verify();
    answer["generated"] = std::move(generated);
    answer["failed"] = std::move(failed);
    return answer;
}

void AssetsVerification::stop() {
    std::thread running_worker;
    {
        std::scoped_lock lock{mutex};
        stop_requested = true;
        running_worker = std::move(worker);
    }
    changed.notify_all();
    if (running_worker.joinable()) running_worker.join();
    std::scoped_lock lock{mutex};
    stop_requested = false;
}

void AssetsVerification::holdForTesting(bool held) {
    {
        std::scoped_lock lock{mutex};
        held_for_testing = held;
    }
    changed.notify_all();
}

void AssetsVerification::waitForTesting() {
    std::unique_lock lock{mutex};
    changed.wait(lock, [&] { return !running; });
}

} // namespace Pelican
