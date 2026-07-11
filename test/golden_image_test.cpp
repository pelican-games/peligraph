#include "../src/core/container.hpp"
#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/fullscreenpass/fullscreenpasscontainer.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/log.hpp"
#include "../src/core/playback/vatplayer.hpp"
#include "../src/core/renderer/debugdraw.hpp"
#include "../src/core/renderer/debugtext.hpp"
#include "../src/core/renderer/camera.hpp"
#include "../src/core/renderingpass/renderingpasscontainer.hpp"
#include "../src/core/renderingpass/rendertargetcontainer.hpp"
#include "../src/core/shader/pipelinefactory.hpp"
#include "../src/core/shader/shadercompiler.hpp"
#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/shader/surfacecompiler.hpp"
#include "../src/core/userpublic/details/system/registerer.hpp"
#include "../src/core/userpublic/gamecontext.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/core/vkcore/rendertarget.hpp"
#include "../src/core/vkcore/rendertiming.hpp"
#include "vat_fixture.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <picosha2.h>
#include <stdexcept>
#include <string>
#include <vector>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace Pelican {

namespace {

constexpr uint32_t goldenWidth = 16;
constexpr uint32_t goldenHeight = 16;

struct RgbaImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;
};

struct GoldenCase {
    std::string name;
    std::string mode;
    std::filesystem::path root;
};

struct RenderedCase {
    RgbaImage image;
    std::string device_name;
    nlohmann::json execution_trace;
    std::vector<std::string> plan_order;
    std::vector<std::string> gpu_timing_node_names;
    uint32_t gpu_timing_query_count = 0;
    bool gpu_timing_queries_collected = false;
};

struct Tolerance {
    double average = 0.0;
    int max = 0;
};

struct CompareResult {
    double average = 0.0;
    int max = 0;
    RgbaImage diff;
};

std::filesystem::path sourceRoot() { return std::filesystem::path{PELICAN_TEST_SOURCE_DIR}; }
std::filesystem::path binaryRoot() { return std::filesystem::path{PELICAN_TEST_BINARY_DIR}; }

std::vector<GoldenCase> discoverGoldenCases() {
    const auto root = sourceRoot() / "test/golden";
    std::vector<GoldenCase> cases;
    for (const auto &entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_directory()) {
            continue;
        }
        const auto config_path = entry.path() / "case.json";
        if (!std::filesystem::exists(config_path)) {
            continue;
        }
        std::ifstream file{config_path};
        const auto config = nlohmann::json::parse(file);
        const auto mode = config.at("mode").get<std::string>();
#if !PELICAN_WITH_VAT
        if (mode == "vat_playback") {
            continue;
        }
#endif
        cases.push_back(GoldenCase{
            entry.path().filename().string(),
            mode,
            entry.path(),
        });
    }
    std::sort(cases.begin(), cases.end(), [](const GoldenCase &lhs, const GoldenCase &rhs) {
        return lhs.name < rhs.name;
    });
    return cases;
}

std::filesystem::path makeTempProjectDir(const std::string &case_name) {
    auto dir = std::filesystem::temp_directory_path() / ("pelican_golden_" + case_name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

void writeTextFile(const std::filesystem::path &path, const std::string &contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary};
    file << contents;
}

nlohmann::json makeProjectConfig(const std::filesystem::path &scene_path, const std::filesystem::path &asset_path) {
    return nlohmann::json{
        {"basic_config",
         {
             {"window_size", {{"width", goldenWidth}, {"height", goldenHeight}}},
             {"scene_data_json", scene_path.generic_string()},
             {"asset_data_json", asset_path.generic_string()},
         }},
    };
}

nlohmann::json makeStemProjectJson() {
    return nlohmann::json{
        {"schema", "pelican.project"},
        {"version", 1},
        {"name", "stem golden"},
        {"engine_min_version", "0.1.0"},
        {"basic_config",
         {
             {"window_title", "Stem Golden"},
             {"window_size", {{"width", goldenWidth}, {"height", goldenHeight}}},
             {"fullscreen", false},
             {"framerate", 60},
             {"camera", {{"yfov", 1.0471975511965976}, {"znear", 0.1}, {"zfar", 100.0}, {"up", {0, 1, 0}}}},
             {"default_scene_id", "default_scene"},
             {"scene_data_json", "scene.json"},
             {"asset_data_json", "assets.json"},
             {"rendering_config_json", "passes/main.json"},
             {"ui_config_json", "ui/ui.json"},
             {"default_rendering_pass", "main"},
         }},
    };
}

nlohmann::json makeVatProjectJson() {
    return nlohmann::json{
        {"schema", "pelican.project"},
        {"version", 1},
        {"name", "vat golden"},
        {"engine_min_version", "0.1.0"},
        {"basic_config",
         {
             {"window_title", "VAT Golden"},
             {"window_size", {{"width", goldenWidth}, {"height", goldenHeight}}},
             {"fullscreen", false},
             {"framerate", 60},
             {"camera", {{"yfov", 0.7853981633974483}, {"znear", 0.1}, {"zfar", 1000.0}, {"up", {0, 1, 0}}}},
             {"default_scene_id", "default_scene"},
             {"scene_data_json", "scenes/main.scene.json"},
             {"asset_data_json", "assets/asset_data.json"},
             {"rendering_config_json", "passes/main_rendering_config.json"},
             {"ui_config_json", "ui/ui_overlay.json"},
             {"default_rendering_pass", "main_render"},
         }},
    };
}

nlohmann::json makeFeatureProjectJson() {
    return nlohmann::json{
        {"schema", "pelican.project"},
        {"version", 1},
        {"name", "feature golden"},
        {"engine_min_version", "0.1.0"},
        {"basic_config",
         {
             {"window_title", "Feature Golden"},
             {"window_size", {{"width", goldenWidth}, {"height", goldenHeight}}},
             {"fullscreen", false},
             {"framerate", 60},
             {"camera", {{"yfov", 0.7853981633974483}, {"znear", 0.1}, {"zfar", 100.0}, {"up", {0, 1, 0}}}},
             {"default_scene_id", "default_scene"},
             {"scene_data_json", "scene.json"},
             {"asset_data_json", "assets.json"},
             {"rendering_config_json", "passes/main.json"},
             {"ui_config_json", "ui/ui.json"},
             {"default_rendering_pass", "main"},
         }},
    };
}

nlohmann::json makeHdrProjectJson() {
    return nlohmann::json{
        {"schema", "pelican.project"},
        {"version", 1},
        {"name", "hdr golden"},
        {"engine_min_version", "0.1.0"},
        {"basic_config",
         {
             {"window_title", "HDR Golden"},
             {"window_size", {{"width", goldenWidth}, {"height", goldenHeight}}},
             {"fullscreen", false},
             {"framerate", 60},
             {"camera", {{"yfov", 0.7853981633974483}, {"znear", 0.1}, {"zfar", 100.0}, {"up", {0, 1, 0}}}},
             {"default_scene_id", "default_scene"},
             {"scene_data_json", "scene.json"},
             {"asset_data_json", "assets.json"},
             {"rendering_config_json", "passes/main.json"},
             {"ui_config_json", "ui/ui.json"},
             {"default_rendering_pass", "main"},
         }},
    };
}

nlohmann::json makeShadowProjectJson() {
    return nlohmann::json{
        {"schema", "pelican.project"},
        {"version", 1},
        {"name", "shadow golden"},
        {"engine_min_version", "0.1.0"},
        {"basic_config",
         {
             {"window_title", "Shadow Golden"},
             {"window_size", {{"width", goldenWidth}, {"height", goldenHeight}}},
             {"fullscreen", false},
             {"framerate", 60},
             {"camera", {{"yfov", 0.8726646259971648}, {"znear", 0.1}, {"zfar", 100.0}, {"up", {0, 1, 0}}}},
             {"default_scene_id", "default_scene"},
             {"scene_data_json", "scene.json"},
             {"asset_data_json", "assets.json"},
             {"rendering_config_json", "passes/main.json"},
             {"ui_config_json", "ui/ui.json"},
             {"default_rendering_pass", "main"},
         }},
    };
}

nlohmann::json makeComputeProjectJson() {
    return nlohmann::json{
        {"schema", "pelican.project"},
        {"version", 1},
        {"name", "compute buffer golden"},
        {"engine_min_version", "0.1.0"},
        {"basic_config",
         {
             {"window_title", "Compute Buffer Golden"},
             {"window_size", {{"width", goldenWidth}, {"height", goldenHeight}}},
             {"fullscreen", false},
             {"framerate", 60},
             {"camera", {{"yfov", 0.7853981633974483}, {"znear", 0.1}, {"zfar", 100.0}, {"up", {0, 1, 0}}}},
             {"default_scene_id", "default_scene"},
             {"scene_data_json", "scene.json"},
             {"asset_data_json", "assets.json"},
             {"rendering_config_json", "passes/main.json"},
             {"ui_config_json", "ui/ui.json"},
             {"default_rendering_pass", "main"},
         }},
    };
}

RgbaImage loadPng(const std::filesystem::path &path) {
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char *data = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
    if (data == nullptr) {
        throw std::runtime_error("failed to load PNG: " + path.string());
    }

    RgbaImage image;
    image.width = static_cast<uint32_t>(width);
    image.height = static_cast<uint32_t>(height);
    image.pixels.assign(data, data + static_cast<size_t>(width) * height * 4);
    stbi_image_free(data);
    return image;
}

void writePng(const std::filesystem::path &path, const RgbaImage &image) {
    std::filesystem::create_directories(path.parent_path());
    const int stride = static_cast<int>(image.width * 4);
    if (stbi_write_png(path.string().c_str(), static_cast<int>(image.width), static_cast<int>(image.height), 4,
                       image.pixels.data(), stride) == 0) {
        throw std::runtime_error("failed to write PNG: " + path.string());
    }
}

Tolerance loadTolerance(const std::filesystem::path &path) {
    std::ifstream file{path};
    const auto config = nlohmann::json::parse(file);
    return Tolerance{
        config.value("average", 0.0),
        config.value("max", 0),
    };
}

bool updateGoldenRequested() {
    const char *value = std::getenv("PELICAN_UPDATE_GOLDEN");
    return value != nullptr && std::string{value} == "1";
}

bool writeColorDiffArtifactsRequested() {
    const char *value = std::getenv("PELICAN_WRITE_COLOR_DIFF_ARTIFACTS");
    return value != nullptr && std::string{value} == "1";
}

void renderClearFrame(RenderTarget &render_target, vk::ClearColorValue clear_color) {
    auto frame = render_target.render_begin();

    vk::RenderingAttachmentInfo color_attachment;
    color_attachment.imageView = frame.color_attachment;
    color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_attachment.loadOp = vk::AttachmentLoadOp::eClear;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;
    color_attachment.clearValue.color = clear_color;

    vk::RenderingInfo rendering_info;
    rendering_info.renderArea = vk::Rect2D{{0, 0}, frame.extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(color_attachment);

    frame.cmd_buf.beginRendering(rendering_info);
    frame.cmd_buf.endRendering();
    render_target.render_end();
}

const char *fullscreenVertexShader() {
    return R"glsl(
#version 450
layout(location = 0) out vec2 outUV;
void main() {
    outUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(outUV * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";
}

const char *solidFullscreenFragmentShader() {
    return R"glsl(
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(0.9, 0.5, 0.1, 1.0);
}
)glsl";
}

const char *explicitOrderBlueFragmentShader() {
    return R"glsl(
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(0.05, 0.15, 0.90, 1.0);
}
)glsl";
}

