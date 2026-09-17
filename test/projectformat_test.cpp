#include "projectformat.hpp"
#include "projectpathresolver.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
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
          "[project-format][p2b][requires]") {
    const auto base = nlohmann::json{
        {"schema", "pelican.project"},
        {"version", 1},
        {"name", "fixture"},
        {"basic_config", {{"scene_data_json", "scene.json"}}},
    };
    REQUIRE(parseProjectEnvelopeJson(base).envelope.requirements.empty());
    REQUIRE_FALSE(unsatisfiedProjectRequirement(base));

    // The vocabulary is one name in v0. `physics` is a real build option, and
    // still not a requirement name: the module namespace is X-1's decision,
    // and a parser that guessed would settle it here.
    REQUIRE(knownProjectRequirements() == std::vector<std::string>{"animation"});
    REQUIRE(std::string{projectRequirementOption("animation")} ==
            "PELICAN_WITH_ANIMATION");
    auto unknown = base;
    unknown["requires"] = nlohmann::json::array({"physics"});
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
