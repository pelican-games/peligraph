#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/vkcore/core.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stb_image.h>

namespace Pelican {
namespace {

void writeFile(const std::filesystem::path &path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary};
    file << contents;
}

} // namespace

TEST_CASE("RPC capture reports contract 2 and absolute encoded-sRGB color", "[rpc][color][headless]") {
    const bool fallback = GENERATE(false, true);
    CAPTURE(fallback);
    setupLogger(true);
    FastModuleContainer modules;
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("pelican_rpc_color_" + std::to_string(fallback) + "_" + std::to_string(suffix));
    const auto capture = root / "capture.png";

    writeFile(root / "scene.json", R"json({
  "schema":"pelican.scene","version":1,
  "scenes":{"default_scene":{"objects":[]}}
})json");
    writeFile(root / "assets.json", R"json({"models":[]})json");
    writeFile(root / "ui/ui.json", R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
    writeFile(root / "shaders/fullscreen.vert", R"glsl(
#version 450
layout(location=0) out vec2 uv;
void main(){ uv=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2); gl_Position=vec4(uv*2.0-1.0,0,1); }
)glsl");
    writeFile(root / "shaders/half.frag", R"glsl(
#version 450
layout(location=0) out vec4 outColor;
void main(){ outColor=vec4(0.5,0.5,0.5,0.25); }
)glsl");
    writeFile(root / "passes/main.json", R"json({
  "render_targets":[],
  "rendering_passes":[{"name":"main","passes":[{
    "name":"known_value","type":"fullscreen",
    "output":{"color":"swapchain","depth":null},
    "shader":{"vertex":"shaders/fullscreen","fragment":"shaders/half"}
  }]}]
})json");

    const nlohmann::json project{
        {"schema", "pelican.project"}, {"version", 1}, {"name", "RPC color"},
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
    launch.headless_extent = vk::Extent2D{16, 16};
    launch.shader_hot_reload = false;
    launch.force_unorm_color_path_for_testing = fallback;
    GET_MODULE(EngineTime).setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);

    const auto capture_string = capture.generic_string();
    std::istringstream input{
        nlohmann::json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "render_frame"},
                       {"params", nlohmann::json::object()}}.dump() + "\n" +
        nlohmann::json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "capture"},
                       {"params", {{"path", capture_string}}}}.dump() + "\n" +
        nlohmann::json{{"jsonrpc", "2.0"}, {"id", 3}, {"method", "get_status"},
                       {"params", nlohmann::json::object()}}.dump() + "\n"};
    std::ostringstream output;
    runEngineRpcServer(input, output);
    GET_MODULE(VulkanManageCore).waitIdle();

    std::string first_line;
    std::string second_line;
    std::string third_line;
    std::istringstream responses{output.str()};
    std::getline(responses, first_line);
    std::getline(responses, second_line);
    std::getline(responses, third_line);
    const auto response = nlohmann::json::parse(second_line).at("result");
    REQUIRE(response.at("contract") == 2);
    REQUIRE(response.at("encoding") == "srgb");
    const auto color_status = nlohmann::json::parse(third_line).at("result").at("color");
    REQUIRE(color_status.at("contract") == 2);
    REQUIRE(color_status.at("path") == (fallback ? "unorm_fallback" : "srgb"));

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc *pixels = stbi_load(capture.string().c_str(), &width, &height, &channels, 4);
    REQUIRE(pixels != nullptr);
    REQUIRE(width == 16);
    REQUIRE(height == 16);
    const auto center = static_cast<size_t>((height / 2) * width + width / 2) * 4;
    const int tolerance = fallback ? 1 : 0;
    REQUIRE(std::abs(static_cast<int>(pixels[center + 0]) - 188) <= tolerance);
    REQUIRE(std::abs(static_cast<int>(pixels[center + 1]) - 188) <= tolerance);
    REQUIRE(std::abs(static_cast<int>(pixels[center + 2]) - 188) <= tolerance);
    REQUIRE(pixels[center + 3] == 64);
    stbi_image_free(pixels);
    std::filesystem::remove_all(root);
}

} // namespace Pelican