const char *explicitOrderRedFragmentShader() {
    return R"glsl(
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(0.90, 0.10, 0.05, 1.0);
}
)glsl";
}

const char *triangleMaskFragmentShader() {
    return R"glsl(
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    if (gl_FragCoord.x + gl_FragCoord.y < 16.0) {
        outColor = vec4(0.0, 0.8, 0.2, 1.0);
    } else {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
    }
}
)glsl";
}

const char *hdrSourceFragmentShader() {
    return R"glsl(
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(4.0, 1.5, 0.5, 1.0);
}
)glsl";
}

const char *copyInputFragmentShader() {
    return R"glsl(
#version 450
layout(set = 1, binding = 0) uniform sampler2D inputTexture;
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = texture(inputTexture, inUV);
}
)glsl";
}

const char *computeWriteColorShader() {
    return R"glsl(
#version 450
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(std430, set = 1, binding = 0) buffer ComputeColor {
    vec4 color;
} compute_color;
void main() {
    compute_color.color = vec4(0.10, 0.75, 0.35, 1.0);
}
)glsl";
}

const char *computeBufferFragmentShader() {
    return R"glsl(
#version 450
layout(std430, set = 1, binding = 0) readonly buffer ComputeColor {
    vec4 color;
} compute_color;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = compute_color.color;
}
)glsl";
}

const char *orthographicCameraFragmentShader() {
    return R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_frame.glsl"
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(pelicanFrame.projection[0][0], pelicanFrame.projection[1][1], 0.25, 1.0);
}
)glsl";
}

const char *orbitCameraControllerFragmentShader() {
    return R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_frame.glsl"
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(
        clamp(-pelicanFrame.view[3][2] / 8.0, 0.0, 1.0),
        clamp(pelicanFrame.view[0][2] * 0.5 + 0.5, 0.0, 1.0),
        clamp(pelicanFrame.view[2][0] * 0.5 + 0.5, 0.0, 1.0),
        1.0
    );
}
)glsl";
}

const char *stemFullscreenVertexShader() {
    return R"glsl(
#version 450
layout(location = 0) out vec2 outUV;
vec2 positions[6] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0,  1.0)
);
void main() {
    vec2 pos = positions[gl_VertexIndex];
    outUV = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
)glsl";
}

ShaderBundleId compileShaderToBundle(ShaderLibrary &library, const std::string &source,
                                     vk::ShaderStageFlagBits stage, const std::string &name) {
#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompiler compiler;
    const auto result = compiler.compileSource(source, stage, name);
    if (!result.ok) {
        throw std::runtime_error("golden shader compile failed: " + result.log);
    }
    return library.loadFromSpirv(result.spirv, name);
#else
    (void)library;
    (void)source;
    (void)stage;
    (void)name;
    throw std::runtime_error("runtime shader compiler disabled");
#endif
}

void renderFullscreenFrameWithFragment(RenderTarget &render_target, ShaderBundleId frag) {
    auto &library = GET_MODULE(ShaderLibrary);
    const auto vert = compileShaderToBundle(library, fullscreenVertexShader(), vk::ShaderStageFlagBits::eVertex,
                                            "golden_fullscreen.vert");

    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    const auto pipeline = pipeline_factory.create(GraphicsPipelineDesc{
        vert,
        frag,
        {render_target.getSwapchainFormat()},
    });

    auto frame = render_target.render_begin();

    vk::RenderingAttachmentInfo color_attachment;
    color_attachment.imageView = frame.color_attachment;
    color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_attachment.loadOp = vk::AttachmentLoadOp::eClear;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;
    color_attachment.clearValue.color = vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 1.0f}};

    vk::RenderingInfo rendering_info;
    rendering_info.renderArea = vk::Rect2D{{0, 0}, frame.extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(color_attachment);

    frame.cmd_buf.beginRendering(rendering_info);
    frame.cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline_factory.pipeline(pipeline));
    frame.cmd_buf.draw(3, 1, 0, 0);
    frame.cmd_buf.endRendering();

    render_target.render_end();
}

