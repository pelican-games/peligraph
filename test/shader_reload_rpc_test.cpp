#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/watch/reloadgate.hpp"
#include "../src/core/watch/reloadservice.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stb_image.h>
#include <string>

// F-5. reload_shaders: a client that drives the engine over RPC asks for the
// shaders to be recompiled, because the file watch is shut under an RPC
// driver and stays shut. The request passes that gate and no other: replay
// and strict assets still refuse it.

namespace Pelican {
namespace {

using Json = nlohmann::json;

void writeFile(const std::filesystem::path &path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file << contents;
}

struct TempProject {
    std::filesystem::path root;
    ~TempProject() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

std::string fragment(std::string_view body) {
    return "#version 450\n"
           "layout(location=0) out vec4 outColor;\n"
           "void main(){ " +
           std::string{body} + " }\n";
}

} // namespace

TEST_CASE("F-5 reload_shaders recompiles what changed under an RPC driver, "
          "and only the determinism gates refuse it",
          "[f5][rpc][shader-reload][gpu]") {
    setupLogger(true);
    FastModuleContainer modules;
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const TempProject project_dir{
        std::filesystem::temp_directory_path() /
        ("pelican_f5_reload_shaders_" + std::to_string(suffix))};
    const auto &root = project_dir.root;
    const auto shader = root / "shaders" / "tone.frag";
    const auto capture = root / "capture.png";

    writeFile(root / "scene.json", R"json({
  "schema":"pelican.scene","version":2,
  "scenes":{"default_scene":{"objects":[]}}
})json");
    writeFile(root / "assets.json",
              R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
    writeFile(root / "ui/ui.json",
              R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
    writeFile(root / "shaders/fullscreen.vert", R"glsl(
#version 450
layout(location=0) out vec2 uv;
void main(){ uv=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2); gl_Position=vec4(uv*2.0-1.0,0,1); }
)glsl");
    writeFile(shader, fragment("outColor=vec4(0.5,0.5,0.5,1.0);"));
    writeFile(root / "passes/main.json", R"json({
  "schema": "pelican.graph",
  "version": 1,
  "definitions": {},
  "nodes": [],
  "outputs": {},
  "rendering": {
    "render_targets":[],
    "rendering_passes":[{"name":"main","passes":[{
      "name":"tone","type":"fullscreen",
      "output":{"color":"swapchain","depth":null},
      "shader":{"vertex":"shaders/fullscreen","fragment":"shaders/tone"}
    }]}]
  }
})json");
    const Json project{
        {"schema", "pelican.project"}, {"version", 1}, {"name", "F-5 reload_shaders"},
        {"engine_min_version", "0.1.0"},
        {"basic_config", {{"window_size", {{"width", 16}, {"height", 16}}},
                          {"framerate", 60}, {"default_scene_id", "default_scene"},
                          {"scene_data_json", "scene.json"}, {"asset_data_json", "assets.json"},
                          {"rendering_config_json", "passes/main.json"},
                          {"ui_config_json", "ui/ui.json"}, {"default_rendering_pass", "main"}}},
    };
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setProjectData(project.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    // What Studio launches: an RPC driver. Headless also leaves the file
    // watch unrequested, which an explicit request does not need either.
    launch.rpc = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    launch.shader_hot_reload = false;
    GET_MODULE(EngineTime).setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    TestSupport::requireVulkanDevice("F-5 reload_shaders");
    // The endpoint resolves the production reload owner when it is built.
    (void)GET_MODULE(watch::ReloadService);

    std::istringstream input;
    std::ostringstream output;
    EngineRpcEndpoint endpoint{input, output};
    std::uint64_t next_id = 500;
    const auto call = [&](std::string_view method, Json params = Json::object()) {
        return Json::parse(endpoint.processLine(
            Json{{"jsonrpc", "2.0"}, {"id", ++next_id}, {"method", method},
                 {"params", std::move(params)}}
                .dump()));
    };
    const auto result = [&](std::string_view method, Json params = Json::object()) {
        const auto response = call(method, std::move(params));
        INFO(response.dump(2));
        REQUIRE(response.contains("result"));
        return response.at("result");
    };
    // The red channel of the centre pixel of a fresh frame.
    const auto drawn = [&] {
        (void)result("render_frame");
        std::filesystem::remove(capture);
        (void)result("capture", {{"path", capture.generic_string()}});
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc *pixels =
            stbi_load(capture.string().c_str(), &width, &height, &channels, 4);
        REQUIRE(pixels != nullptr);
        const auto centre = static_cast<std::size_t>((height / 2) * width + width / 2) * 4;
        const int red = pixels[centre];
        stbi_image_free(pixels);
        return red;
    };
    const auto shader_status = [&] {
        return result("get_status").at("reload").at("runtime").at(
            std::string{watch::shaderReloadParticipantName});
    };
    const auto near = [](int value, int expected) {
        return std::abs(value - expected) <= 1;
    };

    const int grey = drawn();
    CHECK(near(grey, 188));
    // The file watch is shut under the RPC driver (EB-4: in a
    // PELICAN_WITH_HOT_RELOAD=OFF build by the build first; the explicit
    // request below works the same).
    const auto initial = shader_status();
    REQUIRE_FALSE(initial.at("details").at("enabled").get<bool>());
    REQUIRE(initial.at("details").at("gate_reason") ==
            (PELICAN_WITH_HOT_RELOAD ? "rpc driver" : "hot reload build disabled"));

    SECTION("a changed shader is committed at once, and drawn") {
        writeFile(shader, fragment("outColor=vec4(1.0,1.0,1.0,1.0);"));
        // The poll path is untouched: a frame after the write draws the old
        // shader, because nothing watches the file under an RPC driver.
        CHECK(near(drawn(), grey));

        const auto reloaded = result("reload_shaders");
        CHECK(reloaded.at("committed") == true);
        CHECK(reloaded.at("changed") == Json::array({"project://shaders/tone.frag"}));
        CHECK(reloaded.contains("cache_hits"));
        CHECK(reloaded.contains("cache_misses"));
        CHECK(near(drawn(), 255));
        const auto status = shader_status();
        CHECK(status.at("applied").get<std::uint64_t>() == 1);
        CHECK(status.at("last_error").is_null());

        // Nothing has moved since: nothing is attempted.
        const auto again = result("reload_shaders");
        CHECK(again == Json{{"committed", false}, {"attempted", false}});
        CHECK(shader_status().at("attempted").get<std::uint64_t>() == 1);
    }

    SECTION("a shader that does not compile is refused with its log, and the "
            "old one keeps drawing") {
        writeFile(shader, fragment("outColor=vaule0;"));
        const auto refused = call("reload_shaders");
        INFO(refused.dump(2));
        REQUIRE(refused.contains("error"));
        const auto &error = refused.at("error");
        const auto message = error.at("message").get<std::string>();
        CHECK(message.find("Shader compile failed: ") != std::string::npos);
        CHECK(message.find("tone.frag") != std::string::npos);
        CHECK(message.find("vaule0") != std::string::npos);
        CHECK(error.at("data").at("error_code") == "shader_reload_failed@1");
        CHECK(error.at("data").at("error_fields").at("changed") ==
              Json::array({"project://shaders/tone.frag"}));
        const auto status = shader_status();
        CHECK(status.at("failed").get<std::uint64_t>() == 1);
        CHECK(status.at("last_error") == message);
        CHECK(near(drawn(), grey));

        // Repaired to exactly what is published: nothing new to draw, but the
        // unit failed last time, so it is republished and the error goes.
        writeFile(shader, fragment("outColor=vec4(0.5,0.5,0.5,1.0);"));
        const auto repaired = result("reload_shaders");
        CHECK(repaired.at("committed") == true);
        CHECK(repaired.at("changed") == Json::array({"project://shaders/tone.frag"}));
        CHECK(shader_status().at("last_error").is_null());
        CHECK(near(drawn(), grey));
        // And after that, the same file is nothing to do.
        CHECK(result("reload_shaders") == Json{{"committed", false}, {"attempted", false}});
    }

    SECTION("replay keeps the gate shut to the request") {
        GET_MODULE(watch::ReloadGate).setReason(watch::ReloadGateReason::replay, true);
        writeFile(shader, fragment("outColor=vec4(1.0,1.0,1.0,1.0);"));
        const auto refused = call("reload_shaders");
        INFO(refused.dump(2));
        REQUIRE(refused.contains("error"));
        CHECK(refused.at("error").at("data").at("error_code") ==
              "shader_reload_unavailable@1");
        CHECK(refused.at("error").at("data").at("error_fields").at("reason") ==
              "replay");
        CHECK(shader_status().at("attempted").get<std::uint64_t>() == 0);
    }

    SECTION("strict assets keep the gate shut to the request") {
        launch.strict_assets = true;
        GET_MODULE(watch::ReloadGate).configureFromLaunch(launch);
        writeFile(shader, fragment("outColor=vec4(1.0,1.0,1.0,1.0);"));
        const auto refused = call("reload_shaders");
        INFO(refused.dump(2));
        REQUIRE(refused.contains("error"));
        CHECK(refused.at("error").at("data").at("error_code") ==
              "shader_reload_unavailable@1");
        CHECK(refused.at("error").at("data").at("error_fields").at("reason") ==
              "strict assets");
    }

    SECTION("parameters are refused") {
        const auto refused = call("reload_shaders", {{"path", "shaders/tone.frag"}});
        REQUIRE(refused.contains("error"));
        CHECK(refused.at("error").at("code") == -32602);
    }

    GET_MODULE(VulkanManageCore).waitIdle();
}

} // namespace Pelican
