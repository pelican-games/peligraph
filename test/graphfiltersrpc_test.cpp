#include "../src/core/communication/graphfiltersrpc.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/project/graphfiltersdocument.hpp"
#include "../src/project/projectdocumenttransaction.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

namespace {
using Json = nlohmann::json;

struct Fixture {
    std::filesystem::path root;
    Pelican::PathResolver paths;
    std::istringstream input;
    std::ostringstream output;
    Pelican::RpcServer server{input, output};
    int next_id = 0;

    Fixture() {
        root = std::filesystem::temp_directory_path() /
               ("pelican_gf1b_rpc_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        paths.setup(root, false);
        Pelican::configureGraphFiltersRpcHandlers(server, paths);
    }
    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path file() const { return root / "graph_filters.json"; }
    Json call(const std::string &method, Json params = Json::object()) {
        return Json::parse(server.processLine(Json{{"jsonrpc", "2.0"}, {"id", ++next_id},
                                                   {"method", method}, {"params", std::move(params)}}.dump()));
    }
    Json result(const std::string &method, Json params = Json::object()) {
        const auto answer = call(method, std::move(params));
        INFO(answer.dump());
        REQUIRE(answer.contains("result"));
        return answer.at("result");
    }
    Json refusal(const std::string &method, Json params) {
        const auto answer = call(method, std::move(params));
        INFO(answer.dump());
        REQUIRE(answer.contains("error"));
        REQUIRE(answer.at("error").contains("data"));
        return answer.at("error").at("data");
    }
};

std::string readFile(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}
void writeFile(const std::filesystem::path &path, std::string_view bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output << bytes;
}
Json emptyDocument() {
    return Json{{"schema", "pelican.graph_filters"}, {"version", 1},
                {"categories", Json::array()}, {"feature_categories", Json::object()},
                {"filters", Json::array()}};
}
Json oneFilterDocument() {
    auto document = emptyDocument();
    document["filters"] = Json::array({{{"id", "left_only"}, {"label", "Left only"},
                                        {"match", {{"node_ids", {"left"}}}}}});
    return document;
}
}

TEST_CASE("GF1b graph filters RPC reads missing valid and invalid documents", "[gf1b]") {
    Fixture fixture;
    auto read = fixture.result("get_graph_filters");
    CHECK(read.at("digest").is_null());
    CHECK(read.at("document").is_null());
    CHECK(read.at("error").is_null());

    const auto document = oneFilterDocument();
    const auto canonical = Pelican::renderGraphFilters(*Pelican::parseGraphFilters(document).document);
    writeFile(fixture.file(), canonical);
    read = fixture.result("get_graph_filters");
    CHECK(read.at("digest") == Pelican::projectDocumentDigest(canonical));
    CHECK(read.at("document") == document);
    CHECK(read.at("error").is_null());

    writeFile(fixture.file(), "{not json\n");
    read = fixture.result("get_graph_filters");
    CHECK(read.at("digest") == Pelican::projectDocumentDigest("{not json\n"));
    CHECK(read.at("document").is_null());
    CHECK(read.at("error").get<std::string>().find("invalid JSON") != std::string::npos);
}

TEST_CASE("GF1b graph filters RPC creates replaces and preserves previous", "[gf1b]") {
    Fixture fixture;
    const auto document = oneFilterDocument();
    auto edited = fixture.result("edit_graph_filters", {{"base_digest", nullptr}, {"document", document}});
    REQUIRE(edited.at("committed") == true);
    REQUIRE(edited.at("changed") == true);
    CHECK(edited.at("previous_digest").is_null());
    CHECK(edited.at("previous").is_null());
    CHECK(edited.at("digest") == Pelican::projectDocumentDigest(readFile(fixture.file())));
    CHECK(readFile(fixture.file()) == Pelican::renderGraphFilters(*Pelican::parseGraphFilters(document).document));

    const auto first_digest = edited.at("digest");
    auto second = document;
    second["filters"][0]["match"]["node_ids"] = Json::array({"right"});
    edited = fixture.result("edit_graph_filters", {{"base_digest", first_digest}, {"document", second}});
    CHECK(edited.at("changed") == true);
    CHECK(edited.at("previous") == document);
    CHECK(edited.at("previous_digest") == first_digest);

    const auto before = readFile(fixture.file());
    const auto before_time = std::filesystem::last_write_time(fixture.file());
    edited = fixture.result("edit_graph_filters", {{"base_digest", edited.at("digest")}, {"document", second}});
    CHECK(edited.at("changed") == false);
    CHECK(edited.at("digest") == Pelican::projectDocumentDigest(before));
    CHECK(readFile(fixture.file()) == before);
    CHECK(std::filesystem::last_write_time(fixture.file()) == before_time);
}

TEST_CASE("GF1b graph filters RPC digest and validation refusals write nothing", "[gf1b]") {
    Fixture fixture;
    const auto existing = emptyDocument();
    const auto bytes = Pelican::renderGraphFilters(*Pelican::parseGraphFilters(existing).document);
    writeFile(fixture.file(), bytes);
    const auto digest = Pelican::projectDocumentDigest(bytes);

    auto refusal = fixture.refusal("edit_graph_filters", {{"base_digest", nullptr}, {"document", oneFilterDocument()}});
    CHECK(refusal.at("error_code") == "external_modification");
    CHECK(refusal.at("error_fields").at("expected").is_null());
    CHECK(refusal.at("error_fields").at("actual") == digest);
    CHECK(readFile(fixture.file()) == bytes);

    refusal = fixture.refusal("edit_graph_filters", {{"base_digest", "stale"}, {"document", oneFilterDocument()}});
    CHECK(refusal.at("error_code") == "external_modification");
    CHECK(refusal.at("error_fields").at("actual") == digest);
    CHECK(readFile(fixture.file()) == bytes);

    auto invalid = oneFilterDocument();
    invalid["filters"][0]["id"] = "duplicate";
    const auto duplicate = invalid["filters"][0];
    invalid["filters"].push_back(duplicate);
    refusal = fixture.refusal("edit_graph_filters", {{"base_digest", digest}, {"document", invalid}});
    CHECK(refusal.at("error_code") == "project.graph_filters_invalid@1");
    CHECK(refusal.at("error_fields").contains("reason"));
    CHECK(readFile(fixture.file()) == bytes);

    const std::string broken = "{\"schema\": true\n";
    writeFile(fixture.file(), broken);
    refusal = fixture.refusal("edit_graph_filters",
                              {{"base_digest", Pelican::projectDocumentDigest(broken)}, {"document", oneFilterDocument()}});
    CHECK(refusal.at("error_code") == "project.graph_filters_invalid@1");
    CHECK(readFile(fixture.file()) == broken);
}

TEST_CASE("GF1b graph filters RPC reports a busy transaction without writing", "[gf1b]") {
    Fixture fixture;
    std::filesystem::create_directories(fixture.root / ".pelican" / "render-authoring.transaction");
    const auto refusal = fixture.refusal("edit_graph_filters", {{"base_digest", nullptr}, {"document", emptyDocument()}});
    CHECK(refusal.at("error_code") == "project.graph_filters_transaction_busy@1");
    CHECK(refusal.at("error_fields").contains("path"));
    CHECK_FALSE(std::filesystem::exists(fixture.file()));
}
