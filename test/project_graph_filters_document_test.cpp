#include "../src/project/graphfiltersdocument.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace {

Pelican::GraphFilters fixture() {
    Pelican::GraphFilters document;
    document.categories = {{"post", "Post"}};
    document.feature_categories = {{"bloom", {"post"}}};
    Pelican::GraphFilter child;
    child.id = "child";
    child.label = "Child";
    child.match.ops = {"value.add"};
    Pelican::GraphFilter parent;
    parent.id = "parent";
    parent.label = "Parent";
    parent.match.feature_categories = {"post"};
    parent.children = {child};
    document.filters = {parent};
    return document;
}

} // namespace

TEST_CASE("graph filter rendering is canonical, stable and round-trips", "[gf1b]") {
    const auto document = fixture();
    const auto bytes = Pelican::renderGraphFilters(document);
    CHECK(bytes == Pelican::renderGraphFilters(document));
    CHECK(bytes.starts_with("{\n  \"schema\": \"pelican.graph_filters\",\n  \"version\": 1,"));
    CHECK(bytes.ends_with("\n"));
    CHECK(bytes.find("\"children\"") != std::string::npos);
    auto parsed = Pelican::parseGraphFilters(nlohmann::json::parse(bytes));
    REQUIRE(parsed.document.has_value());
    CHECK(*parsed.document == document);

    Pelican::GraphFilters empty;
    const auto empty_bytes = Pelican::renderGraphFilters(empty);
    auto empty_parsed = Pelican::parseGraphFilters(nlohmann::json::parse(empty_bytes));
    REQUIRE(empty_parsed.document.has_value());
    CHECK(*empty_parsed.document == empty);
}

TEST_CASE("graph filter parsing is strict and never partially accepts", "[gf1b]") {
    auto root = nlohmann::json::parse(Pelican::renderGraphFilters(fixture()));
    SECTION("unknown key") {
        root["future"] = true;
        CHECK_FALSE(Pelican::parseGraphFilters(root).document.has_value());
    }
    SECTION("bad id") {
        root["filters"][0]["id"] = "Bad";
        CHECK_FALSE(Pelican::parseGraphFilters(root).document.has_value());
    }
    SECTION("duplicate child id") {
        root["filters"][0]["children"][0]["id"] = "parent";
        CHECK_FALSE(Pelican::parseGraphFilters(root).document.has_value());
    }
    SECTION("undeclared category") {
        root["filters"][0]["match"]["feature_categories"] = {"absent"};
        CHECK_FALSE(Pelican::parseGraphFilters(root).document.has_value());
    }
    SECTION("wrong version") {
        root["version"] = 2;
        CHECK_FALSE(Pelican::parseGraphFilters(root).document.has_value());
    }
}