void renderFullscreenFrame(RenderTarget &render_target, const std::string &fragment_shader_source) {
    auto &library = GET_MODULE(ShaderLibrary);
    const auto frag = compileShaderToBundle(library, fragment_shader_source,
                                            vk::ShaderStageFlagBits::eFragment,
                                            "golden_fullscreen.frag");
    renderFullscreenFrameWithFragment(render_target, frag);
}

const char *surfaceGoldenFragmentTemplate() {
    return R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : enable
#extension GL_GOOGLE_cpp_style_line_directive : enable
#include "pelican_surface_v1.glsl"

vec4 pelican_param_tint() { return vec4(1.0, 0.32, 0.08, 1.0); }
uint pelican_light_count() { return 1u; }
PelicanLightV1 pelican_light(uint index, vec3 world_position) {
    PelicanLightV1 light;
    light.direction = normalize(vec3(0.3, 0.2, 1.0));
    light.radiance = vec3(0.9, 0.8, 0.7);
    light.attenuation = 1.0;
    return light;
}
float pelican_shadow(uint light_index, vec3 world_position) { return 1.0; }
vec3 pelican_env_ambient(vec3 normal) { return vec3(0.04, 0.05, 0.08); }

#include "__pelican_user_surface.glsl"

layout(location = 0) out vec4 outColor;
void main() {
    PelicanSurfaceInputV1 surface_input;
    surface_input.uv = gl_FragCoord.xy / vec2(16.0);
    surface_input.vertex_color = vec4(1.0);
    surface_input.world_position = vec3(0.0);
    surface_input.normal = vec3(0.0, 0.0, 1.0);
    surface_input.view_direction = vec3(0.0, 0.0, 1.0);
    surface_input.custom0 = vec4(0.0);
    surface_input.custom1 = vec4(0.0);
    PelicanSurfaceV1 surface;
    surface.base_color = vec4(1.0);
    surface.normal = surface_input.normal;
    surface.metallic = 0.0;
    surface.roughness = 1.0;
    surface.occlusion = 1.0;
    surface.emissive = vec3(0.0);
    pelican_surface_v1(surface_input, surface);
    outColor = vec4(pelican_lighting_v1(surface, surface_input), surface.base_color.a);
}
)glsl";
}

void renderSurfaceToonFrame(RenderTarget &render_target) {
#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto path = sourceRoot() / "projects" / "example" / "shaders" / "toon.surface";
    std::ifstream file{path, std::ios::binary};
    if (!file) throw std::runtime_error("failed to open B-layer golden surface: " + path.string());
    const std::string source{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    const auto surface = parseSurfaceFormat(source, path.generic_string());
    const auto composition = composeSurfaceShaders(surface, path.generic_string());
    ShaderCompiler compiler;
    ShaderCompileOptions options;
    options.virtual_includes = composition.virtual_includes;
    const auto result = compiler.compileSource(surfaceGoldenFragmentTemplate(),
                                               vk::ShaderStageFlagBits::eFragment,
                                               "engine://golden/surface_toon.frag", options);
    if (!result.ok) throw std::runtime_error("surface toon golden compile failed: " + result.log);
    auto &library = GET_MODULE(ShaderLibrary);
    renderFullscreenFrameWithFragment(render_target,
                                      library.loadFromSpirv(result.spirv, "surface_toon.frag"));
#else
    (void)render_target;
    throw std::runtime_error("runtime shader compiler disabled");
#endif
}

void writeStemProject(const std::filesystem::path &root) {
    writeTextFile(root / "project.json", makeStemProjectJson().dump(2));
    writeTextFile(root / "scene.json", R"json({
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": []
    }
  }
})json");
    writeTextFile(root / "assets.json", R"json({"models":[]})json");
    writeTextFile(root / "ui" / "ui.json", R"json({"images":[]})json");
    writeTextFile(root / "shaders" / "solid.vert", stemFullscreenVertexShader());
    writeTextFile(root / "shaders" / "solid.frag", solidFullscreenFragmentShader());
    writeTextFile(root / "passes" / "main.json", R"json({
  "render_targets": [],
  "rendering_passes": [
    {
      "name": "main",
      "passes": [
        {
          "name": "solid",
          "type": "fullscreen",
          "output": {"color": "swapchain", "depth": null},
          "shader": {
            "vertex": "shaders/solid",
            "fragment": "shaders/solid"
          }
        }
      ]
    }
  ]
})json");
}

void writeExplicitOrderProject(const std::filesystem::path &root) {
    writeTextFile(root / "project.json", makeFeatureProjectJson().dump(2));
    writeTextFile(root / "scene.json", R"json({
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {"default_scene": {"objects": []}}
})json");
    writeTextFile(root / "assets.json", R"json({"models":[]})json");
    writeTextFile(root / "ui" / "ui.json", R"json({"images":[]})json");
    writeTextFile(root / "shaders" / "fullscreen.vert", stemFullscreenVertexShader());
    writeTextFile(root / "shaders" / "blue.frag", explicitOrderBlueFragmentShader());
    writeTextFile(root / "shaders" / "red.frag", explicitOrderRedFragmentShader());
    writeTextFile(root / "passes" / "main.json", R"json({
  "features": ["engine://features/gpu_timing.json"],
  "render_targets": [],
  "rendering_passes": [
    {
      "name": "main",
      "passes": [
        {
          "name": "red_after_blue",
          "type": "fullscreen",
          "after": ["blue_first"],
          "output": {"color": "swapchain", "depth": null},
          "shader": {"vertex": "shaders/fullscreen", "fragment": "shaders/red"}
        },
        {
          "name": "blue_first",
          "type": "fullscreen",
          "output": {"color": "swapchain", "depth": null},
          "shader": {"vertex": "shaders/fullscreen", "fragment": "shaders/blue"}
        }
      ]
    }
  ]
})json");
}

void writeVatProject(const std::filesystem::path &root) {
    writeTextFile(root / "project.json", makeVatProjectJson().dump(2));
    writeTextFile(root / "scenes" / "main.scene.json", R"json({
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": []
    }
  }
})json");
    writeTextFile(root / "assets" / "asset_data.json", R"json({"models":[]})json");
    writeTextFile(root / "ui" / "ui_overlay.json", R"json({"images":[]})json");

    std::filesystem::create_directories(root / "passes");
    std::filesystem::copy_file(sourceRoot() / "projects/example/passes/main_rendering_config.json",
                               root / "passes/main_rendering_config.json",
                               std::filesystem::copy_options::overwrite_existing);
}

void writeFeatureProject(const std::filesystem::path &root) {
    writeTextFile(root / "project.json", makeFeatureProjectJson().dump(2));
    writeTextFile(root / "scene.json", R"json({
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": []
    }
  }
})json");
    writeTextFile(root / "assets.json", R"json({"models":[]})json");
    writeTextFile(root / "ui" / "ui.json", R"json({"images":[]})json");
    writeTextFile(root / "shaders" / "fullscreen.vert", stemFullscreenVertexShader());
    writeTextFile(root / "shaders" / "present.frag", R"glsl(
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(0.02, 0.02, 0.04, 1.0);
}
)glsl");
    writeTextFile(root / "shaders" / "feature.frag", R"glsl(
#version 450
#ifndef PELICAN_FEATURE_DUMMY
#error PELICAN_FEATURE_DUMMY must be defined
#endif
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(0.15, 0.70, 0.95, 1.0);
}
)glsl");
    writeTextFile(root / "features" / "dummy.json", R"json({
  "schema": "pelican.render_feature",
  "version": 1,
  "name": "dummy_feature",
  "passes": [
    {
      "insert": "after:post_ldr",
      "pass": {
        "name": "feature_present",
        "type": "fullscreen",
        "output": {"color": "swapchain", "depth": null},
        "shader": {
          "vertex": "shaders/fullscreen",
          "fragment": "shaders/feature"
        }
      }
    }
  ],
  "shader_defines": ["PELICAN_FEATURE_DUMMY"]
})json");
    writeTextFile(root / "passes" / "main.json", R"json({
  "features": ["features/dummy.json"],
  "render_targets": [],
  "rendering_passes": [
    {
      "name": "main",
      "passes": [
        {
          "name": "present",
          "type": "fullscreen",
          "output": {"color": "swapchain", "depth": null},
          "shader": {
            "vertex": "shaders/fullscreen",
            "fragment": "shaders/present"
          }
        }
      ]
    }
  ]
})json");
}

