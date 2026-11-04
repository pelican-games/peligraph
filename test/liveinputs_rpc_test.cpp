#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/osc/osclistener.hpp"
#include "../src/core/osc/osclistenerconfig.hpp"
#include "../src/core/userpublic/userinput.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <bit>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {
namespace {

using Json = nlohmann::json;
using Catch::Matchers::ContainsSubstring;

std::string readFile(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void writeFile(const std::filesystem::path &path, const std::string &text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file << text;
}

struct TempDir {
    std::filesystem::path root;
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

// posegraph_demo, with its animation clock driven by a declared live input
// instead of scene_time, so the published pose is a visible function of the
// frozen snapshot.
std::filesystem::path copyLiveProject(const TempDir &dir) {
    const auto shipped = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" / "posegraph_demo";
    const auto root = dir.root / "live_posegraph_demo";
    std::filesystem::create_directories(root);
    std::filesystem::copy(shipped, root,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing);
    std::filesystem::create_directories(root / "shaders");

    auto project = Json::parse(readFile(root / "project.json"));
    project["basic_config"]["live_inputs_json"] = "live/inputs.json";
    project["basic_config"]["input_actions_json"] = "live/actions.json";
    project["basic_config"]["input_profiles"] = Json{{"live", "live/profile.json"}};
    project["basic_config"]["input_profile"] = "live";
    writeFile(root / "project.json", project.dump(2));

    writeFile(root / "live" / "actions.json", Json{
        {"schema", "pelican.input_actions"},
        {"version", 1},
        {"action_sets", Json::array()},
    }.dump(2));
    writeFile(root / "live" / "profile.json", Json{
        {"schema", "pelican.input_profile"},
        {"version", 1},
        {"name", "live"},
        {"bindings", Json::array()},
        {"live", Json::array({
            {{"address", "/phase"}, {"input", "phase"}},
        })},
    }.dump(2));

    writeFile(root / "live" / "inputs.json", Json{
        {"schema", "pelican.live_inputs"},
        {"version", 1},
        {"inputs", Json::array({
            {{"name", "phase"}, {"type", "float"}, {"default", 0.0}, {"min", 0.0}, {"max", 4.0}},
            {{"name", "steps"}, {"type", "int"}, {"default", 1}, {"min", 0}, {"max", 8}},
            {{"name", "enabled"}, {"type", "bool"}, {"default", false}},
        })},
    }.dump(2));

    auto graph = Json::parse(readFile(root / "passes" / "main.json"));
    // Both the definition's clip clock and the top-level blend clock read the
    // same live value, so the published pose is a direct sample of it.
    for (auto &[name, definition] : graph["definitions"].items()) {
        (void)name;
        for (auto &node : definition["nodes"])
            if (node["id"] == "clock")
                node["parameters"]["name"] = "live:phase";
    }
    for (auto &node : graph["nodes"])
        if (node["id"] == "clock")
            node["parameters"]["name"] = "live:phase";
    writeFile(root / "passes" / "main.json", graph.dump(2));
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
    TestSupport::requireVulkanDevice("OSC-1a live inputs over RPC");
    GET_MODULE(ECSPredefinedRegistration).reg();
}

struct Rpc {
    std::istringstream input;
    std::ostringstream output;
    EngineRpcEndpoint endpoint{input, output};
    std::uint64_t next_id = 9300;

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
    Json error(std::string_view method, Json params = Json::object()) {
        const auto response = call(method, std::move(params));
        INFO(response.dump(2));
        REQUIRE(response.contains("error"));
        return response.at("error");
    }
};

Json inputNamed(const Json &snapshot, std::string_view name) {
    for (const auto &input : snapshot.at("inputs"))
        if (input.at("name").get<std::string>() == name) return input;
    FAIL("no live input " << name);
    return {};
}

const Json &firstSink(const Json &status) {
    const auto &animation = status.at("animation");
    REQUIRE(animation.at("available") == true);
    REQUIRE_FALSE(animation.at("sinks").empty());
    return animation.at("sinks").at(0);
}

#if defined(_WIN32)
struct WinsockScope {
    WinsockScope() {
        WSADATA data{};
        ok = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockScope() {
        if (ok) ::WSACleanup();
    }
    bool ok = false;
};
#endif

bool sendOscPhase(std::uint16_t port, float value) {
#if defined(_WIN32)
    static WinsockScope winsock;
    if (!winsock.ok) return false;
#endif
#if defined(_WIN32)
    const auto socket_handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == INVALID_SOCKET) return false;
#else
    const int socket_handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle < 0) return false;
#endif

    std::vector<std::uint8_t> bytes{'/', 'p', 'h', 'a', 's', 'e', 0, 0,
                                    ',', 'f', 0, 0};
    const auto bits = std::bit_cast<std::uint32_t>(value);
    bytes.push_back(static_cast<std::uint8_t>(bits >> 24));
    bytes.push_back(static_cast<std::uint8_t>(bits >> 16));
    bytes.push_back(static_cast<std::uint8_t>(bits >> 8));
    bytes.push_back(static_cast<std::uint8_t>(bits));

    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(port);
    destination.sin_addr.s_addr = htonl(0x7f000001u);
#if defined(_WIN32)
    const int sent =
        ::sendto(socket_handle, reinterpret_cast<const char *>(bytes.data()),
                 static_cast<int>(bytes.size()), 0,
                 reinterpret_cast<const sockaddr *>(&destination),
                 sizeof(destination));
    ::closesocket(socket_handle);
#else
    const auto sent =
        ::sendto(socket_handle, bytes.data(), bytes.size(), 0,
                 reinterpret_cast<const sockaddr *>(&destination),
                 sizeof(destination));
    ::close(socket_handle);
#endif
    return sent >= 0 && static_cast<std::size_t>(sent) == bytes.size();
}

} // namespace

TEST_CASE("OSC-1a liveinputs RPC stages a typed live value, freezes it next frame, and publishes it",
          "[osc1a][liveinputs][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
                      ("pelican_liveinputs_rpc_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    const auto root = copyLiveProject(dir);

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("render_frame");

    // The project declaration is the source, with declared defaults and types.
    auto snapshot = rpc.result("get_live_inputs");
    REQUIRE(snapshot.at("dropped_during_replay") == 0);
    auto phase = inputNamed(snapshot, "phase");
    REQUIRE(phase.at("type") == "float");
    REQUIRE(phase.at("value") == Catch::Approx(0.0));
    REQUIRE(phase.at("default") == Catch::Approx(0.0));
    REQUIRE(phase.at("source") == "default");
    REQUIRE(phase.at("changed_frame") == 0);
    REQUIRE(inputNamed(snapshot, "steps").at("type") == "int");
    REQUIRE(inputNamed(snapshot, "steps").at("value") == 1);
    REQUIRE(inputNamed(snapshot, "enabled").at("type") == "bool");
    REQUIRE(inputNamed(snapshot, "enabled").at("value") == false);

    // Freeze a stable baseline: a paused frame whose game time does not move.
    REQUIRE(rpc.result("set_play_state", {{"state", "paused"}}) ==
            Json{{"state", "paused"}, {"previous", "playing"}});
    const auto baseline = rpc.result("step_frame");
    const auto baseline_stats = rpc.result("get_status");
    const auto baseline_hash = firstSink(baseline_stats).at("pose_hash");
    const auto baseline_frame = baseline.at("frame").get<std::uint64_t>();

    // Staged values are not visible until the next frame boundary.
    REQUIRE(rpc.result("set_live_input", {{"values", {{"phase", 0.25}}}}) ==
            Json{{"accepted", 1}, {"applies_at_frame", baseline_frame + 1}});
    phase = inputNamed(rpc.result("get_live_inputs"), "phase");
    REQUIRE(phase.at("value") == Catch::Approx(0.0));
    REQUIRE(phase.at("source") == "default");

    // The next drawn frame freezes it and the graph's published pose follows.
    const auto changed = rpc.result("step_frame");
    REQUIRE(changed.at("frame").get<std::uint64_t>() == baseline_frame + 1);
    snapshot = rpc.result("get_live_inputs");
    phase = inputNamed(snapshot, "phase");
    REQUIRE(phase.at("value") == Catch::Approx(0.25));
    REQUIRE(phase.at("source") == "rpc");
    REQUIRE(phase.at("changed_frame").get<std::uint64_t>() == baseline_frame + 1);
    const auto changed_stats = rpc.result("get_status");
    REQUIRE(changed_stats.at("frame") == changed.at("frame"));
    REQUIRE(firstSink(changed_stats).at("pose_hash") != baseline_hash);

    // Re-sending the same value changes neither the value nor its changed
    // frame; the pose is the same paused pose, not an advancing one.
    REQUIRE(rpc.result("set_live_input", {{"values", {{"phase", 0.25}}}}).at("accepted") == 1);
    const auto resend = rpc.result("step_frame");
    phase = inputNamed(rpc.result("get_live_inputs"), "phase");
    REQUIRE(phase.at("changed_frame").get<std::uint64_t>() == baseline_frame + 1);
    const auto resend_stats = rpc.result("get_status");
    REQUIRE(resend_stats.at("frame") == resend.at("frame"));
    REQUIRE(firstSink(resend_stats).at("pose_hash") == firstSink(changed_stats).at("pose_hash"));

    // Every value is validated before any commits; a bad name leaves the
    // earlier accepted values untouched.
    const auto before_bad = inputNamed(rpc.result("get_live_inputs"), "phase");
    auto bad = rpc.error("set_live_input", {{"values", {{"phase", 0.1}, {"missing", 1}}}});
    REQUIRE_THAT(bad.at("message").get<std::string>(), ContainsSubstring("live_input.unknown@1"));
    REQUIRE(inputNamed(rpc.result("get_live_inputs"), "phase") == before_bad);
    bad = rpc.error("set_live_input", {{"values", {{"steps", 1.5}}}});
    REQUIRE_THAT(bad.at("message").get<std::string>(), ContainsSubstring("live_input.type@1"));
    bad = rpc.error("set_live_input", {{"values", {{"enabled", 1}}}});
    REQUIRE_THAT(bad.at("message").get<std::string>(), ContainsSubstring("live_input.type@1"));

    // Integers and booleans keep their declared JSON types; a declared bound
    // clamps and marks the value.
    REQUIRE(rpc.result("set_live_input",
                       {{"values", {{"steps", 99}, {"enabled", true}}}}).at("accepted") == 2);
    (void)rpc.result("step_frame");
    snapshot = rpc.result("get_live_inputs");
    const auto steps = inputNamed(snapshot, "steps");
    REQUIRE(steps.at("value").is_number_integer());
    REQUIRE(steps.at("value") == 8);
    REQUIRE(steps.at("clamped") == true);
    const auto enabled = inputNamed(snapshot, "enabled");
    REQUIRE(enabled.at("value").is_boolean());
    REQUIRE(enabled.at("value") == true);

    // A project whose published output reads live:phase cannot start a replay
    // that does not record it.
    const auto replay = rpc.error("start_input_replay",
                                  {{"path", (dir.root / "recording.pelican").generic_string()}});
    REQUIRE_THAT(replay.at("message").get<std::string>(),
                 ContainsSubstring("offline.live_input_unrecorded@1"));
}

TEST_CASE("OSC-2b a headless player listens on the option-configured UDP port and freezes packets",
          "[osc2b][osc][liveinputs][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
                      ("pelican_osc2b_rpc_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    const auto root = copyLiveProject(dir);

    FastModuleContainer modules;
    configureHeadless(root);

    // Project declarations must exist before input profiles are parsed: the
    // live binding names an input and is validated against the declaration.
    internal::prepareInputActionsRuntime();
    internal::prepareOscListener();
    auto &listener = GET_MODULE(OscListener);
    REQUIRE(listener.statusJson().at("state").get<std::string>() == "off");
    REQUIRE_FALSE(listener.socketOpen());

    // Exactly what --osc-port 0 (with the default interface and no filter)
    // hands the engine after option parsing.
    const auto config =
        parseOscListenerConfig("0", "127.0.0.1", std::nullopt);
    REQUIRE(config.allow.empty());
    GET_MODULE(EngineLaunchConfig).osc_listener = config;
    internal::prepareOscListener();
    REQUIRE(listener.statusJson().at("state").get<std::string>() ==
            "listening");
    const auto port = listener.statusJson().at("port").get<std::uint16_t>();
    REQUIRE(port != 0);

    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("render_frame");

    auto snapshot = rpc.result("get_live_inputs");
    REQUIRE(snapshot.at("osc").at("listener").at("state").get<std::string>() ==
            "listening");
    REQUIRE(snapshot.at("osc").at("listener").at("port").get<std::uint16_t>() ==
            port);
    REQUIRE(inputNamed(snapshot, "phase").at("value") == Catch::Approx(0.0));

    REQUIRE(sendOscPhase(port, 0.625f));

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{30};
    double phase = 0.0;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)rpc.result("step_frame");
        snapshot = rpc.result("get_live_inputs");
        phase = inputNamed(snapshot, "phase").at("value").get<double>();
        if (phase == Catch::Approx(0.625)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    REQUIRE(phase == Catch::Approx(0.625));
    REQUIRE(inputNamed(snapshot, "phase").at("source").get<std::string>() ==
            "osc");
    REQUIRE(snapshot.at("osc").at("received").get<std::uint64_t>() >= 1);
    REQUIRE(snapshot.at("osc").at("listener").at("sender_not_allowed")
                .get<std::uint64_t>() == 0);
}

} // namespace Pelican
