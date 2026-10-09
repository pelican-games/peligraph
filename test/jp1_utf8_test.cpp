#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/log.hpp"
#include "../src/project/assetsmanifest.hpp"
#include "../src/project/windowsutf8.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
namespace fs = std::filesystem;
using namespace Pelican;
using Json = nlohmann::json;

struct Project {
    fs::path root = fs::temp_directory_path() /
        ("pelican_jp1_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) /
        fs::path{u8"日本語 プロジェクト"};
    Project() { fs::create_directories(root / "assets/models"); }
    ~Project() { std::error_code ec; fs::remove_all(root.parent_path(), ec); }
};
}

TEST_CASE("JP-1 pathresolver assetsmanifest UTF-8 process contract 日本語 ソ表 …", "[jp1][pathresolver][assetsmanifest]") {
    REQUIRE(processUsesUtf8CodePage());
    Project project;
    const auto file = project.root / fs::path{u8"assets/models/木箱ソ表.glb"};
    std::ofstream{file, std::ios::binary} << "model bytes";
    REQUIRE(file.generic_string().ends_with("assets/models/木箱ソ表.glb"));
    REQUIRE(fs::path{file.string()} == file);
    if (!logger) setupLogger();
    auto &resolver = GET_MODULE(PathResolver);
    resolver.resetForTesting();
    resolver.setup(project.root, false);
    CHECK(resolver.resolveExistingFile("project://assets/models/木箱ソ表.glb") == fs::weakly_canonical(file));
    const auto manifest = project.root / "assets.manifest.json";
    const auto cache = project.root / ".pelican/cache.json";
    const auto generated = generateAssetsManifest(project.root / "assets", manifest, cache, "main", false);
    REQUIRE(generated.files == 1);
    std::ifstream input{manifest};
    const auto document = Json::parse(input);
    CHECK(document.dump().find("木箱ソ表.glb") != std::string::npos);
    CHECK(verifyAssetsManifest(project.root / "assets", manifest, cache, "main",
          {.include_content = true, .force_rehash = true, .strict = true}).issues.empty());
    resolver.resetForTesting();
}

TEST_CASE("JP-1 rpcserver survives Japanese missing paths and invalid UTF-8 errors", "[jp1][rpcserver]") {
    Project project;
    if (!logger) setupLogger();
    auto &resolver = GET_MODULE(PathResolver);
    resolver.resetForTesting();
    resolver.setup(project.root, false);
    std::istringstream input;
    std::ostringstream output;
    RpcServer server{input, output};
    server.setHandler("missing", [&](const Json &) -> Json {
        return resolver.resolveExistingFile("project://assets/models/見つからないソ表.glb").string();
    });
    const std::string bad = std::string{"File not found: "} + char(0x82) + " path";
    server.setHandler("bad", [&](const Json &) -> Json { throw std::runtime_error(bad); });
    server.setHandler("data", [&](const Json &) -> Json {
        throw JsonRpcHandlerError(-32000, bad, Json{{"path", bad}});
    });
    server.setHandler("ordered", [&](const Json &) -> Json {
        throw JsonRpcHandlerError(-32000, bad, nlohmann::ordered_json{{"path", bad}}, PreserveJsonRpcDataOrder{});
    });
    server.setHandler("status", [](const Json &) { return Json{{"running", true}}; });
    auto call = [&](const char *method) {
        return Json::parse(server.processLine(Json{{"jsonrpc", "2.0"}, {"id", 7}, {"method", method}}.dump()));
    };
    const auto missing = call("missing");
    REQUIRE(missing.contains("error"));
    CHECK(missing.at("error").at("message").get<std::string>().find("見つからないソ表.glb") != std::string::npos);
    for (const auto *method : {"bad", "data", "ordered"}) {
        const auto response = call(method);
        CHECK(response.at("error").at("message") == "File not found: \xEF\xBF\xBD path");
        if (response.at("error").contains("data"))
            CHECK(response.at("error").at("data").at("path") == "File not found: \xEF\xBF\xBD path");
        CHECK(call("status").at("result").at("running") == true);
    }
    resolver.resetForTesting();
}
