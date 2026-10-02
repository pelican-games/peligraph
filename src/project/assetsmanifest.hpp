#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

enum class AssetManifestIssueSeverity {
    info,
    warning,
    error,
};

enum class AssetManifestIssueKind {
    content_mismatch,
    missing,
    extra,
    case_mismatch,
    unreadable,
    invalid_manifest,
};

struct AssetManifestEntry {
    std::string file;
    std::uintmax_t size = 0;
    std::string sha256;
};

struct AssetsManifest {
    std::vector<AssetManifestEntry> files;
};

struct AssetManifestGenerationResult {
    int files = 0;
    int hashed = 0;
    int cache_hits = 0;
};

struct AssetManifestIssue {
    AssetManifestIssueSeverity severity = AssetManifestIssueSeverity::warning;
    AssetManifestIssueKind kind = AssetManifestIssueKind::unreadable;
    std::string file;
    std::string message;
};

struct AssetManifestVerificationResult {
    int matched = 0;
    std::vector<AssetManifestIssue> issues;
};

// The content check's work, known before any file is hashed.
struct AssetManifestHashPlan {
    int to_hash = 0;
    std::uintmax_t bytes_to_hash = 0;
    int cached = 0;
};

struct AssetManifestVerifyOptions {
    bool include_content = false;
    bool force_rehash = false;
    bool strict = false;
    // Called once before hashing when include_content is set.
    std::function<void(const AssetManifestHashPlan &)> before_hashing;
};

// Lowercase hex SHA-256 of a file's bytes, read in 1 MiB chunks.
std::string assetFileSha256(const std::filesystem::path &path);

AssetsManifest parseAssetsManifestJson(std::string_view json_text);
std::string serializeAssetsManifest(const AssetsManifest &manifest);

AssetManifestGenerationResult generateAssetsManifest(const std::filesystem::path &store_root,
                                                      const std::filesystem::path &manifest_path,
                                                      const std::filesystem::path &cache_path,
                                                      std::string_view cache_namespace,
                                                      bool full);

AssetManifestVerificationResult verifyAssetsManifest(const std::filesystem::path &store_root,
                                                      const std::filesystem::path &manifest_path,
                                                      const std::filesystem::path &cache_path,
                                                      std::string_view cache_namespace,
                                                      const AssetManifestVerifyOptions &options = {});

int countAssetManifestIssues(const AssetManifestVerificationResult &result,
                             AssetManifestIssueSeverity severity);
std::string_view assetManifestIssueSeverityName(AssetManifestIssueSeverity severity);

} // namespace Pelican
