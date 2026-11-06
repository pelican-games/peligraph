#include "graphfilters.hpp"

#include <nlohmann/json.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {

using Json = nlohmann::json;
using namespace PelicanStudio;

std::uint64_t seed0() {
    if (const char *text = std::getenv("READ_FUZZ_SEED0")) return std::strtoull(text, nullptr, 0);
    return 0x9e3779b97f4a7c15ULL;
}
unsigned fuzzCount(unsigned fallback) {
    if (const char *text = std::getenv("READ_FUZZ_COUNT"))
        return static_cast<unsigned>(std::strtoul(text, nullptr, 0));
    return fallback;
}

Json validDocument() {
    return {{"schema", "pelican.graph_filters"}, {"version", 1},
            {"categories", Json::array({{{"id", "post"}, {"label", "Post"}}})},
            {"feature_categories", {{"bloom", Json::array({"post"})}}},
            {"filters", Json::array({
                {{"id", "post_only"}, {"label", "Post"},
                 {"match", {{"feature_categories", Json::array({"post"})}}}},
                {{"id", "canvas"}, {"label", "Canvas"},
                 {"match", {{"ops", Json::array({"canvas."})}}},
                 {"children", Json::array({{{"id", "canvas_text"},
                     {"label", "Canvas text"}, {"match", Json::object()}}})}}})}};
}

struct Project {
    Project() {
        static std::atomic<unsigned> next{};
        root = std::filesystem::temp_directory_path() /
               ("pelican_read_fuzz_filters_" + std::to_string(std::random_device{}()) +
                "_" + std::to_string(next++));
        std::filesystem::create_directories(root);
    }
    ~Project() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    void write(const std::string &text) {
        std::ofstream out(root / "graph_filters.json", std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    std::filesystem::path root;
};

std::vector<Json *> nodes(Json &value) {
    std::vector<Json *> result{&value};
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            auto nested = nodes(it.value());
            result.insert(result.end(), nested.begin(), nested.end());
        }
    } else if (value.is_array()) {
        for (auto &entry : value) {
            auto nested = nodes(entry);
            result.insert(result.end(), nested.begin(), nested.end());
        }
    }
    return result;
}

void filterProperties(unsigned count, std::uint64_t seed) {
    Project project;
    std::mt19937_64 random(seed);
    for (unsigned iteration = 0; iteration < count; ++iteration) {
        Json document = validDocument();
        const unsigned mutation = iteration % 12;
        if (mutation < 6) {
            auto all = nodes(document);
            Json *selected = all[random() % all.size()];
            if (mutation == 0 && selected->is_object() && !selected->empty())
                selected->erase(selected->begin());
            else if (mutation == 1 && selected->is_object() && !selected->empty())
                (*selected)["duplicate"] = *selected->begin();
            else if (mutation == 2)
                *selected = Json::array({nullptr, true, -1, 1.0e308, "text"});
            else if (mutation == 3 && selected->is_object())
                (*selected)[std::string(random() % 4096, 'x')] = *selected;
            else if (mutation == 4 && selected->is_object())
                (*selected)["invalid_utf8"] = std::string("\xc0\x80\xff", 3);
            else if (mutation == 5)
                *selected = std::string(random() % (1024 * 1024), 's');
        } else if (mutation == 6) {
            document["filters"][0]["id"] = std::string(random() % 4096, 'a');
        } else if (mutation == 7) {
            document["filters"][0]["id"] = std::string(random() % 64, static_cast<char>(random()));
        } else if (mutation == 8) {
            Json *filter = &document["filters"][0];
            for (unsigned depth = 0; depth < 32; ++depth) {
                (*filter)["children"] = Json::array({Json{
                    {"id", "f" + std::to_string(depth)}, {"label", "deep"},
                    {"match", Json::object()}}});
                filter = &(*filter)["children"][0];
            }
        } else if (mutation == 9) {
            document["filters"][0]["children"] = document["filters"];
        } else if (mutation == 10) {
            project.write(std::string(1024 * 1024 + (random() % 3 - 1), ' '));
            const auto read = readGraphFilters(project.root);
            CHECK_FALSE(read.document);
            CHECK_FALSE(read.error.empty());
            continue;
        } else {
            project.write(document.dump() + std::string(1024 * 1024, ' '));
            const auto read = readGraphFilters(project.root);
            CHECK_FALSE(read.document);
            CHECK_FALSE(read.error.empty());
            continue;
        }
        const std::string text = document.dump(-1, ' ', false, Json::error_handler_t::replace);
        project.write(text);
        GraphFiltersRead read;
        REQUIRE_NOTHROW(read = readGraphFilters(project.root));
        CHECK(static_cast<bool>(read.document) == read.error.empty());
        if (read.document) {
            CHECK_NOTHROW(project.write(text));
            const auto again = readGraphFilters(project.root);
            REQUIRE(again.document);
            CHECK(*again.document == *read.document);
            GraphFilterFacts facts;
            facts.node_id = "node";
            facts.op = "canvas.text";
            facts.feature_name = "bloom";
            CHECK_NOTHROW(graphFilterMatches(read.document->filters.front(), *read.document, facts));
            CHECK_NOTHROW(findGraphFilter(*read.document, "canvas_text"));
        }
    }
}

} // namespace

TEST_CASE("strict graph filter reader fuzz", "[read-fuzz-short]") {
    filterProperties(fuzzCount(1000), seed0() ^ 0x474631);
}
TEST_CASE("strict graph filter reader long fuzz", "[read-fuzz-long][.]") {
    filterProperties(fuzzCount(1000000), seed0() ^ 0x474631);
}