void writeDebugDrawProject(const std::filesystem::path &root) {
    writeTextFile(root / "project.json", makeFeatureProjectJson().dump(2));
    writeTextFile(root / "scene.json", R"json({
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": []
    }
  }
})json");
    writeTextFile(root / "assets.json", R"json({"models":[]})json");
    writeTextFile(root / "ui" / "ui.json", R"json({"images":[]})json");
    writeTextFile(root / "shaders" / "fullscreen.vert", stemFullscreenVertexShader());
    writeTextFile(root / "shaders" / "present.frag", R"glsl(
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(0.02, 0.02, 0.04, 1.0);
}
)glsl");
    writeTextFile(root / "passes" / "main.json", R"json({
  "features": ["engine://features/debug_draw.json"],
  "render_targets": [],
  "rendering_passes": [
    {
      "name": "main",
      "passes": [
        {
          "name": "present",
          "type": "fullscreen",
          "output": {"color": "swapchain", "depth": null},
          "shader": {
            "vertex": "shaders/fullscreen",
            "fragment": "shaders/present"
          }
        }
      ]
    }
  ]
})json");
}

void writeDebugTextProject(const std::filesystem::path &root) {
    writeTextFile(root / "project.json", makeFeatureProjectJson().dump(2));
    writeTextFile(root / "scene.json", R"json({
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": []
    }
  }
})json");
    writeTextFile(root / "assets.json", R"json({"models":[]})json");
    writeTextFile(root / "ui" / "ui.json", R"json({"images":[]})json");
    writeTextFile(root / "shaders" / "fullscreen.vert", stemFullscreenVertexShader());
    writeTextFile(root / "shaders" / "present.frag", R"glsl(
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(0.02, 0.02, 0.04, 1.0);
}
)glsl");
    writeTextFile(root / "passes" / "main.json", R"json({
  "features": ["engine://features/debug_text.json"],
  "render_targets": [],
  "rendering_passes": [
    {
      "name": "main",
      "passes": [
        {
          "name": "present",
          "type": "fullscreen",
          "output": {"color": "swapchain", "depth": null},
          "shader": {
            "vertex": "shaders/fullscreen",
            "fragment": "shaders/present"
          }
        }
      ]
    }
  ]
})json");
}

void writeColliderDebugDrawProject(const std::filesystem::path &root) {
    writeDebugDrawProject(root);
    writeTextFile(root / "scene.json", R"json({
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": [
        {
          "name": "ColliderSphere",
          "components": [
            {"name": "transform", "pos": [3.0, -0.45, -0.45], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
            {"name": "collider", "shape": "sphere", "radius": 0.28}
          ]
        },
        {
          "name": "ColliderBox",
          "components": [
            {"name": "transform", "pos": [3.0, 0.45, 0.0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
            {"name": "collider", "shape": "box", "half_extents": [0.28, 0.28, 0.28]}
          ]
        },
        {
          "name": "ColliderCapsule",
          "components": [
            {"name": "transform", "pos": [3.0, -0.35, 0.45], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
            {"name": "collider", "shape": "capsule", "radius": 0.18, "half_height": 0.32}
          ]
        }
      ]
    }
  }
})json");
}

nlohmann::json makeHdrRenderingConfig(bool hdr_enabled) {
    auto config = nlohmann::json{
        {"render_targets",
         nlohmann::json::array({
             {
                 {"name", "lit_color"},
                 {"extent_scale", 1.0},
                 {"format", "B8G8R8A8_UNORM"},
                 {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
             },
             {
                 {"name", "Bloom_Threshold_RT"},
                 {"extent_scale", 1.0},
                 {"format", "B8G8R8A8_UNORM"},
                 {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
             },
         })},
        {"rendering_passes",
         nlohmann::json::array({
             {
                 {"name", "main"},
                 {"passes",
                  nlohmann::json::array({
                      {
                          {"name", "hdr_source"},
                          {"type", "fullscreen"},
                          {"output", {{"color", "lit_color"}, {"depth", nullptr}}},
                          {"shader", {{"vertex", "shaders/fullscreen"}, {"fragment", "shaders/hdr_source"}}},
                      },
                      {
                          {"name", "HighLuminanceExtraction"},
                          {"type", "fullscreen"},
                          {"output", {{"color", "Bloom_Threshold_RT"}, {"depth", nullptr}}},
                          {"input", nlohmann::json::array({"lit_color"})},
                          {"shader", {{"vertex", "engine://fullscreen"},
                                      {"fragment", "engine://bloom_highpass"}}},
                          {"clear_color", nlohmann::json::array({0.0, 0.0, 0.0, 1.0})},
                      },
                      {
                          {"name", "FinalBloomComposite"},
                          {"type", "fullscreen"},
                          {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
                          {"input", nlohmann::json::array({"lit_color", "Bloom_Threshold_RT"})},
                          {"shader", {{"vertex", "engine://fullscreen"},
                                      {"fragment", "engine://bloom_composite"}}},
                          {"clear_color", nlohmann::json::array({0.0, 0.0, 0.0, 1.0})},
                      },
                      {
                          {"name", "present"},
                          {"type", "ui"},
                          {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
                      },
                  })},
             },
         })},
    };

    config["features"] = nlohmann::json::array({"engine://features/debug_text.json"});
    if (hdr_enabled) {
        config["features"].push_back("engine://features/hdr.json");
    }
    return config;
}

void writeHdrProject(const std::filesystem::path &root, bool hdr_enabled) {
    writeTextFile(root / "project.json", makeHdrProjectJson().dump(2));
    writeTextFile(root / "scene.json", R"json({
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": []
    }
  }
})json");
    writeTextFile(root / "assets.json", R"json({"models":[]})json");
    writeTextFile(root / "ui" / "ui.json", R"json({"images":[]})json");
    writeTextFile(root / "shaders" / "fullscreen.vert", stemFullscreenVertexShader());
    writeTextFile(root / "shaders" / "hdr_source.frag", hdrSourceFragmentShader());
    writeTextFile(root / "shaders" / "copy_input.frag", copyInputFragmentShader());
    writeTextFile(root / "passes" / "main.json", makeHdrRenderingConfig(hdr_enabled).dump(2));
}

nlohmann::json makeFullscreenRebindRenderingConfig() {
    return {
        {"render_targets",
         nlohmann::json::array({
             {
                 {"name", "lit_color"},
                 {"extent_scale", 1.0},
                 {"format", "B8G8R8A8_UNORM"},
                 {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
             },
         })},
        {"rendering_passes",
         nlohmann::json::array({
             {
                 {"name", "main"},
                 {"passes",
                  nlohmann::json::array({
                      {
                          {"name", "source"},
                          {"type", "fullscreen"},
                          {"output", {{"color", "lit_color"}, {"depth", nullptr}}},
                          {"shader", {{"vertex", "shaders/fullscreen"},
                                      {"fragment", "shaders/hdr_source"}}},
                      },
                      {
                          {"name", "copy_input"},
                          {"type", "fullscreen"},
                          {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
                          {"input", nlohmann::json::array({"lit_color"})},
                          {"shader", {{"vertex", "shaders/fullscreen"},
                                      {"fragment", "shaders/copy_input"}}},
                      },
                  })},
             },
         })},
    };
}

nlohmann::json makeShadowRenderingConfig(bool shadow_enabled) {
    auto passes = nlohmann::json::array();
    passes.push_back({
        {"name", "gbuffer_pass"},
        {"type", "material"},
        {"output",
         {{"color",
           nlohmann::json::array({"gbuffer_albedo", "gbuffer_normal", "gbuffer_material",
                                   "gbuffer_worldpos", "g_emissive"})},
          {"depth", "offscreen_depth"}}},
    });
    passes.push_back({
        {"name", "ssao_clear"},
        {"type", "fullscreen"},
        {"output", {{"color", "ssao_blur"}, {"depth", nullptr}}},
        {"shader", {{"vertex", "shaders/fullscreen"}, {"fragment", "shaders/white"}}},
        {"clear_color", nlohmann::json::array({1.0, 1.0, 1.0, 1.0})},
    });
    passes.push_back({
        {"name", "lighting_pass"},
        {"type", "fullscreen"},
        {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
        {"input",
         nlohmann::json::array({"gbuffer_albedo", "gbuffer_normal", "gbuffer_material",
                                 "gbuffer_worldpos", "g_emissive", "ssao_blur"})},
        {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://fullscreen"}}},
        {"push_constants", "camera_position"},
        {"uses_light_data", true},
    });

    auto config = nlohmann::json{
        {"render_targets",
         nlohmann::json::array({
             {{"name", "gbuffer_albedo"},
              {"extent_scale", 1.0},
              {"format", "B8G8R8A8_UNORM"},
              {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})}},
             {{"name", "gbuffer_normal"},
              {"extent_scale", 1.0},
              {"format", "R16G16B16A16_SFLOAT"},
              {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})}},
             {{"name", "gbuffer_material"},
              {"extent_scale", 1.0},
              {"format", "R8G8B8A8_UNORM"},
              {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})}},
             {{"name", "gbuffer_worldpos"},
              {"extent_scale", 1.0},
              {"format", "R16G16B16A16_SFLOAT"},
              {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})}},
             {{"name", "g_emissive"},
              {"extent_scale", 1.0},
              {"format", "R8G8B8A8_UNORM"},
              {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})}},
             {{"name", "offscreen_depth"},
              {"extent_scale", 1.0},
              {"format", "D32_SFLOAT"},
              {"usage", nlohmann::json::array({"DEPTH_STENCIL_ATTACHMENT"})}},
             {{"name", "ssao_blur"},
              {"extent_scale", 1.0},
              {"format", "R8_UNORM"},
              {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})}},
         })},
        {"rendering_passes",
         nlohmann::json::array({
             {{"name", "main"}, {"passes", passes}},
         })},
    };

    if (shadow_enabled) {
        config["features"] = nlohmann::json::array({"engine://features/shadow_directional.json"});
    }
    return config;
}

