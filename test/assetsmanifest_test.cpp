#include "../src/project/assetsmanifest.hpp"
#include "../src/project/sha256stream.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

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

TEST_CASE("asset file hashing matches SHA-256 for empty, small, and multi-chunk files",
          "[assets][manifest][AH-1]") {
    Sandbox sandbox;
    const auto empty = sandbox.store / "empty.bin";
    const auto small = sandbox.store / "abc.txt";
    const auto large = sandbox.store / "large.bin";
    writeText(empty, "");
    writeText(small, "abc");
    // Larger than the 1 MiB read chunk and not a multiple of it or of 64.
    std::string bytes(3 * 1048576 + 7, char{});
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<char>(i % 251);
    }
    writeText(large, bytes);

    // Reference digests from an independent SHA-256 (Python hashlib) over the same bytes.
    CHECK(assetFileSha256(empty) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(assetFileSha256(small) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(assetFileSha256(large) == "f578a61853ca2f4272dba551bd868420e22302fbb6d6dfc0cc80da1d2c7b779f");
    CHECK_THROWS(assetFileSha256(sandbox.store / "missing.bin"));
}

TEST_CASE("FW-1 Sha256Stream streams across the 1 MiB read unit like one-shot SHA-256",
          "[assets][manifest][fw1][sha256]") {
    Sandbox sandbox;
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // Reference digests from Python hashlib over byte i = (i * 131 + 7) & 0xff.
    const std::pair<std::size_t, std::string> cases[] = {
        {Sha256Stream::read_chunk_bytes - 1, "c27a704f6379855ce5af0db6208cfb79b4879f230290b48afd636eb4eaa3b166"},
        {Sha256Stream::read_chunk_bytes, "b7f7ba5ce5463b3c84a283f779d7a652cbf99122de5923ba51627607ff1497d5"},
        {Sha256Stream::read_chunk_bytes + 1, "5420b23d4fac850c1a38533d9427e301c0d1c398712bf0bac05a0d99163cd42a"},
    };
    for (const auto &[length, expected] : cases) {
        std::string bytes(length, char{});
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<char>((i * 131 + 7) & 0xff);
        }
        CHECK(sha256Hex(bytes) == expected);

        Sha256Stream split;
        std::size_t offset = 0;
        std::size_t step = 1;
        while (offset < bytes.size()) {
            const auto count = std::min(step, bytes.size() - offset);
            split.update(bytes.data() + offset, count);
            offset += count;
            step = step * 3 + 1;
        }
        CHECK(split.finishHex() == expected);
        CHECK_THROWS(split.finishHex());

        const auto file = sandbox.store / ("boundary-" + std::to_string(length) + ".bin");
        writeText(file, bytes);
        CHECK(assetFileSha256(file) == expected);
    }
}

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
