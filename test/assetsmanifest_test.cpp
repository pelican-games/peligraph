#include "../src/project/assetsmanifest.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace Pelican {

namespace {

struct Sandbox {
    std::filesystem::path root;
    std::filesystem::path store;
    std::filesystem::path manifest;
    std::filesystem::path cache;

    Sandbox() {
        root = std::filesystem::temp_directory_path() /
               ("pelican_assets_manifest_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        store = root / "store";
        manifest = root / "assets.manifest.json";
        cache = root / ".pelican" / "assets-hash-cache.json";
        std::filesystem::create_directories(store);
    }

    ~Sandbox() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

void writeText(const std::filesystem::path &path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    REQUIRE(file.is_open());
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string readText(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    REQUIRE(file.is_open());
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

bool hasIssue(const AssetManifestVerificationResult &result,
              AssetManifestIssueSeverity severity,
              AssetManifestIssueKind kind,
              std::string_view file = {}) {
    for (const auto &issue : result.issues) {
        if (issue.severity == severity && issue.kind == kind &&
            (file.empty() || issue.file == file)) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("assets manifest generation is byte-idempotent and reuses unchanged hashes",
          "[assets-manifest]") {
    Sandbox sandbox;
    writeText(sandbox.store / "z.bin", "z-content");
    writeText(sandbox.store / "nested" / "a.bin", "a-content");

    const auto first = generateAssetsManifest(sandbox.store, sandbox.manifest, sandbox.cache, "main", false);
    REQUIRE(first.files == 2);
    REQUIRE(first.hashed == 2);
    REQUIRE(first.cache_hits == 0);
    const auto first_bytes = readText(sandbox.manifest);

    const auto second = generateAssetsManifest(sandbox.store, sandbox.manifest, sandbox.cache, "main", false);
    REQUIRE(second.files == 2);
    REQUIRE(second.hashed == 0);
    REQUIRE(second.cache_hits == 2);
    REQUIRE(readText(sandbox.manifest) == first_bytes);

    const auto parsed = parseAssetsManifestJson(first_bytes);
    REQUIRE(parsed.files.size() == 2);
    REQUIRE(parsed.files[0].file == "nested/a.bin");
    REQUIRE(parsed.files[1].file == "z.bin");
}

TEST_CASE("assets manifest cache rehashes only changed files and full bypasses it",
          "[assets-manifest]") {
    Sandbox sandbox;
    writeText(sandbox.store / "a.bin", "a");
    writeText(sandbox.store / "b.bin", "b");
    (void)generateAssetsManifest(sandbox.store, sandbox.manifest, sandbox.cache, "main", false);

    writeText(sandbox.store / "a.bin", "a changed and larger");
    const auto differential =
        generateAssetsManifest(sandbox.store, sandbox.manifest, sandbox.cache, "main", false);
    REQUIRE(differential.hashed == 1);
    REQUIRE(differential.cache_hits == 1);

    const auto verified = verifyAssetsManifest(
        sandbox.store, sandbox.manifest, sandbox.cache, "main",
        {.include_content = true, .force_rehash = true, .strict = false});
    REQUIRE(verified.issues.empty());

    const auto full = generateAssetsManifest(sandbox.store, sandbox.manifest, sandbox.cache, "main", true);
    REQUIRE(full.hashed == 2);
    REQUIRE(full.cache_hits == 0);
}

TEST_CASE("assets verification classifies content, structure, case, and strict errors",
          "[assets-manifest]") {
    Sandbox sandbox;
    writeText(sandbox.store / "content.bin", "original");
    writeText(sandbox.store / "missing.bin", "remove me");
    writeText(sandbox.store / "Case" / "Asset.bin", "case");
    (void)generateAssetsManifest(sandbox.store, sandbox.manifest, sandbox.cache, "main", true);

    writeText(sandbox.store / "content.bin", "changed");
    std::filesystem::remove(sandbox.store / "missing.bin");
    std::filesystem::rename(sandbox.store / "Case" / "Asset.bin", sandbox.store / "case-temp.bin");
    std::filesystem::remove_all(sandbox.store / "Case");
    std::filesystem::create_directories(sandbox.store / "case");
    std::filesystem::rename(sandbox.store / "case-temp.bin", sandbox.store / "case" / "Asset.bin");
    writeText(sandbox.store / "extra.bin", "extra");

    const auto structural = verifyAssetsManifest(
        sandbox.store, sandbox.manifest, sandbox.cache, "main",
        {.include_content = false, .force_rehash = false, .strict = false});
    REQUIRE_FALSE(hasIssue(structural, AssetManifestIssueSeverity::info,
                           AssetManifestIssueKind::content_mismatch));
    REQUIRE(hasIssue(structural, AssetManifestIssueSeverity::warning,
                     AssetManifestIssueKind::missing, "missing.bin"));
    REQUIRE(hasIssue(structural, AssetManifestIssueSeverity::warning,
                     AssetManifestIssueKind::extra, "extra.bin"));
    REQUIRE(hasIssue(structural, AssetManifestIssueSeverity::warning,
                     AssetManifestIssueKind::case_mismatch, "Case/Asset.bin"));

    const auto full = verifyAssetsManifest(
        sandbox.store, sandbox.manifest, sandbox.cache, "main",
        {.include_content = true, .force_rehash = true, .strict = false});
    REQUIRE(hasIssue(full, AssetManifestIssueSeverity::info,
                     AssetManifestIssueKind::content_mismatch, "content.bin"));
    REQUIRE(countAssetManifestIssues(full, AssetManifestIssueSeverity::error) == 0);

    const auto strict = verifyAssetsManifest(
        sandbox.store, sandbox.manifest, sandbox.cache, "main",
        {.include_content = true, .force_rehash = true, .strict = true});
    REQUIRE(strict.issues.size() == full.issues.size());
    REQUIRE(countAssetManifestIssues(strict, AssetManifestIssueSeverity::error) ==
            static_cast<int>(strict.issues.size()));
}

} // namespace Pelican