void writeShadowProject(const std::filesystem::path &root, bool shadow_enabled) {
    writeTextFile(root / "project.json", makeShadowProjectJson().dump(2));
    writeTextFile(root / "scene.json", R"json({
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": [
        {
          "name": "Ground",
          "components": [
            {"name": "transform", "pos": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [4.0, 0.05, 4.0]},
            {"name": "simplemodelview", "model": "ground"}
          ]
        },
        {
          "name": "Caster",
          "components": [
            {"name": "transform", "pos": [0.0, 0.55, 0.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [0.7, 0.7, 0.7]},
            {"name": "simplemodelview", "model": "ground"}
          ]
        },
        {
          "name": "Sun",
          "components": [
            {"name": "light", "type": "directional", "direction": [0.35, -1.0, -0.25], "intensity": 3.0, "color": [1.0, 0.94, 0.82]}
          ]
        }
      ]
    }
  }
})json");
    writeTextFile(root / "assets.json", R"json({
  "models": [
    {"name": "ground", "path": "assets/ground.glb"}
  ]
})json");
    writeTextFile(root / "ui" / "ui.json", R"json({"images":[]})json");
    writeTextFile(root / "shaders" / "fullscreen.vert", stemFullscreenVertexShader());
    writeTextFile(root / "shaders" / "white.frag", R"glsl(
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(1.0);
}
)glsl");
    writeTextFile(root / "passes" / "main.json", makeShadowRenderingConfig(shadow_enabled).dump(2));

    std::filesystem::create_directories(root / "assets");
    std::filesystem::copy_file(sourceRoot() / "test" / "fixtures" / "ground.glb",
                               root / "assets" / "ground.glb",
                               std::filesystem::copy_options::overwrite_existing);
}

void writeComputeProject(const std::filesystem::path &root) {
    writeTextFile(root / "project.json", makeComputeProjectJson().dump(2));
    writeTextFile(root / "scene.json", R"json({
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": []
    }
  }
})json");
    writeTextFile(root / "assets.json", R"json({"models":[]})json");
    writeTextFile(root / "ui" / "ui.json", R"json({"images":[]})json");
    writeTextFile(root / "shaders" / "fullscreen.vert", stemFullscreenVertexShader());
    writeTextFile(root / "shaders" / "write_color.comp", computeWriteColorShader());
    writeTextFile(root / "shaders" / "compute_present.frag", computeBufferFragmentShader());
    writeTextFile(root / "passes" / "main.json", R"json({
  "buffers": [
    {"name": "compute_color", "size": 16, "lifetime": "persistent"}
  ],
  "rendering_passes": [
    {
      "name": "main",
      "passes": [
        {
          "name": "present",
          "type": "fullscreen",
          "input": ["compute_color"],
          "output": {"color": "swapchain", "depth": null},
          "shader": {
            "vertex": "shaders/fullscreen",
            "fragment": "shaders/compute_present"
          }
        }
      ]
    }
  ],
  "compute_tasks": [
    {
      "name": "write_color",
      "shader": "shaders/write_color",
      "writes": ["compute_color"],
      "before": ["present"],
      "dispatch": {"groups": [1, 1, 1]},
      "schedule": "per_frame"
    }
  ]
})json");
}

void writeOrthographicCameraProject(const std::filesystem::path &root) {
    writeTextFile(root / "project.json", makeFeatureProjectJson().dump(2));
    writeTextFile(root / "scene.json", R"json({
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": [
        {
          "name": "OrthoGoldenCamera",
          "components": [
            {
              "name": "camera",
              "type": "orthographic",
              "xmag": 2.0,
              "ymag": 4.0,
              "znear": 0.1,
              "zfar": 20.0
            }
          ]
        }
      ]
    }
  }
})json");
    writeTextFile(root / "assets.json", R"json({"models":[]})json");
    writeTextFile(root / "ui" / "ui.json", R"json({"images":[]})json");
    writeTextFile(root / "shaders" / "fullscreen.vert", stemFullscreenVertexShader());
    writeTextFile(root / "shaders" / "ortho_camera.frag", orthographicCameraFragmentShader());
    writeTextFile(root / "passes" / "main.json", R"json({
  "render_targets": [],
  "rendering_passes": [
    {
      "name": "main",
      "passes": [
        {
          "name": "ortho_camera",
          "type": "fullscreen",
          "output": {"color": "swapchain", "depth": null},
          "shader": {
            "vertex": "shaders/fullscreen",
            "fragment": "shaders/ortho_camera"
          },
          "push_constants": "projection_view"
        }
      ]
    }
  ]
})json");
}

