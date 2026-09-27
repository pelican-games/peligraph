#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

// PM-1a. playing <-> paused, advance_frames and the evaluation index, over the
// headless RPC the way a tool drives them. The animated scene is the shipped
// posegraph_demo: a pose graph poses the hero on every frame, so a paused
// frame that did not evaluate again, or evaluated at a moving time, shows up
// in the animation section of get_status.

namespace Pelican {
namespace {

using Json = nlohmann::json;

std::string readFile(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

struct TempDir {
    std::filesystem::path root;
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

std::filesystem::path copyPosegraphDemo(const TempDir &dir) {
    const auto shipped = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" / "posegraph_demo";
    const auto root = dir.root / "posegraph_demo";
    std::filesystem::create_directories(root);
    std::filesystem::copy(shipped, root,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing);
    std::filesystem::create_directories(root / "shaders");
    return root;
}

void configureHeadless(const std::filesystem::path &root) {
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setProjectData(readFile(root / "project.json"));
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.rpc = true;
    launch.headless_extent = vk::Extent2D{64, 64};
    launch.shader_hot_reload = false;
    GET_MODULE(EngineTime).setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    TestSupport::requireVulkanDevice("PM-1a play state over RPC");
    GET_MODULE(ECSPredefinedRegistration).reg();
}

struct Rpc {
    std::istringstream input;
    std::ostringstream output;
    EngineRpcEndpoint endpoint{input, output};
    std::uint64_t next_id = 9100;

    Json call(std::string_view method, Json params = Json::object()) {
        return Json::parse(endpoint.processLine(
            Json{{"jsonrpc", "2.0"}, {"id", ++next_id}, {"method", method}, {"params", std::move(params)}}
                .dump()));
    }
    Json result(std::string_view method, Json params = Json::object()) {
        const auto response = call(method, std::move(params));
        INFO(response.dump(2));
        REQUIRE(response.contains("result"));
        return response.at("result");
    }
    // The error a call answers with; `code` is data.code, or empty when the
    // error carries none.
    Json error(std::string_view method, Json params = Json::object()) {
        const auto response = call(method, std::move(params));
        INFO(response.dump(2));
        REQUIRE(response.contains("error"));
        return response.at("error");
    }
};

std::string dataCode(const Json &error) {
    if (!error.contains("data") || !error.at("data").is_object() || !error.at("data").contains("code"))
        return {};
    return error.at("data").at("code").get<std::string>();
}

const Json &firstSink(const Json &status) {
    const auto &animation = status.at("animation");
    REQUIRE(animation.at("available") == true);
    REQUIRE_FALSE(animation.at("sinks").empty());
    return animation.at("sinks").at(0);
}

} // namespace

// PM-1a acceptance 1 and 2 (and the headless half of 5 and 7).
TEST_CASE("PM-1a paused, headless frames evaluate the animated scene again and advance_frames steps the game",
          "[pm1a][play-state][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
                      ("pelican_pm1a_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    const auto root = copyPosegraphDemo(dir);

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    // One draw before the first step. Without it, the first step_frame of
    // this in-process endpoint refuses with "pose scalar input was not
    // evaluated this frame"; the player's own headless frames do not.
    (void)rpc.result("render_frame");

    // The default launch plays: the evaluation index and the game frame are
    // one number, and a step moves the game clock by the fixed step.
    for (int frame = 0; frame < 3; ++frame) (void)rpc.result("step_frame");
    const auto playing = rpc.result("get_status");
    INFO(playing.at("play").dump());
    REQUIRE(playing.at("play").at("state") == "playing");
    REQUIRE(playing.at("play").at("pending_steps") == 0);
    REQUIRE(playing.at("frame") == playing.at("play").at("game_frame"));
    REQUIRE(playing.at("play").at("game_time").get<double>() == Catch::Approx(3.0 / 60.0));
    REQUIRE(playing.at("time") == playing.at("play").at("game_time"));
    REQUIRE(firstSink(playing).at("frame_revision") == playing.at("frame"));

    // Refused while playing: steps are a paused game's.
    const auto early = rpc.error("advance_frames", {{"count", 1}});
    REQUIRE(dataCode(early) == "play_state_mismatch");
    REQUIRE(early.at("data").at("state") == "playing");

    REQUIRE(rpc.result("set_play_state", {{"state", "paused"}}) ==
            Json{{"state", "paused"}, {"previous", "playing"}});
    // The same state again is a success that changes nothing.
    REQUIRE(rpc.result("set_play_state", {{"state", "paused"}}) ==
            Json{{"state", "paused"}, {"previous", "paused"}});

    const auto before = rpc.result("get_status");
    const auto before_frame = before.at("frame").get<std::uint64_t>();
    const auto game_frame = before.at("play").at("game_frame").get<std::uint64_t>();
    const auto game_time = before.at("play").at("game_time").get<double>();
    const auto pose_hash = firstSink(before).at("pose_hash");
    for (std::uint64_t frame = 1; frame <= 3; ++frame) {
        const auto stepped = rpc.result("step_frame");
        REQUIRE(stepped.at("frame").get<std::uint64_t>() == before_frame + frame);
        REQUIRE(stepped.at("t").get<double>() == game_time);
    }
    const auto paused = rpc.result("get_status");
    INFO(paused.at("play").dump());
    REQUIRE(paused.at("play").at("state") == "paused");
    REQUIRE(paused.at("play").at("game_frame").get<std::uint64_t>() == game_frame);
    REQUIRE(paused.at("play").at("game_time").get<double>() == game_time);
    REQUIRE(paused.at("frame").get<std::uint64_t>() == before_frame + 3);
    // Evaluated again on the paused frame, at the stopped time: the sink was
    // written by this frame's evaluation, and the pose is the one it was.
    REQUIRE(firstSink(paused).at("frame_revision") == paused.at("frame"));
    REQUIRE(firstSink(paused).at("pose_hash") == pose_hash);

    // Three steps of 1/fps, taken before the answer; still paused after.
    const auto advanced = rpc.result("advance_frames", {{"count", 3}});
    REQUIRE(advanced.at("scheduled") == 3);
    REQUIRE(advanced.at("until_game_frame").get<std::uint64_t>() == game_frame + 3);
    REQUIRE(advanced.at("frame").get<std::uint64_t>() == before_frame + 6);
    const auto after = rpc.result("get_status");
    REQUIRE(after.at("play").at("state") == "paused");
    REQUIRE(after.at("play").at("game_frame").get<std::uint64_t>() == game_frame + 3);
    REQUIRE(after.at("play").at("game_time").get<double>() == Catch::Approx(game_time + 3.0 / 60.0));
    REQUIRE(after.at("play").at("pending_steps") == 0);
    REQUIRE(firstSink(after).at("frame_revision") == after.at("frame"));

    // The count is 1 to 600; anything else is an argument error with no code.
    for (const auto count : {0, 601}) {
        const auto out_of_range = rpc.error("advance_frames", {{"count", count}});
        REQUIRE(out_of_range.at("code") == -32602);
        REQUIRE(dataCode(out_of_range).empty());
    }
    // This build has no edit state.
    const auto edit = rpc.error("set_play_state", {{"state", "edit"}});
    REQUIRE(dataCode(edit) == "play_state_mismatch");
    REQUIRE(edit.at("data").at("state") == "paused");
    REQUIRE(edit.at("data").at("requested") == "edit");
    REQUIRE(rpc.error("set_play_state", {{"state", "stopped"}}).at("code") == -32602);

    // Playing again: every frame is a game step once more.
    REQUIRE(rpc.result("set_play_state", {{"state", "playing"}}) ==
            Json{{"state", "playing"}, {"previous", "paused"}});
    const auto resumed = rpc.result("step_frame");
    REQUIRE(resumed.at("frame").get<std::uint64_t>() == before_frame + 7);
    const auto running = rpc.result("get_status");
    REQUIRE(running.at("play").at("game_frame").get<std::uint64_t>() == game_frame + 4);
    REQUIRE(running.at("play").at("game_time").get<double>() == Catch::Approx(game_time + 4.0 / 60.0));
    REQUIRE(firstSink(running).at("frame_revision") == running.at("frame"));
}

} // namespace Pelican
