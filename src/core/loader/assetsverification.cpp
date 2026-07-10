#include "assetsverification.hpp"

#include "../../project/assetsmanifest.hpp"
#include "../log.hpp"

#include <sstream>
#include <string>

namespace Pelican {

StartupAssetsVerificationSummary verifyAssetsAtStartup(
    const std::vector<AssetStoreStatus> &stores,
    const std::filesystem::path &cache_path,
    bool strict_assets) {
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

        if (store.manifest_error) {
            const auto detail = "assets manifest [" + store.name + "] " + *store.manifest_error;
            if (strict_assets) {
                ++summary.errors;
                LOG_ERROR(logger, "{}", detail);
            } else {
                ++summary.warnings;
                LOG_WARNING(logger, "{}", detail);
            }
            continue;
        }

        const auto verified = verifyAssetsManifest(
            store.root, *store.manifest, cache_path, store.name,
            {.include_content = true, .force_rehash = false, .strict = strict_assets});
        summary.matched += verified.matched;
        summary.info += countAssetManifestIssues(verified, AssetManifestIssueSeverity::info);
        summary.warnings += countAssetManifestIssues(verified, AssetManifestIssueSeverity::warning);
        summary.errors += countAssetManifestIssues(verified, AssetManifestIssueSeverity::error);

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

    if (summary.stores == 0) {
        return summary;
    }

    const auto message = "assets manifest: {} stores, {} matched, {} info, {} warnings, {} errors; stores={}";
    if (summary.errors > 0) {
        LOG_ERROR(logger, message, summary.stores, summary.matched, summary.info, summary.warnings,
                  summary.errors, resolved.str());
    } else {
        LOG_INFO(logger, message, summary.stores, summary.matched, summary.info, summary.warnings,
                 summary.errors, resolved.str());
    }
    return summary;
}

} // namespace Pelican