void writeOrbitCameraControllerProject(const std::filesystem::path &root) {
    writeTextFile(root / "project.json", makeFeatureProjectJson().dump(2));
    writeTextFile(root / "scene.json", R"json({
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": [
        {
          "name": "Target",
          "components": [
            {"name": "transform", "pos": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0]}
          ]
        },
        {
          "name": "OrbitGoldenCamera",
          "components": [
            {"name": "transform", "pos": [0.0, 0.0, 4.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0]},
            {
              "name": "camera",
              "controller": {
                "type": "orbit",
                "target": "Target",
                "distance": 4.0,
                "yaw_degrees": 90.0,
                "pitch_degrees": 0.0,
                "damping": 0.0
              }
            }
          ]
        }
      ]
    }
  }
})json");
    writeTextFile(root / "assets.json", R"json({"models":[]})json");
    writeTextFile(root / "ui" / "ui.json", R"json({"images":[]})json");
    writeTextFile(root / "shaders" / "fullscreen.vert", stemFullscreenVertexShader());
    writeTextFile(root / "shaders" / "orbit_camera.frag", orbitCameraControllerFragmentShader());
    writeTextFile(root / "passes" / "main.json", R"json({
  "render_targets": [],
  "rendering_passes": [
    {
      "name": "main",
      "passes": [
        {
          "name": "orbit_camera",
          "type": "fullscreen",
          "output": {"color": "swapchain", "depth": null},
          "shader": {
            "vertex": "shaders/fullscreen",
            "fragment": "shaders/orbit_camera"
          },
          "push_constants": "projection_view"
        }
      ]
    }
  ]
})json");
}

void renderStemFullscreenFrame(RenderTarget &render_target) {
    GET_MODULE(Renderer).render();
    GET_MODULE(VulkanManageCore).waitIdle();
    (void)render_target;
}

void renderVatPlaybackFrame(RenderTarget &render_target, const std::filesystem::path &root) {
    const auto vat_path = root / "tiny_vat.glb";
    TestVatFixture::writeTinyVatGlb(vat_path);

    auto &path_resolver = GET_MODULE(PathResolver);
    if (!path_resolver.isSetup()) {
        path_resolver.setup(root, false);
    }

    auto &launch_config = GET_MODULE(EngineLaunchConfig);
    launch_config.play_vat = vat_path;
    launch_config.camera_override = EngineLaunchCameraOverride{
        .position = {0.0f, 0.0f, 2.0f},
        .target = {0.0f, 0.0f, 0.0f},
        .fov_y = 45.0f,
    };

    auto &engine_time = GET_MODULE(EngineTime);
    engine_time.setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    engine_time.setTime(1.0);

    (void)GET_MODULE(VatPlayer);
    GET_MODULE(Renderer).render();
    GET_MODULE(VulkanManageCore).waitIdle();
    (void)render_target;
}

void renderFeatureFrame(RenderTarget &render_target) {
    GET_MODULE(Renderer).render();
    GET_MODULE(VulkanManageCore).waitIdle();
    (void)render_target;
}

void renderShadowFrame(RenderTarget &render_target) {
    GET_MODULE(ECSPredefinedRegistration).reg();
    GET_MODULE(SceneLoader).load("default_scene");
    GET_MODULE(ECSCore).update();
    GET_MODULE(ECSCore).update();

    auto &camera = GET_MODULE(Camera);
    const glm::vec3 position{0.0f, 2.0f, -4.5f};
    const glm::vec3 target{0.0f, 0.25f, 0.0f};
    camera.setPos(position);
    camera.setDir(glm::normalize(target - position));
    camera.setUp({0.0f, 1.0f, 0.0f});

    GET_MODULE(Renderer).render();
    GET_MODULE(VulkanManageCore).waitIdle();
    (void)render_target;
}

void renderComputeFrame(RenderTarget &render_target) {
    GET_MODULE(Renderer).render();
    GET_MODULE(VulkanManageCore).waitIdle();
    (void)render_target;
}

void renderOrthographicCameraFrame(RenderTarget &render_target) {
    GET_MODULE(Renderer).render();
    GET_MODULE(VulkanManageCore).waitIdle();
    (void)render_target;
}

void renderOrbitCameraControllerFrame(RenderTarget &render_target) {
    auto &engine_time = GET_MODULE(EngineTime);
    engine_time.setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    engine_time.setTime(2.0);

    GET_MODULE(ECSPredefinedRegistration).reg();
    GET_MODULE(SceneLoader).load("default_scene");

    GameContext game_context;
    internal::updateRegisteredGameSystems(game_context);
    GET_MODULE(Renderer).render();
    GET_MODULE(VulkanManageCore).waitIdle();
    (void)render_target;
}

void renderDebugDrawFrame(RenderTarget &render_target) {
    auto &renderer = GET_MODULE(Renderer);
    auto &debug_draw = GET_MODULE(DebugDraw);
    debug_draw.line({-0.8125f, 0.0625f, 0.0f}, {0.8125f, 0.0625f, 0.0f},
                    {1.0f, 0.05f, 0.02f, 1.0f});
    debug_draw.line({0.0625f, -0.8125f, 0.0f}, {0.0625f, 0.8125f, 0.0f},
                    {0.10f, 0.95f, 0.25f, 1.0f});
    renderer.render();
    GET_MODULE(VulkanManageCore).waitIdle();
    (void)render_target;
}

void renderDebugTextFrame(RenderTarget &render_target) {
    auto &renderer = GET_MODULE(Renderer);
    auto &debug_text = GET_MODULE(DebugText);
    debug_text.text(-64, -64, "CLIP", {1.0f, 0.0f, 0.0f, 1.0f});

    GameContext context;
    context.debugText(0, 0, "WP");

    renderer.render();
    GET_MODULE(VulkanManageCore).waitIdle();
    (void)render_target;
}

void renderColliderDebugDrawFrame(RenderTarget &render_target) {
    GET_MODULE(ECSPredefinedRegistration).reg();
    GET_MODULE(SceneLoader).load("default_scene");
    GET_MODULE(Renderer).render();
    GET_MODULE(VulkanManageCore).waitIdle();
    (void)render_target;
}

bool isHdrGoldenMode(const std::string &mode) {
    return mode == "hdr_off" || mode == "hdr_on";
}

bool isShadowGoldenMode(const std::string &mode) {
    return mode == "shadow_off" || mode == "shadow_on";
}

void requireGoldenVulkanDevice() {
    FastModuleContainer modules;
    auto &launch_config = GET_MODULE(EngineLaunchConfig);
    launch_config.headless = true;
    launch_config.headless_extent = vk::Extent2D{goldenWidth, goldenHeight};

    try {
        GET_MODULE(VulkanManageCore).waitIdle();
    } catch (const std::exception &ex) {
        const std::string message = ex.what();
        if (message.find("No suitable Vulkan physical device found") != std::string::npos) {
            SKIP("Golden image rendering requires a Vulkan device: " << message);
        }
        throw;
    }
}

bool usesRenderer(const GoldenCase &golden_case) {
    return golden_case.mode != "clear" && golden_case.mode != "fullscreen" &&
           golden_case.mode != "triangle" && golden_case.mode != "surface_toon";
}

