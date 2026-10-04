#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/loader/assetsverification.hpp"
#include "../src/core/log.hpp"
#include "../src/project/assetsmanifest.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <sstream>
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

namespace {

// AV-1: a store whose manifest lists a.bin and b.bin; on disk a.bin has
// other bytes, b.bin is gone and c.bin is not listed.
struct DriftedStore {
    std::filesystem::path root;
    std::filesystem::path store;
    std::filesystem::path manifest;
    std::filesystem::path cache;
    std::filesystem::path plain;

    static void write(const std::filesystem::path &path, std::string_view bytes) {
        std::ofstream{path, std::ios::binary | std::ios::trunc} << bytes;
    }

    DriftedStore() {
        root = std::filesystem::temp_directory_path() /
               ("pelican_av1_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        store = root / "assets";
        manifest = root / "assets.manifest.json";
        cache = root / ".pelican" / "assets-hash-cache.json";
        plain = root / "plain";
        std::filesystem::create_directories(store);
        std::filesystem::create_directories(plain);
        write(store / "a.bin", "original");
        write(store / "b.bin", "second");
        (void)generateAssetsManifest(store, manifest, cache, "main", true);
        write(store / "a.bin", "changed!!");
        std::filesystem::remove(store / "b.bin");
        write(store / "c.bin", "extra");
    }

    void restore() const {
        write(store / "a.bin", "original");
        write(store / "b.bin", "second");
        std::filesystem::remove(store / "c.bin");
    }

    std::vector<AssetStoreStatus> stores() const {
        return {{"main", "assets", store, manifest}, {"plain", "plain", plain}};
    }

    ~DriftedStore() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

struct VerificationWire {
    std::istringstream input;
    std::ostringstream output;
    RpcServer server{input, output};
    int next_id = 1;

    explicit VerificationWire(AssetsVerification *verification) {
        internal::configureAssetsVerificationRpcHandlers(server, verification);
        server.setHandler("get_status", [verification](const nlohmann::json &) {
            return nlohmann::json{
                {"assets_verification", internal::assetsVerificationStatusJson(verification)}};
        });
    }

    nlohmann::json call(std::string_view method, nlohmann::json params = nlohmann::json::object()) {
        const nlohmann::json request{
            {"jsonrpc", "2.0"}, {"id", next_id++}, {"method", std::string{method}}, {"params", std::move(params)}};
        return nlohmann::json::parse(server.processLine(request.dump()));
    }

    nlohmann::json status() { return call("get_status").at("result").at("assets_verification"); }
};

const nlohmann::json *issueOfKind(const nlohmann::json &store, std::string_view kind) {
    for (const auto &issue : store.at("issues")) {
        if (issue.at("kind").get<std::string>() == kind) return &issue;
    }
    return nullptr;
}

} // namespace

TEST_CASE("AV-1 get_assets_verification returns every issue by kind and get_status counts them",
          "[assets-manifest][av1]") {
#if !PELICAN_WITH_HOT_RELOAD
    SKIP("PELICAN_WITH_HOT_RELOAD=OFF: the startup check does not run unless strict (AV-1 distribution build startup covers it)");
#else
    ensureLogger();
    DriftedStore sandbox;
    AssetsVerification verification;
    const auto summary = verification.verifyAtStartup(sandbox.stores(), sandbox.cache, false);
    REQUIRE(summary.shouldContinueLoading());
    REQUIRE(summary.stores == 1);
    VerificationWire wire{&verification};

    const auto detail = wire.call("get_assets_verification").at("result");
    REQUIRE(detail.at("state") == "verified");
    REQUIRE(detail.at("verified_at").is_string());
    REQUIRE(detail.at("stores").size() == 1);
    const auto &store = detail.at("stores").at(0);
    REQUIRE(store.at("name") == "main");
    REQUIRE(store.at("root") == sandbox.store.generic_string());
    REQUIRE(store.at("manifest") == sandbox.manifest.generic_string());
    REQUIRE(store.at("matched") == 1);
    REQUIRE(store.at("hashed") == 1);
    REQUIRE(store.at("cached") == 0);
    REQUIRE(store.at("issues").size() == 3);
    const auto *differs = issueOfKind(store, "content_mismatch");
    const auto *missing = issueOfKind(store, "missing");
    const auto *extra = issueOfKind(store, "extra");
    REQUIRE(differs != nullptr);
    REQUIRE(missing != nullptr);
    REQUIRE(extra != nullptr);
    REQUIRE(differs->at("file") == "a.bin");
    REQUIRE(differs->at("severity") == "info");
    REQUIRE(missing->at("file") == "b.bin");
    REQUIRE(missing->at("severity") == "warning");
    REQUIRE(extra->at("file") == "c.bin");
    REQUIRE(extra->at("severity") == "warning");
    REQUIRE(differs->at("message").get<std::string>().starts_with("asset content differs: a.bin"));

    const auto status = wire.status();
    REQUIRE(status == nlohmann::json{{"state", "verified"},
                                     {"stores", 1},
                                     {"matched", 1},
                                     {"info", 1},
                                     {"warnings", 2},
                                     {"errors", 0}});
    REQUIRE(status.at("matched") == summary.matched);
    REQUIRE(status.at("info") == summary.info);
    REQUIRE(status.at("warnings") == summary.warnings);

    const auto refused = wire.call("get_assets_verification", {{"store", "main"}});
    REQUIRE(refused.at("error").at("code") == JsonRpcErrorCodes::invalidParams);
#endif
}

TEST_CASE("AV-1 a project whose stores declare no manifest is not_run with the reason",
          "[assets-manifest][av1]") {
#if !PELICAN_WITH_HOT_RELOAD
    SKIP("PELICAN_WITH_HOT_RELOAD=OFF: the startup check does not run unless strict (AV-1 distribution build startup covers it)");
#else
    ensureLogger();
    DriftedStore sandbox;
    AssetsVerification verification;
    const std::vector<AssetStoreStatus> stores{{"plain", "plain", sandbox.plain}};
    REQUIRE(verification.verifyAtStartup(stores, sandbox.cache, false).stores == 0);
    VerificationWire wire{&verification};

    const nlohmann::json not_run{{"state", "not_run"}, {"reason", "no asset store declares a manifest"}};
    REQUIRE(wire.status() == not_run);
    const auto detail = wire.call("get_assets_verification").at("result");
    REQUIRE(detail.at("state") == "not_run");
    REQUIRE(detail.at("reason") == "no asset store declares a manifest");
    REQUIRE(detail.at("stores").empty());
    REQUIRE(wire.call("verify_assets").at("result") == not_run);

    // A host that never ran the startup check.
    VerificationWire absent{nullptr};
    REQUIRE(absent.status() ==
            nlohmann::json{{"state", "not_run"}, {"reason", "this host does not verify assets"}});
    const auto refused = absent.call("verify_assets");
    REQUIRE(refused.at("error").at("code") == JsonRpcErrorCodes::applicationError);
    REQUIRE(refused.at("error").at("data").at("error_code") == "assets_verification_unavailable@1");
#endif
}

TEST_CASE("AV-1 verify_assets reruns on a worker, is running meanwhile, and a second call joins it",
          "[assets-manifest][av1]") {
#if !PELICAN_WITH_HOT_RELOAD
    SKIP("PELICAN_WITH_HOT_RELOAD=OFF: the startup check does not run unless strict (AV-1 distribution build startup covers it)");
#else
    ensureLogger();
    DriftedStore sandbox;
    AssetsVerification verification;
    (void)verification.verifyAtStartup(sandbox.stores(), sandbox.cache, false);
    VerificationWire wire{&verification};
    REQUIRE(wire.status().at("warnings") == 2);
    const auto before = wire.call("get_assets_verification").at("result");

    sandbox.restore();
    verification.holdForTesting(true);
    REQUIRE(wire.call("verify_assets").at("result") == nlohmann::json{{"state", "running"}});
    REQUIRE(wire.status() == nlohmann::json{{"state", "running"}});
    // Joins the run in progress rather than starting another.
    REQUIRE(wire.call("verify_assets").at("result") == nlohmann::json{{"state", "running"}});
    // Meanwhile the previous result is still the one there is.
    const auto during = wire.call("get_assets_verification").at("result");
    REQUIRE(during.at("state") == "running");
    REQUIRE(during.at("stores") == before.at("stores"));

    verification.holdForTesting(false);
    verification.waitForTesting();
    REQUIRE(wire.status() == nlohmann::json{{"state", "verified"},
                                            {"stores", 1},
                                            {"matched", 2},
                                            {"info", 0},
                                            {"warnings", 0},
                                            {"errors", 0}});
    const auto after = wire.call("get_assets_verification").at("result");
    REQUIRE(after.at("stores").at(0).at("issues").empty());
    REQUIRE(after.at("stores").at(0).at("matched") == 2);
    REQUIRE(after.at("stores").at(0).at("hashed").get<int>() +
                after.at("stores").at(0).at("cached").get<int>() ==
            2);
#endif
}

#if !PELICAN_WITH_HOT_RELOAD
// EB-4: what the startup cases above check, in a distribution build. The
// startup check is not run unless strict (the reason says why), the explicit
// verify_assets still runs, and --strict-assets checks as the ON build does.
TEST_CASE("AV-1 distribution build startup: not_run unless strict, verify_assets still runs",
          "[assets-manifest][av1][eb4]") {
    ensureLogger();
    DriftedStore sandbox;
    AssetsVerification verification;
    const auto summary = verification.verifyAtStartup(sandbox.stores(), sandbox.cache, false);
    REQUIRE(summary.shouldContinueLoading());
    REQUIRE(summary.stores == 0);
    VerificationWire wire{&verification};
    const nlohmann::json not_run{{"state", "not_run"}, {"reason", "PELICAN_WITH_HOT_RELOAD=OFF"}};
    REQUIRE(wire.status() == not_run);
    const auto detail = wire.call("get_assets_verification").at("result");
    REQUIRE(detail.at("state") == "not_run");
    REQUIRE(detail.at("reason") == "PELICAN_WITH_HOT_RELOAD=OFF");
    REQUIRE(detail.at("verified_at").is_null());
    REQUIRE(detail.at("stores").empty());

    REQUIRE(wire.call("verify_assets").at("result") == nlohmann::json{{"state", "running"}});
    verification.waitForTesting();
    REQUIRE(wire.status() == nlohmann::json{{"state", "verified"},
                                            {"stores", 1},
                                            {"matched", 1},
                                            {"info", 1},
                                            {"warnings", 2},
                                            {"errors", 0}});

    AssetsVerification strict;
    const auto strict_summary = strict.verifyAtStartup(sandbox.stores(), sandbox.cache, true);
    REQUIRE_FALSE(strict_summary.shouldContinueLoading());
    REQUIRE(strict_summary.stores == 1);
    VerificationWire strict_wire{&strict};
    REQUIRE(strict_wire.status() == nlohmann::json{{"state", "verified"},
                                                   {"stores", 1},
                                                   {"matched", 1},
                                                   {"info", 0},
                                                   {"warnings", 0},
                                                   {"errors", 3}});
}
#endif

TEST_CASE("AV-1 update_assets_manifest rewrites the manifest and the rerun matches everything",
          "[assets-manifest][av1]") {
    ensureLogger();
    DriftedStore sandbox;
    AssetsVerification verification;
    (void)verification.verifyAtStartup(sandbox.stores(), sandbox.cache, false);
    VerificationWire wire{&verification};

    const auto written = wire.call("update_assets_manifest", {{"store", "main"}}).at("result");
    REQUIRE(written.at("store") == "main");
    REQUIRE(written.at("manifest") == sandbox.manifest.generic_string());
    REQUIRE(written.at("files") == 2);
    REQUIRE(written.at("state") == "running");
    verification.waitForTesting();

    const auto manifest = parseAssetsManifestJson([&] {
        std::ifstream file{sandbox.manifest, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    }());
    REQUIRE(manifest.files.size() == 2);
    REQUIRE(manifest.files.at(0).file == "a.bin");
    REQUIRE(manifest.files.at(0).size == 9);
    REQUIRE(manifest.files.at(1).file == "c.bin");

    REQUIRE(wire.status() == nlohmann::json{{"state", "verified"},
                                            {"stores", 1},
                                            {"matched", 2},
                                            {"info", 0},
                                            {"warnings", 0},
                                            {"errors", 0}});
    const auto after = wire.call("get_assets_verification").at("result");
    REQUIRE(after.at("stores").at(0).at("issues").empty());
    REQUIRE(after.at("stores").at(0).at("cached") == 2);

    const auto unknown = wire.call("update_assets_manifest", {{"store", "nope"}});
    REQUIRE(unknown.at("error").at("code") == JsonRpcErrorCodes::applicationError);
    REQUIRE(unknown.at("error").at("message") == "update_assets_manifest: no asset store is named 'nope'");
    REQUIRE(unknown.at("error").at("data") ==
            nlohmann::json{{"error_code", "assets_store_unknown@1"}, {"error_fields", {{"store", "nope"}}}});

    const auto undeclared = wire.call("update_assets_manifest", {{"store", "plain"}});
    REQUIRE(undeclared.at("error").at("code") == JsonRpcErrorCodes::applicationError);
    REQUIRE(undeclared.at("error").at("message") ==
            "update_assets_manifest: asset store 'plain' declares no manifest in project.json");
    REQUIRE(undeclared.at("error").at("data") ==
            nlohmann::json{{"error_code", "assets_manifest_undeclared@1"}, {"error_fields", {{"store", "plain"}}}});

    REQUIRE(wire.call("update_assets_manifest").at("error").at("code") == JsonRpcErrorCodes::invalidParams);
}

// EB-1: update_assets_manifest while a rerun is reading the stores is
// refused at once (the RPC thread, which is the frame's, never waits for the
// hashing), and goes through once the run is done.
TEST_CASE("EB-1 update_assets_manifest during a verification run is refused at once and works after it",
          "[assets-manifest][av1][eb1]") {
    ensureLogger();
    DriftedStore sandbox;
    AssetsVerification verification;
    (void)verification.verifyAtStartup(sandbox.stores(), sandbox.cache, false);
    VerificationWire wire{&verification};
    const auto manifest_before = [&] {
        std::ifstream file{sandbox.manifest, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    };
    const auto before = manifest_before();

    // Held, the worker holds the stores as a run that is hashing does.
    verification.holdForTesting(true);
    REQUIRE(wire.call("verify_assets").at("result") == nlohmann::json{{"state", "running"}});
    auto answer = std::async(std::launch::async, [&] {
        return wire.call("update_assets_manifest", {{"store", "main"}});
    });
    const bool answered = answer.wait_for(std::chrono::seconds{10}) == std::future_status::ready;
    if (!answered) verification.holdForTesting(false);
    REQUIRE(answered);
    const auto refused = answer.get();
    REQUIRE(refused.contains("error"));
    CHECK(refused.at("error").at("code") == JsonRpcErrorCodes::applicationError);
    CHECK(refused.at("error").at("data") ==
          nlohmann::json{{"error_code", "assets_verification_running@1"}, {"error_fields", {{"store", "main"}}}});
    CHECK(refused.at("error").at("message").get<std::string>().starts_with("update_assets_manifest: "));
    CHECK(manifest_before() == before);
    CHECK(wire.status() == nlohmann::json{{"state", "running"}});

    verification.holdForTesting(false);
    verification.waitForTesting();
    const auto written = wire.call("update_assets_manifest", {{"store", "main"}});
    REQUIRE(written.contains("result"));
    CHECK(written.at("result").at("files") == 2);
    verification.waitForTesting();
    CHECK(wire.status().at("warnings") == 0);
    CHECK(manifest_before() != before);
}

} // namespace Pelican
