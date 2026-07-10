#include "../src/core/loader/assetsverification.hpp"
#include "../src/core/log.hpp"
#include "../src/project/assetsmanifest.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace Pelican {

namespace {

struct Sandbox {
    std::filesystem::path root;
    std::filesystem::path store;
    std::filesystem::path manifest;
    std::filesystem::path cache;

    Sandbox() {
        root = std::filesystem::temp_directory_path() /
               ("pelican_assets_startup_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        store = root / "assets";
        manifest = root / "assets.manifest.json";
        cache = root / ".pelican" / "assets-hash-cache.json";
        std::filesystem::create_directories(store);
        std::ofstream{store / "asset.bin", std::ios::binary} << "original";
        (void)generateAssetsManifest(store, manifest, cache, "main", true);
        std::ofstream{store / "asset.bin", std::ios::binary | std::ios::trunc} << "changed";
    }

    ~Sandbox() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

void ensureLogger() {
    if (logger == nullptr) {
        setupLogger();
    }
}

} // namespace

TEST_CASE("startup assets verification continues normally and fails only in strict mode",
          "[assets-manifest][startup]") {
    ensureLogger();
    Sandbox sandbox;
    const std::vector<AssetStoreStatus> stores{{"main", "assets", sandbox.store, sandbox.manifest}};

    const auto normal = verifyAssetsAtStartup(stores, sandbox.cache, false);
    REQUIRE(normal.stores == 1);
    REQUIRE(normal.info == 1);
    REQUIRE(normal.errors == 0);
    REQUIRE(normal.shouldContinueLoading());

    const auto strict = verifyAssetsAtStartup(stores, sandbox.cache, true);
    REQUIRE(strict.stores == 1);
    REQUIRE(strict.info == 0);
    REQUIRE(strict.errors == 1);
    REQUIRE_FALSE(strict.shouldContinueLoading());

    const std::vector<AssetStoreStatus> invalid_manifest_store{
        {"broken", "assets", sandbox.store, std::nullopt, "manifest reference cannot be resolved"}};
    const auto invalid_normal = verifyAssetsAtStartup(invalid_manifest_store, sandbox.cache, false);
    REQUIRE(invalid_normal.warnings == 1);
    REQUIRE(invalid_normal.shouldContinueLoading());
    const auto invalid_strict = verifyAssetsAtStartup(invalid_manifest_store, sandbox.cache, true);
    REQUIRE(invalid_strict.errors == 1);
    REQUIRE_FALSE(invalid_strict.shouldContinueLoading());
}

} // namespace Pelican