RenderedCase renderCase(const GoldenCase &golden_case) {
    FastModuleContainer modules;
    const auto temp_dir = makeTempProjectDir(golden_case.name);
    if (golden_case.mode == "stem_fullscreen") {
        writeStemProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeStemProjectJson().dump());
    } else if (golden_case.mode == "explicit_order") {
        writeExplicitOrderProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeFeatureProjectJson().dump());
    } else if (golden_case.mode == "vat_playback") {
        writeVatProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeVatProjectJson().dump());
    } else if (golden_case.mode == "feature_compose") {
        writeFeatureProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeFeatureProjectJson().dump());
    } else if (golden_case.mode == "debug_draw_feature") {
        writeDebugDrawProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeFeatureProjectJson().dump());
    } else if (golden_case.mode == "debug_text_feature") {
        writeDebugTextProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeFeatureProjectJson().dump());
    } else if (golden_case.mode == "collider_debug_draw") {
        writeColliderDebugDrawProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeFeatureProjectJson().dump());
    } else if (isHdrGoldenMode(golden_case.mode)) {
        writeHdrProject(temp_dir, golden_case.mode == "hdr_on");
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeHdrProjectJson().dump());
    } else if (isShadowGoldenMode(golden_case.mode)) {
        writeShadowProject(temp_dir, golden_case.mode == "shadow_on");
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeShadowProjectJson().dump());
    } else if (golden_case.mode == "compute_buffer") {
        writeComputeProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeComputeProjectJson().dump());
    } else if (golden_case.mode == "orthographic_camera") {
        writeOrthographicCameraProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeFeatureProjectJson().dump());
    } else if (golden_case.mode == "orbit_camera_controller") {
        writeOrbitCameraControllerProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeFeatureProjectJson().dump());
    } else {
        const auto scene_path = temp_dir / "scene.json";
        const auto asset_path = temp_dir / "assets.json";
        writeTextFile(scene_path, "{}");
        writeTextFile(asset_path, "{}");
        GET_MODULE(ProjectSource).setSourceByData(makeProjectConfig(scene_path, asset_path).dump());
    }

    auto &launch_config = GET_MODULE(EngineLaunchConfig);
    launch_config.headless = true;
    launch_config.shader_hot_reload = false;
    launch_config.headless_extent = vk::Extent2D{goldenWidth, goldenHeight};
    launch_config.headless_frames = 1;

    auto &render_target = GET_MODULE(RenderTarget);
    Renderer *renderer = nullptr;
    if (usesRenderer(golden_case)) {
        renderer = &GET_MODULE(Renderer);
        renderer->setExecutionTracingForTesting(true);
    }
    if (golden_case.mode == "clear") {
        renderClearFrame(render_target, vk::ClearColorValue{std::array{0.1f, 0.2f, 0.3f, 1.0f}});
    } else if (golden_case.mode == "fullscreen") {
        renderFullscreenFrame(render_target, solidFullscreenFragmentShader());
    } else if (golden_case.mode == "stem_fullscreen") {
        renderStemFullscreenFrame(render_target);
    } else if (golden_case.mode == "triangle") {
        renderFullscreenFrame(render_target, triangleMaskFragmentShader());
    } else if (golden_case.mode == "surface_toon") {
        renderSurfaceToonFrame(render_target);
    } else if (golden_case.mode == "explicit_order") {
        renderFeatureFrame(render_target);
    } else if (golden_case.mode == "vat_playback") {
        renderVatPlaybackFrame(render_target, temp_dir);
    } else if (golden_case.mode == "feature_compose") {
        renderFeatureFrame(render_target);
    } else if (golden_case.mode == "debug_draw_feature") {
        renderDebugDrawFrame(render_target);
    } else if (golden_case.mode == "debug_text_feature") {
        renderDebugTextFrame(render_target);
    } else if (golden_case.mode == "collider_debug_draw") {
        renderColliderDebugDrawFrame(render_target);
    } else if (isHdrGoldenMode(golden_case.mode)) {
        renderDebugTextFrame(render_target);
    } else if (isShadowGoldenMode(golden_case.mode)) {
        renderShadowFrame(render_target);
    } else if (golden_case.mode == "compute_buffer") {
        renderComputeFrame(render_target);
    } else if (golden_case.mode == "orthographic_camera") {
        renderOrthographicCameraFrame(render_target);
    } else if (golden_case.mode == "orbit_camera_controller") {
        renderOrbitCameraControllerFrame(render_target);
    } else {
        throw std::runtime_error("unknown golden case mode: " + golden_case.mode);
    }

    if (golden_case.mode == "explicit_order") {
        GET_MODULE(RenderTiming).flush();
    }
    const auto pixels = render_target.readbackLastFrameRGBA8();
    const auto device_properties = GET_MODULE(VulkanManageCore).getPhysDevice().getProperties();
    GET_MODULE(VulkanManageCore).waitIdle();
    std::filesystem::remove_all(temp_dir);

    return RenderedCase{
        RgbaImage{goldenWidth, goldenHeight, pixels},
        std::string{device_properties.deviceName.data()},
        renderer != nullptr ? renderer->lastExecutionTraceForTesting() : nlohmann::json{},
        renderer != nullptr ? renderer->currentFramePlanOrderForTesting() : std::vector<std::string>{},
        RenderTiming::__get().has_value() ? RenderTiming::__get()->lastFrameNodeNamesForTesting()
                                          : std::vector<std::string>{},
        RenderTiming::__get().has_value() ? RenderTiming::__get()->lastFrameQueryCountForTesting() : 0,
        RenderTiming::__get().has_value() && RenderTiming::__get()->allGpuQueriesCollectedForTesting(),
    };
}

CompareResult compareImages(const RgbaImage &expected, const RgbaImage &actual) {
    if (expected.width != actual.width || expected.height != actual.height ||
        expected.pixels.size() != actual.pixels.size()) {
        throw std::runtime_error("golden image dimensions do not match actual image");
    }

    CompareResult result;
    result.diff = RgbaImage{actual.width, actual.height, std::vector<uint8_t>(actual.pixels.size(), 255)};

    uint64_t total = 0;
    for (size_t i = 0; i < actual.pixels.size(); ++i) {
        const int delta = std::abs(static_cast<int>(expected.pixels[i]) - static_cast<int>(actual.pixels[i]));
        total += static_cast<uint64_t>(delta);
        result.max = std::max(result.max, delta);
        result.diff.pixels[i] = static_cast<uint8_t>(std::min(delta * 8, 255));
        if (i % 4 == 3) {
            result.diff.pixels[i] = 255;
        }
    }
    result.average = static_cast<double>(total) / static_cast<double>(actual.pixels.size());
    return result;
}

void writeFailureMetadata(const std::filesystem::path &path, const GoldenCase &golden_case,
                          const RenderedCase &rendered, const CompareResult &comparison,
                          const Tolerance &tolerance) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path};
    file << nlohmann::json{
                {"case", golden_case.name},
                {"device", rendered.device_name},
                {"average_diff", comparison.average},
                {"max_diff", comparison.max},
                {"tolerance", {{"average", tolerance.average}, {"max", tolerance.max}}},
            }.dump(2);
}

std::filesystem::path rendererTraceFixturePath() {
    return sourceRoot() / "test" / "fixtures" / "renderer_execution_traces.json";
}

std::filesystem::path rgba8HashFixturePath() {
    return sourceRoot() / "test" / "fixtures" / "wp73_rgba8_hashes.json";
}

std::filesystem::path canonicalFramePlanTraceFixturePath() {
    return sourceRoot() / "test" / "fixtures" / "canonical_frame_plan_trace.txt";
}

bool updateRendererTraceFixturesRequested() {
    const char *value = std::getenv("PELICAN_UPDATE_RENDERER_TRACE_FIXTURES");
    return value != nullptr && std::string{value} == "1";
}

bool updateRgba8HashFixturesRequested() {
    const char *value = std::getenv("PELICAN_UPDATE_RGBA8_HASH_FIXTURES");
    return value != nullptr && std::string{value} == "1";
}

nlohmann::json loadJsonFile(const std::filesystem::path &path) {
    std::ifstream file{path};
    if (!file) {
        throw std::runtime_error("failed to open JSON fixture: " + path.string());
    }
    return nlohmann::json::parse(file);
}

} // namespace

