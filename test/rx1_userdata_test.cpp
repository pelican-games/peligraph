#include <fstream>
#include <chrono>
#include "../src/core/log.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/model/animationcomponentvalidation.hpp"
#include "../src/core/renderer/shadowvalidation.hpp"
#include "../src/core/ui/module.hpp"
#include "../src/core/userpublic/valuegraph.hpp"
#include "../src/core/watch/userdataguard.hpp"
#include "../src/core/loader/controllervalidation.hpp"
#include "../src/core/appflow/enginetime.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>
using namespace Pelican;

TEST_CASE("RX-1 marked errors skip one scope until repaired or removed", "[rx1][problems][ecs]") {
    FastModuleContainer lifetime;
    UserDataScope broken{"test/sprite/1", "missing", {{"object", 1}, {"component", "sprite_view"}}};
    UserDataScope healthy{"test/sprite/2", "valid"};
    int attempts = 0, draws = 0;
    const auto frame = [&] {
        guardUserData(broken, [&] { ++attempts; throw UserDataError("sprite asset declaration not found: missing"); });
        guardUserData(healthy, [&] { ++draws; });
    };
    frame();
    const auto before = GET_MODULE(ProblemList).problemsJson();
    frame(); frame();
    CHECK(attempts == 1);
    CHECK(draws == 3);
    CHECK(GET_MODULE(ProblemList).problemsJson() == before);
    REQUIRE(before.at("problems").size() == 1);
    CHECK(before.at("problems")[0].at("code") == "runtime.user_data@1");
    broken.version = "repaired";
    REQUIRE(guardUserData(broken, [&] { ++draws; }));
    CHECK(GET_MODULE(ProblemList).problemsJson().at("problems").empty());
    frame();
    retainUserDataScopes("test/", {healthy.key});
    CHECK(GET_MODULE(ProblemList).problemsJson().at("problems").empty());
}
TEST_CASE("RX-1 unmarked engine exceptions cross the guard unchanged", "[rx1][problems][ecs]") {
    FastModuleContainer lifetime;
    UserDataScope scope{"test/engine", "1"};
    CHECK_THROWS_AS(guardUserData(scope, [] { throw std::logic_error("engine invariant"); }), std::logic_error);
    CHECK_THROWS_AS(guardUserData(scope, [] { throw std::runtime_error("Vulkan failure"); }), std::runtime_error);
    CHECK(GET_MODULE(ProblemList).problemsJson().at("problems").empty());
}
TEST_CASE("RX-1 game code reports the complete behavior exception and requests Stop", "[rx1][behavior][problems]") {
    FastModuleContainer lifetime;
    (void)GET_MODULE(EngineTime);
    UserDataScope scope{"behavior/Thrower", "1", {{"behavior", "behavior Thrower"}}};
    CHECK_FALSE(guardUserCode(scope, [] { throw std::runtime_error("first line\nsecond line"); }));
    CHECK(userCodeStopRequested());
    const auto problems = GET_MODULE(ProblemList).problemsJson().at("problems");
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].at("message") == "behavior Thrower: first line\nsecond line");
    CHECK_FALSE(guardUserCode(scope, [] { FAIL("failed code must not be called again"); }));
}
TEST_CASE("RX-1 camera target and controller actions are checked before evaluation", "[rx1][camera]") {
    CHECK_THROWS_AS(requireControllerTarget("Camera", "Gone", false), UserDataError);
    CHECK_NOTHROW(requireControllerTarget("Camera", "Target", true));
    InputActionMap empty;
    CHECK_THROWS_AS(requireControllerActions(&empty, true, false), UserDataError);
    CHECK_NOTHROW(requireControllerActions(&empty, false, false));
}

