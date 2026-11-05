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

namespace {

Pelican::GraphFilter simpleFilter(std::string id, std::vector<Pelican::GraphFilter> children = {}) {
    const std::string label = "Label " + id;
    return {std::move(id), label, {}, std::move(children)};
}

void roundTrips(const Pelican::GraphFilters &document) {
    const auto parsed = Pelican::parseGraphFilters(nlohmann::json::parse(Pelican::renderGraphFilters(document)));
    REQUIRE(parsed.document.has_value());
    CHECK(*parsed.document == document);
}

} // namespace

TEST_CASE("GF-1c merge appends distinct filters into an empty or populated base", "[gf1c]") {
    Pelican::GraphFilters base;
    Pelican::GraphFilters incoming;
    incoming.filters = {simpleFilter("new")};
    auto result = Pelican::mergeGraphFilters(base, incoming);
    CHECK(result.merged == incoming);
    CHECK(result.skipped_filters.empty());
    roundTrips(result.merged);

    base.filters = {simpleFilter("base")};
    incoming.filters = {simpleFilter("new")};
    result = Pelican::mergeGraphFilters(base, incoming);
    REQUIRE(result.merged.filters.size() == 2);
    CHECK(result.merged.filters[0].id == "base");
    CHECK(result.merged.filters[1].id == "new");
    roundTrips(result.merged);
}

TEST_CASE("GF-1c merge refuses a whole filter when any id collides", "[gf1c]") {
    Pelican::GraphFilters base;
    base.filters = {simpleFilter("top", {simpleFilter("base_child")})};
    Pelican::GraphFilters incoming;
    SECTION("top level") {
        incoming.filters = {simpleFilter("top", {simpleFilter("other")})};
    }
    SECTION("child") {
        incoming.filters = {simpleFilter("other", {simpleFilter("base_child")})};
    }
    SECTION("child against base top level") {
        incoming.filters = {simpleFilter("other", {simpleFilter("top")})};
    }
    const auto result = Pelican::mergeGraphFilters(base, incoming);
    CHECK(result.merged == base);
    CHECK(result.skipped_filters == std::vector<std::string>{incoming.filters[0].id});
    roundTrips(result.merged);
}

TEST_CASE("GF-1c merge keeps base category labels and unions feature categories in order", "[gf1c]") {
    Pelican::GraphFilters base;
    base.categories = {{"same", "Same"}, {"conflict", "Base"}};
    base.feature_categories = {{"feature", {"same"}}};
    Pelican::GraphFilters incoming;
    incoming.categories = {{"same", "Same"}, {"conflict", "Incoming"}, {"added", "Added"}};
    incoming.feature_categories = {{"feature", {"conflict", "added"}}, {"other", {"added"}}};
    const auto result = Pelican::mergeGraphFilters(base, incoming);
    CHECK(result.merged.categories ==
          std::vector<Pelican::GraphFilterCategory>{{"same", "Same"}, {"conflict", "Base"}, {"added", "Added"}});
    CHECK(result.skipped_categories == std::vector<std::string>{"conflict"});
    REQUIRE(result.merged.feature_categories.size() == 2);
    CHECK(result.merged.feature_categories[0].second == std::vector<std::string>{"same", "conflict", "added"});
    CHECK(result.merged.feature_categories[1] == std::make_pair(std::string{"other"}, std::vector<std::string>{"added"}));
    roundTrips(result.merged);
}

TEST_CASE("GF-1c merging can exceed the strict depth limit", "[gf1c]") {
    Pelican::GraphFilters base;
    base.filters = {simpleFilter("base")};
    Pelican::GraphFilters incoming;
    incoming.filters = {simpleFilter("f1")};
    auto *filter = &incoming.filters[0];
    for (int depth = 2; depth <= 9; ++depth) {
        filter->children = {simpleFilter("f" + std::to_string(depth))};
        filter = &filter->children[0];
    }
    const auto result = Pelican::mergeGraphFilters(base, incoming);
    CHECK(result.skipped_filters.empty());
    const auto parsed = Pelican::parseGraphFilters(nlohmann::json::parse(Pelican::renderGraphFilters(result.merged)));
    CHECK_FALSE(parsed.document.has_value());
    CHECK(parsed.error.find("deeper") != std::string::npos);
}
