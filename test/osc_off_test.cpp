#include "../src/core/container.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/userpublic/userinput.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <nlohmann/json.hpp>
#include <optional>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace Pelican;
using Catch::Matchers::StartsWith;

namespace {

void writeText(const std::filesystem::path &path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios_base::binary};
    file << text;
}

void ensureLogger() {
    static const bool initialized = [] {
        setupLogger();
        return true;
    }();
    (void)initialized;
}

struct Sandbox {
    std::filesystem::path base;
    std::filesystem::path root;

    Sandbox() {
        const auto suffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        base = std::filesystem::temp_directory_path() /
               ("pelican_osc2a_off_" + suffix);
        root = base / "project";
    }

    ~Sandbox() {
        std::error_code error;
        std::filesystem::remove_all(base, error);
    }
};

nlohmann::json projectWith(const std::filesystem::path &sandbox,
                           std::string_view profile_rel) {
    return {
        {"schema", "pelican.project"},
        {"version", 1},
        {"name", "osc-off-profile"},
        {"engine_min_version", "0.1.0"},
        {"basic_config",
         {{"input_actions_json", "input/actions.json"},
          {"input_profiles", {{"main", std::string{profile_rel}}}},
          {"input_profile", "main"}}},
    };
}

} // namespace

TEST_CASE("OSC-2a OFF refuses any live key through the real project profile load",
          "[osc2a][osc][off]") {
    ensureLogger();
    Sandbox sandbox;
    writeText(sandbox.root / "input" / "actions.json",
              R"({"schema":"pelican.input_actions","version":1,"action_sets":[{"name":"gameplay","actions":[{"name":"jump","type":"button"}]}]})");
    writeText(sandbox.root / "input" / "profiles" / "live.json",
              R"({"schema":"pelican.input_profile","version":1,"name":"main","bindings":[],"live":[]})");
    const auto project = projectWith(sandbox.root, "input/profiles/live.json");
    const auto bytes = project.dump();

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(ProjectSource).setProjectData(bytes);
    REQUIRE_THROWS_WITH(Actions::isConfigured(),
                        StartsWith("osc.module_unavailable@1:"));
}

TEST_CASE("OSC-2a OFF still loads a project profile without a live key",
          "[osc2a][osc][off]") {
    ensureLogger();
    Sandbox sandbox;
    writeText(sandbox.root / "input" / "actions.json",
              R"({"schema":"pelican.input_actions","version":1,"action_sets":[{"name":"gameplay","actions":[{"name":"jump","type":"button"}]}]})");
    writeText(sandbox.root / "input" / "profiles" / "plain.json",
              R"({"schema":"pelican.input_profile","version":1,"name":"main","bindings":[]})");
    const auto project = projectWith(sandbox.root, "input/profiles/plain.json");
    const auto bytes = project.dump();

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(ProjectSource).setProjectData(bytes);
    REQUIRE(Actions::isConfigured());
    REQUIRE(internal::activeInputProfile() ==
            std::optional<std::string>{"main"});
}