TEST_CASE("RX-1 shadow range admission rejects a near plane beyond the cascade distance", "[rx1][shadow]") {
    CHECK_THROWS_AS(requireShadowDistance(10.0f, 10.0f), UserDataError);
    CHECK_THROWS_AS(requireShadowDistance(11.0f, 10.0f), UserDataError);
    CHECK_NOTHROW(requireShadowDistance(0.1f, 10.0f));
}
TEST_CASE("RX-1 UI document admission names a missing game event", "[rx1][ui][problems]") {
    FastModuleContainer lifetime;
    const auto document = nlohmann::json::parse(R"({"schema":"pelican.ui","version":1,"key":"hud",
      "root":{"id":"root","type":"button","emit":{"on_click":{"event":"MissingGameEvent","fields":[]}}}})");
    UserDataScope scope{"test/ui", "1"};
    CHECK_FALSE(guardUserData(scope, [&] { ui::UiModule module{document}; }));
    const auto problems = GET_MODULE(ProblemList).problemsJson().at("problems");
    REQUIRE(problems.size() == 1);
    CHECK_THAT(problems[0].at("message").get<std::string>(), Catch::Matchers::ContainsSubstring("MissingGameEvent"));
}
#if PELICAN_WITH_AUDIO
TEST_CASE("RX-1 audio constants are refused during graph admission", "[rx1][valuegraph][audio]") {
    auto document = nlohmann::json::parse(R"({"schema":"pelican.graph","version":1,"nodes":[
      {"id":"value","op":"value.constant","parameters":{"value":1},"outputs":["value"]},
      {"id":"sink","op":"audio.volume","parameters":{"target":"voice"},
       "inputs":{"value":{"node":"value","port":"value"}},"outputs":[]}],"outputs":{}})");
    CHECK(ValueGraph::Graph{document.dump()}.requiresSoundBindings());
    document["nodes"][0]["parameters"]["value"] = -1;
    CHECK_THROWS_AS(ValueGraph::Graph{document.dump()}, UserDataError);
    document["nodes"][1]["op"] = "audio.pitch";
    document["nodes"][0]["parameters"]["value"] = 0;
    CHECK_THROWS_AS(ValueGraph::Graph{document.dump()}, UserDataError);
    document["nodes"][0]["parameters"]["value"] = 1;
    CHECK_NOTHROW(ValueGraph::Graph{document.dump()});
}
#endif

#if PELICAN_WITH_ANIMATION
TEST_CASE("RX-1 animation graph admission rejects malformed documents before binding", "[rx1][animation]") {
    CHECK_THROWS_AS(parseAnimationComponentGraph("{}"), UserDataError);
    CHECK_THROWS_AS(parseAnimationComponentGraph("not json"), UserDataError);
    PathResolver resolver;
    AnimationComponent animation;
    animation.graph = "missing.json";
    CHECK_THROWS_AS(requireAnimationComponentGraph(nullptr, animation, resolver), UserDataError);
}
#endif

TEST_CASE("RX-1 scene name admission refuses an unknown destination without scheduling", "[rx1][scene]") {
    setupLogger(true);
    const auto root = std::filesystem::temp_directory_path() /
        ("rx1_scene_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "passes");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    std::ofstream{root / "scene.json"} << R"({"schema":"pelican.scene","version":2,"scenes":{"main":{"objects":[]}}})";
    std::ofstream{root / "passes/main_rendering_config.json"} << R"({"schema":"pelican.graph","version":1,"nodes":[],"outputs":{},"rendering":{}})";
    FastModuleContainer lifetime;
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setProjectData(R"({"schema":"pelican.project","version":1,"name":"RX1", "engine_min_version":"0.1.0",
        "basic_config":{"default_scene_id":"main","scene_data_json":"scene.json"}})");
    auto &loader = GET_MODULE(SceneLoader);
    CHECK_THROWS_AS(loader.requestLoad("absent"), UserDataError);
    CHECK_FALSE(loader.pendingSceneForTesting());
    CHECK_NOTHROW(loader.requestLoad("main"));
    CHECK(loader.pendingSceneForTesting() == "main");
}

TEST_CASE("RX-1 UI file repair clears Problems without creating late modules", "[rx1][ui][problems]") {
    setupLogger(true);
    const auto root = std::filesystem::temp_directory_path() /
        ("rx1_ui_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    const auto file = root / "ui.json";
    std::ofstream{file} << R"({"schema":"pelican.ui","version":1,"key":"hud",
      "root":{"id":"root","type":"button","emit":{"on_click":{"event":"MissingGameEvent","fields":[]}}}})";
    FastModuleContainer lifetime;
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setProjectData(R"({"schema":"pelican.project","version":1,"name":"RX1", "engine_min_version":"0.1.0",
        "basic_config":{"ui_config_json":"ui.json"}})");
    auto &module = GET_MODULE(ui::UiModule);
    const auto broken = GET_MODULE(ProblemList).problemsJson();
    REQUIRE(broken.at("problems").size() == 1);
    CHECK(module.buildFrame({160,90}).quads.empty());
    module.reloadAuthoredDocument();
    CHECK(GET_MODULE(ProblemList).problemsJson() == broken);
    FastModuleContainer::freezeCreation();
    std::ofstream{file} << R"({"schema":"pelican.ui","version":1,"key":"hud",
      "root":{"id":"root","type":"panel","color":[255,255,255,255],
        "layout":{"x":{"mode":"fixed","value":10},"y":{"mode":"fixed","value":10}}}})";
    std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + std::chrono::seconds{1});
    module.reloadAuthoredDocument();
    CHECK(GET_MODULE(ProblemList).problemsJson().at("problems").empty());
    CHECK(module.buildFrame({160,90}).quads.size() == 1);
}
