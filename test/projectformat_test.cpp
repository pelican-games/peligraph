#include "projectformat.hpp"
#include "projectpathresolver.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace Pelican {
namespace {

struct Sandbox {
    std::filesystem::path base;
    std::filesystem::path project;
    std::filesystem::path outside;

    Sandbox() {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        base = std::filesystem::temp_directory_path() /
               ("pelican_projectformat_" + suffix);
        project = base / "project";
        outside = base / "outside";
        std::filesystem::create_directories(project / "assets");
        std::filesystem::create_directories(outside);
        std::ofstream{project / "assets" / "inside.txt"} << "inside";
        std::ofstream{outside / "outside.txt"} << "outside";
    }

    ~Sandbox() {
        std::error_code error;
        std::filesystem::remove_all(base, error);
    }
};

} // namespace

TEST_CASE("pelican.project envelope validation is engine independent",
          "[project-format][wp248]") {
    const auto valid = nlohmann::json{
        {"schema", "pelican.project"},
        {"version", 1},
        {"name", "fixture"},
        {"engine_min_version", "0.1.0"},
        {"basic_config", {{"scene_data_json", "scene.json"}}},
    };
    const auto parsed = parseProjectEnvelopeJson(valid);
    REQUIRE(parsed.envelope.name == "fixture");
    REQUIRE(parsed.envelope.engine_min_version == "0.1.0");
    REQUIRE(parsed.envelope.basic_config.at("scene_data_json") ==
            "scene.json");
    REQUIRE(parsed.warnings.empty());

    auto invalid = valid;
    invalid["schema"] = "not.pelican";
    REQUIRE_THROWS_WITH(
        parseProjectEnvelopeJson(invalid),
        Catch::Matchers::ContainsSubstring("schema"));

    invalid = valid;
    invalid["version"] = 2;
    REQUIRE_THROWS_WITH(
        parseProjectEnvelopeJson(invalid),
        Catch::Matchers::ContainsSubstring("exactly 1"));

    invalid = valid;
    invalid["engine_min_version"] = "999.0.0";
    REQUIRE_THROWS_WITH(
        parseProjectEnvelopeJson(invalid),
        Catch::Matchers::ContainsSubstring("newer than this engine"));

    const auto ignored = parseProjectEnvelopeJson(
        invalid, {.ignore_engine_version = true});
    REQUIRE(ignored.warnings.size() == 1);
    REQUIRE_THAT(
        ignored.warnings.front(),
        Catch::Matchers::ContainsSubstring("--ignore-engine-version"));
}

// P-2b: a project says up front which build features it cannot run without.
// The name is read by the player and by every corpus that walks `projects/`,
// so the vocabulary and the two failures live in one place.
TEST_CASE("pelican.project requires names build features by a known vocabulary",
          "[project-format][p2b][requires][pg1]") {
    const auto base = nlohmann::json{
        {"schema", "pelican.project"},
        {"version", 1},
        {"name", "fixture"},
        {"basic_config", {{"scene_data_json", "scene.json"}}},
    };
    REQUIRE(parseProjectEnvelopeJson(base).envelope.requirements.empty());
    REQUIRE_FALSE(unsatisfiedProjectRequirement(base));

    // PG-1: one word per build module a project can depend on. `physics` is
    // the query service and collider world; there is no word for "a provider
    // is linked", and no word for a module nothing can depend on.
    REQUIRE(knownProjectRequirements() ==
            std::vector<std::string>{"animation", "audio", "physics", "xr", "vat",
                                     "seqplayer"});
    REQUIRE(std::string{projectRequirementOption("animation")} ==
            "PELICAN_WITH_ANIMATION");
    REQUIRE(std::string{projectRequirementOption("xr")} == "PELICAN_WITH_OPENXR");
    REQUIRE(projectRequirementOption("rpc").empty());
    auto unknown = base;
    unknown["requires"] = nlohmann::json::array({"rpc"});
    REQUIRE_THROWS_WITH(parseProjectRequirements(unknown),
        Catch::Matchers::ContainsSubstring("project.requires_unknown@1"));
    REQUIRE_THROWS_WITH(parseProjectEnvelopeJson(unknown),
        Catch::Matchers::ContainsSubstring("project.requires_unknown@1"));
    REQUIRE_THROWS_WITH(unsatisfiedProjectRequirement(unknown),
        Catch::Matchers::ContainsSubstring("project.requires_unknown@1"));

    auto malformed = base;
    malformed["requires"] = "animation";
    REQUIRE_THROWS_WITH(parseProjectEnvelopeJson(malformed),
        Catch::Matchers::ContainsSubstring("project.requires_unknown@1"));

    auto animation = base;
    animation["requires"] = nlohmann::json::array({"animation"});
    REQUIRE(parseProjectRequirements(animation) ==
            std::vector<std::string>{"animation"});
#if PELICAN_WITH_ANIMATION
    REQUIRE_FALSE(unsatisfiedProjectRequirement(animation));
    REQUIRE(parseProjectEnvelopeJson(animation).envelope.requirements ==
            std::vector<std::string>{"animation"});
#else
    // An OFF build says so with the name the animation module already uses
    // when a scene asks for it, rather than a third name for the same fact.
    REQUIRE(unsatisfiedProjectRequirement(animation) == "animation");
    REQUIRE_THROWS_WITH(parseProjectEnvelopeJson(animation),
        Catch::Matchers::ContainsSubstring("animation.module_unavailable@1"));
#endif
}