TEST_CASE("golden image cases match expected output", "[golden][headless]") {
    setupLogger();
    requireGoldenVulkanDevice();
    const auto cases = discoverGoldenCases();
#if PELICAN_WITH_VAT
    REQUIRE(cases.size() == 18);
#else
    REQUIRE(cases.size() == 17);
#endif

    for (const auto &golden_case : cases) {
        DYNAMIC_SECTION(golden_case.name) {
            const auto rendered = renderCase(golden_case);

            const auto expected_path = golden_case.root / "expected.png";
            if (updateGoldenRequested()) {
                writePng(expected_path, rendered.image);
                SUCCEED("updated golden image: " << expected_path.string());
                continue;
            }

            const auto expected = loadPng(expected_path);
            const auto tolerance = loadTolerance(golden_case.root / "tolerance.json");
            const auto comparison = compareImages(expected, rendered.image);

            const auto artifact_dir = binaryRoot() / "test_artifacts" / golden_case.name;
            if (writeColorDiffArtifactsRequested()) {
                writePng(artifact_dir / "actual.png", rendered.image);
                writePng(artifact_dir / "diff.png", comparison.diff);
                writeFailureMetadata(artifact_dir / "wp74_diff.json", golden_case, rendered,
                                     comparison, tolerance);
            }
            if (comparison.average > tolerance.average || comparison.max > tolerance.max) {
                writePng(artifact_dir / "actual.png", rendered.image);
                writePng(artifact_dir / "diff.png", comparison.diff);
                writeFailureMetadata(artifact_dir / "failure.json", golden_case, rendered, comparison, tolerance);
            }

            INFO("device=" << rendered.device_name);
            INFO("average diff=" << comparison.average << " max diff=" << comparison.max);
            REQUIRE(comparison.average <= tolerance.average);
            REQUIRE(comparison.max <= tolerance.max);
        }
    }
}

std::string loadTextFile(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        throw std::runtime_error("failed to open text fixture: " + path.string());
    }
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

TEST_CASE("golden final RGBA8 bytes match the WP74 C1b baseline hashes",
          "[golden][headless][byte-exact]") {
    setupLogger();
    requireGoldenVulkanDevice();
    const bool update_fixtures = updateRgba8HashFixturesRequested();
    const auto fixture_path = rgba8HashFixturePath();
    const auto expected = update_fixtures ? nlohmann::json::object() : loadJsonFile(fixture_path);
    nlohmann::json captured = nlohmann::json::object();

    for (const auto &golden_case : discoverGoldenCases()) {
        CAPTURE(golden_case.name);
        const auto rendered = renderCase(golden_case);
        const auto hash = picosha2::hash256_hex_string(rendered.image.pixels.begin(),
                                                       rendered.image.pixels.end());
        captured[golden_case.name] = hash;
        if (!update_fixtures) {
            REQUIRE(hash == expected.at(golden_case.name).get<std::string>());
        }
    }

    if (update_fixtures) {
        writeTextFile(fixture_path, captured.dump(2) + "\n");
    } else {
        REQUIRE(captured.size() == expected.size());
    }
}

TEST_CASE("Renderer execution matches plan order and captured traces", "[golden][headless][framegraph]") {
    setupLogger();
    requireGoldenVulkanDevice();
    const bool update_fixtures = updateRendererTraceFixturesRequested();
    const auto fixture_path = rendererTraceFixturePath();
    const auto expected = update_fixtures ? nlohmann::json::object() : loadJsonFile(fixture_path);
    const auto plan_trace_path = canonicalFramePlanTraceFixturePath();
    const auto expected_plan_trace = update_fixtures ? std::string{} : loadTextFile(plan_trace_path);
    nlohmann::json captured = nlohmann::json::object();
    std::string captured_plan_trace;

    for (const auto &golden_case : discoverGoldenCases()) {
        if (!usesRenderer(golden_case)) {
            continue;
        }
        CAPTURE(golden_case.name);
        const auto rendered = renderCase(golden_case);
        std::vector<std::string> executed_order;
        std::string plan_line;
        for (const auto &node : rendered.execution_trace.at("nodes")) {
            executed_order.push_back(node.at("name").get<std::string>());
            if (!plan_line.empty()) {
                plan_line += " -> ";
            }
            plan_line += node.at("name").get<std::string>() + "[" +
                         node.at("kind").get<std::string>() + "]";
        }
        captured_plan_trace += golden_case.name + ": " + plan_line + "\n";

        REQUIRE(executed_order == rendered.plan_order);
        if (golden_case.mode == "explicit_order") {
            REQUIRE(rendered.gpu_timing_node_names == rendered.plan_order);
            REQUIRE(rendered.gpu_timing_query_count == rendered.plan_order.size() * 2);
            REQUIRE(rendered.gpu_timing_queries_collected);
        }
        captured[golden_case.name] = rendered.execution_trace;
        if (!update_fixtures) {
            REQUIRE(rendered.execution_trace == expected.at(golden_case.name));
        }
    }

    if (update_fixtures) {
        writeTextFile(fixture_path, captured.dump(2) + "\n");
        writeTextFile(plan_trace_path, captured_plan_trace);
    } else {
        REQUIRE(captured.size() == expected.size());
        REQUIRE(captured_plan_trace == expected_plan_trace);
    }
}

TEST_CASE("fullscreen inputs rebind after shader reload and render-target recreation",
          "[golden][headless][framegraph][rebind]") {
    setupLogger();
    requireGoldenVulkanDevice();

    FastModuleContainer modules;
    const auto temp_dir = makeTempProjectDir("fullscreen_rebind");
    writeHdrProject(temp_dir, false);
    writeTextFile(temp_dir / "passes" / "main.json", makeFullscreenRebindRenderingConfig().dump(2));
    GET_MODULE(PathResolver).setup(temp_dir, false);
    GET_MODULE(ProjectSource).setProjectData(makeHdrProjectJson().dump());

    auto &launch_config = GET_MODULE(EngineLaunchConfig);
    launch_config.headless = true;
    launch_config.shader_hot_reload = true;
    launch_config.headless_extent = vk::Extent2D{goldenWidth, goldenHeight};

    auto &renderer = GET_MODULE(Renderer);
    auto &pass_container = GET_MODULE(RenderingPassContainer);
    std::optional<PassId> input_pass;
    for (const auto rendering_pass_id : pass_container.getRegisteredPassIds()) {
        for (const auto &pass : pass_container.getCompiledRenderingPass(rendering_pass_id).passes) {
            if (pass.definition.isFullscreen() && !pass.definition.input_targets.empty()) {
                input_pass = pass.pass_id;
                break;
            }
        }
    }
    REQUIRE(input_pass.has_value());

    auto &fullscreen_passes = GET_MODULE(FullscreenPassContainer);
    const auto initial_views = fullscreen_passes.boundInputImageViewsForTesting(*input_pass);
    const auto initial_revision = fullscreen_passes.inputBindingRevisionForTesting(*input_pass);
    REQUIRE(initial_views.size() == 1);
    REQUIRE(initial_revision > 0);

    const auto shader_path = temp_dir / "shaders" / "copy_input.frag";
    const auto previous_write_time = std::filesystem::last_write_time(shader_path);
    writeTextFile(shader_path, std::string{copyInputFragmentShader()} + "\n// hot reload rebind probe\n");
    std::filesystem::last_write_time(shader_path, previous_write_time + std::chrono::seconds{2});

    renderer.render();
    GET_MODULE(VulkanManageCore).waitIdle();
    const auto hot_reload_revision = fullscreen_passes.inputBindingRevisionForTesting(*input_pass);
    REQUIRE(hot_reload_revision > initial_revision);
    REQUIRE(fullscreen_passes.boundInputImageViewsForTesting(*input_pass) == initial_views);

    auto &render_targets = GET_MODULE(RenderTargetContainer);
    const auto lit_color = render_targets.getRenderTargetIdByName("lit_color");
    const auto old_view = render_targets.getImageView(lit_color);
    renderer.recreateRenderTargetsAndRebindForTesting(vk::Extent2D{goldenWidth, goldenHeight});
    const auto new_view = render_targets.getImageView(lit_color);

    REQUIRE(new_view != old_view);
    REQUIRE(fullscreen_passes.inputBindingRevisionForTesting(*input_pass) > hot_reload_revision);
    REQUIRE(fullscreen_passes.boundInputImageViewsForTesting(*input_pass) ==
            std::vector<vk::ImageView>{new_view});

    renderer.render();
    GET_MODULE(VulkanManageCore).waitIdle();
    std::filesystem::remove_all(temp_dir);
}

} // namespace Pelican
