#include "graphfilters.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace {

using Json = nlohmann::json;
using PelicanStudio::findGraphFilter;
using PelicanStudio::GraphFilter;
using PelicanStudio::GraphFilterFacts;
using PelicanStudio::GraphFilters;
using PelicanStudio::GraphFiltersRead;
using PelicanStudio::graphFilterMatches;
using PelicanStudio::readGraphFilters;

// A fresh directory per test case, removed with the object. The reader takes a
// project root, so each case needs a graph_filters.json of its own.
class ProjectFiles {
public:
    ProjectFiles() {
        static std::atomic<unsigned> sequence{0};
        static const auto seed = std::random_device{}();
        root_ = std::filesystem::temp_directory_path() /
                ("pelican_gf1a_" + std::to_string(seed) + "_" +
                 std::to_string(sequence.fetch_add(1)));
        std::filesystem::create_directories(root_);
    }
    ~ProjectFiles() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }
    ProjectFiles(const ProjectFiles &) = delete;
    ProjectFiles &operator=(const ProjectFiles &) = delete;

    const std::filesystem::path &root() const { return root_; }
    void write(const std::string &text) const {
        std::ofstream stream{root_ / "graph_filters.json", std::ios::binary};
        stream << text;
    }

private:
    std::filesystem::path root_;
};

Json validDocument() {
    return {
        {"schema", "pelican.graph_filters"},
        {"version", 1},
        {"categories", Json::array({{{"id", "post"}, {"label", "Post"}},
                                    {{"id", "pre"}, {"label", "Pre"}}})},
        {"feature_categories", {{"bloom", Json::array({"post"})},
                                {"ssao", Json::array({"pre"})}}},
        {"filters",
         Json::array({
             {{"id", "post_only"},
              {"label", "Post"},
              {"match", {{"feature_categories", Json::array({"post"})}}}},
             {{"id", "canvas"},
              {"label", "Canvas"},
              {"match", {{"ops", Json::array({"canvas."})}}},
              {"children",
               Json::array({{{"id", "canvas_text"},
                             {"label", "Canvas text"},
                             {"match", {{"ops", Json::array({"canvas.text"})}}}}})}},
         })},
    };
}

Json chain(int depth, int total) {
    Json filter{{"id", "f" + std::to_string(depth)},
                {"label", "F" + std::to_string(depth)},
                {"match", Json::object()}};
    if (depth < total) filter["children"] = Json::array({chain(depth + 1, total)});
    return filter;
}

Json chainDocument(int total) {
    return {{"schema", "pelican.graph_filters"},
            {"version", 1},
            {"filters", Json::array({chain(1, total)})}};
}

void expectInvalid(const std::string &text) {
    ProjectFiles files;
    files.write(text);
    const auto read = readGraphFilters(files.root());
    CHECK_FALSE(read.document.has_value());
    CHECK_FALSE(read.error.empty());
    CHECK(read.error.find("graph_filters.json") != std::string::npos);
}

} // namespace

TEST_CASE("graph_filters.json is read whole, children included", "[studio][gf1a]") {
    ProjectFiles files;
    files.write(validDocument().dump());

    const auto read = readGraphFilters(files.root());
    REQUIRE(read.document.has_value());
    CHECK(read.error.empty());
    const auto &document = *read.document;
    REQUIRE(document.categories.size() == 2);
    CHECK(document.categories.at(0).id == "post");
    CHECK(document.categories.at(0).label == "Post");
    REQUIRE(document.feature_categories.size() == 2);
    CHECK(document.feature_categories.at(0).first == "bloom");
    REQUIRE(document.filters.size() == 2);
    CHECK(document.filters.at(0).id == "post_only");
    CHECK(document.filters.at(0).label == "Post");
    REQUIRE(document.filters.at(0).match.feature_categories.size() == 1);
    CHECK(document.filters.at(0).match.feature_categories.front() == "post");
    REQUIRE(document.filters.at(1).children.size() == 1);
    CHECK(document.filters.at(1).children.front().id == "canvas_text");
    CHECK(findGraphFilter(document, "canvas_text") ==
          &document.filters.at(1).children.front());
    CHECK(findGraphFilter(document, "missing") == nullptr);
}

TEST_CASE("graph_filters.json may be absent, and that is not an error", "[studio][gf1a]") {
    ProjectFiles files;
    const auto read = readGraphFilters(files.root());
    CHECK_FALSE(read.document.has_value());
    CHECK(read.error.empty());
}

TEST_CASE("graph_filters.json is accepted at the depth and size limits", "[studio][gf1a]") {
    SECTION("eight filters deep is enough") {
        ProjectFiles files;
        files.write(chainDocument(8).dump());
        const auto read = readGraphFilters(files.root());
        CHECK(read.document.has_value());
        CHECK(read.error.empty());
    }
    SECTION("a ninth is refused") {
        ProjectFiles files;
        files.write(chainDocument(9).dump());
        const auto read = readGraphFilters(files.root());
        CHECK_FALSE(read.document.has_value());
        CHECK(read.error.find("deeper") != std::string::npos);
    }
    SECTION("one mebibyte of spaces is over the limit") {
        ProjectFiles files;
        auto text = validDocument().dump();
        text.append(1024u * 1024u, ' ');
        files.write(text);
        const auto read = readGraphFilters(files.root());
        CHECK_FALSE(read.document.has_value());
        CHECK(read.error.find("1 MiB") != std::string::npos);
    }
}