// PG-1: every word of the vocabulary is decided by the macro the engine's own
// code reads, and a build that left the module out refuses the project while
// reading it, under `<word>.module_unavailable@1`. The same table answers for
// a component's codec and a package's capabilities, so this is the one place
// the words are pinned to their options.
TEST_CASE("PG-1 every requires word opens or is refused by its module's name",
          "[project-format][requires][pg1]") {
    struct Row {
        const char *word;
        const char *option;
        bool available;
    };
    const Row rows[] = {
        {"animation", "PELICAN_WITH_ANIMATION", static_cast<bool>(PELICAN_WITH_ANIMATION)},
        {"audio", "PELICAN_WITH_AUDIO", static_cast<bool>(PELICAN_WITH_AUDIO)},
        {"physics", "PELICAN_WITH_PHYSICS", static_cast<bool>(PELICAN_WITH_PHYSICS)},
        {"xr", "PELICAN_WITH_OPENXR", static_cast<bool>(PELICAN_WITH_OPENXR)},
        {"vat", "PELICAN_WITH_VAT", static_cast<bool>(PELICAN_WITH_VAT)},
        {"seqplayer", "PELICAN_WITH_SEQPLAYER", static_cast<bool>(PELICAN_WITH_SEQPLAYER)},
    };
    REQUIRE(knownProjectRequirements().size() == std::size(rows));
    for (const auto &row : rows) {
        CAPTURE(row.word);
        const auto project = nlohmann::json{
            {"schema", "pelican.project"},
            {"version", 1},
            {"name", "fixture"},
            {"requires", nlohmann::json::array({row.word})},
            {"basic_config", {{"scene_data_json", "scene.json"}}},
        };
        const auto code = std::string{row.word} + ".module_unavailable@1";
        REQUIRE(std::string{projectRequirementOption(row.word)} == row.option);
        if (row.available) {
            REQUIRE(projectRequirementUnavailableCode(row.word).empty());
            REQUIRE_FALSE(unsatisfiedProjectRequirement(project));
            REQUIRE(parseProjectEnvelopeJson(project).envelope.requirements ==
                    std::vector<std::string>{row.word});
        } else {
            REQUIRE(std::string{projectRequirementUnavailableCode(row.word)} == code);
            REQUIRE(unsatisfiedProjectRequirement(project) == std::string{row.word});
            REQUIRE_THROWS_WITH(parseProjectEnvelopeJson(project),
                Catch::Matchers::ContainsSubstring(code) &&
                    Catch::Matchers::ContainsSubstring(std::string{row.option} + "=OFF"));
            std::cout << "PG1_REQUIRES word=" << row.word << " state=OFF code=" << code
                      << '\n';
        }
    }
    // A word outside the vocabulary has no unavailable code: it is not "off",
    // it is unknown, and reading it says so under its own name.
    REQUIRE(projectRequirementUnavailableCode("rpc").empty());
    REQUIRE(projectRequirementUnavailableCode("").empty());
}

TEST_CASE("ProjectPathResolver returns diagnostics without an engine logger",
          "[project-path][wp248]") {
    Sandbox sandbox;
    ProjectPathResolver resolver;
    resolver.setup(sandbox.project, true);

    const auto project_path = resolver.resolveProjectRef("assets/inside.txt");
    REQUIRE(project_path.warnings.empty());
    REQUIRE(std::get<std::filesystem::path>(project_path.reference) ==
            std::filesystem::weakly_canonical(
                sandbox.project / "assets" / "inside.txt"));
    REQUIRE(resolver.loadText("assets/inside.txt") == "inside");

    const auto cli_path = resolver.resolveCliRef(
        std::filesystem::weakly_canonical(sandbox.outside / "outside.txt")
            .generic_string());
    REQUIRE(cli_path.warnings.size() == 1);
    REQUIRE_THAT(
        cli_path.warnings.front(),
        Catch::Matchers::ContainsSubstring(
            "absolute CLI path reference accepted"));
    REQUIRE(std::get<std::filesystem::path>(cli_path.reference) ==
            std::filesystem::weakly_canonical(
                sandbox.outside / "outside.txt"));
}

} // namespace Pelican