TEST_CASE("graph_filters.json is refused whole on any invalid part", "[studio][gf1a]") {
    SECTION("invalid JSON") {
        expectInvalid("{");
    }
    SECTION("unknown root key") {
        auto document = validDocument();
        document["future"] = true;
        expectInvalid(document.dump());
    }
    SECTION("unknown filter key is not ignored") {
        auto document = validDocument();
        document["filters"][0]["future"] = true;
        expectInvalid(document.dump());
    }
    SECTION("unknown match key is not ignored") {
        auto document = validDocument();
        document["filters"][0]["match"]["future"] = Json::array();
        expectInvalid(document.dump());
    }
    SECTION("unknown category key is not ignored") {
        auto document = validDocument();
        document["categories"][0]["future"] = true;
        expectInvalid(document.dump());
    }
    SECTION("unknown child key is not ignored") {
        auto document = validDocument();
        document["filters"][1]["children"][0]["future"] = true;
        expectInvalid(document.dump());
    }
    SECTION("wrong schema") {
        auto document = validDocument();
        document["schema"] = "pelican.graph_filters.v2";
        expectInvalid(document.dump());
    }
    SECTION("version must be the integer 1") {
        auto document = validDocument();
        document["version"] = 2;
        expectInvalid(document.dump());
        document["version"] = "1";
        expectInvalid(document.dump());
    }
    SECTION("filters is required") {
        auto document = validDocument();
        document.erase("filters");
        expectInvalid(document.dump());
    }
    SECTION("ids are lower-case and underscored") {
        auto document = validDocument();
        document["filters"][0]["id"] = "Post";
        expectInvalid(document.dump());
    }
    SECTION("a child id is held to the same rule") {
        auto document = validDocument();
        document["filters"][1]["children"][0]["id"] = "canvas-text";
        expectInvalid(document.dump());
    }
    SECTION("filter ids are unique across the tree") {
        auto document = validDocument();
        document["filters"][1]["children"][0]["id"] = "post_only";
        expectInvalid(document.dump());
    }
    SECTION("labels are not empty") {
        auto document = validDocument();
        document["filters"][0]["label"] = "";
        expectInvalid(document.dump());
    }
    SECTION("match is required and must be an object") {
        auto document = validDocument();
        document["filters"][0].erase("match");
        expectInvalid(document.dump());
        document["filters"][0]["match"] = Json::array();
        expectInvalid(document.dump());
    }
    SECTION("a match category must be declared") {
        auto document = validDocument();
        document["filters"][0]["match"]["feature_categories"] = Json::array({"missing"});
        expectInvalid(document.dump());
    }
    SECTION("a feature category must be declared") {
        auto document = validDocument();
        document["feature_categories"]["bloom"] = Json::array({"missing"});
        expectInvalid(document.dump());
    }
    SECTION("a category id is unique") {
        auto document = validDocument();
        document["categories"][1]["id"] = "post";
        expectInvalid(document.dump());
    }
    SECTION("match entries are non-empty strings") {
        auto document = validDocument();
        document["filters"][1]["match"]["ops"] = Json::array({""});
        expectInvalid(document.dump());
    }
}

TEST_CASE("a graph filter is ORed across its keys and ANDed with Show", "[studio][gf1a]") {
    GraphFilters document;
    document.categories = {{"post", "Post"}, {"pre", "Pre"}};
    document.feature_categories = {{"bloom", {"post"}}, {"ssao", {"pre"}}};
    GraphFilter filter;
    filter.id = "mix";
    filter.label = "Mix";
    filter.match.ops = {"canvas.", "render.bloom"};
    filter.match.node_ids = {"named"};
    filter.match.features = {"bloom"};
    filter.match.feature_categories = {"pre"};

    const auto node = [&](std::string id, std::string op) {
        GraphFilterFacts facts;
        facts.kind = GraphFilterFacts::Kind::node;
        facts.node_id = std::move(id);
        facts.op = std::move(op);
        return graphFilterMatches(filter, document, facts);
    };
    CHECK(node("a", "canvas.stack"));
    CHECK(node("a", "canvas."));
    CHECK_FALSE(node("a", "canvasx"));
    CHECK(node("a", "render.bloom"));
    CHECK_FALSE(node("a", "render.bloom.extra"));
    CHECK(node("named", "value.add"));
    CHECK_FALSE(node("other", "value.add"));

    const auto folded = [&](GraphFilterFacts::Kind kind, std::string feature) {
        GraphFilterFacts facts;
        facts.kind = kind;
        facts.feature_name = std::move(feature);
        return graphFilterMatches(filter, document, facts);
    };
    CHECK(folded(GraphFilterFacts::Kind::folded_feature, "bloom"));
    CHECK(folded(GraphFilterFacts::Kind::folded_feature, "ssao"));
    CHECK_FALSE(folded(GraphFilterFacts::Kind::folded_feature, "tonemap"));
    CHECK_FALSE(folded(GraphFilterFacts::Kind::folded_other, "bloom"));

    GraphFilter empty;
    empty.id = "empty";
    GraphFilterFacts facts;
    facts.op = "canvas.stack";
    facts.node_id = "named";
    facts.feature_name = "bloom";
    CHECK_FALSE(graphFilterMatches(empty, document, facts));
    facts.kind = GraphFilterFacts::Kind::folded_feature;
    CHECK_FALSE(graphFilterMatches(empty, document, facts));
}
