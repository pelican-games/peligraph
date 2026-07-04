#include "../src/core/container.hpp"
#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/playback/vatplayer.hpp"
#include "../src/core/renderer/debugdraw.hpp"
#include "../src/core/shader/pipelinefactory.hpp"
#include "../src/core/shader/shadercompiler.hpp"
#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/core/vkcore/rendertarget.hpp"
#include "vat_fixture.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
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
        cases.push_back(GoldenCase{
            entry.path().filename().string(),
            config.at("mode").get<std::string>(),
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
             {"camera", {{"fov_y", 60.0}, {"near", 0.1}, {"far", 100.0}, {"up", {0, 1, 0}}}},
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
             {"camera", {{"fov_y", 45.0}, {"near", 0.1}, {"far", 1000.0}, {"up", {0, 1, 0}}}},
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
             {"camera", {{"fov_y", 45.0}, {"near", 0.1}, {"far", 100.0}, {"up", {0, 1, 0}}}},
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

void renderFullscreenFrame(RenderTarget &render_target, const std::string &fragment_shader_source) {
    auto &library = GET_MODULE(ShaderLibrary);
    const auto vert = compileShaderToBundle(library, fullscreenVertexShader(), vk::ShaderStageFlagBits::eVertex,
                                            "golden_fullscreen.vert");
    const auto frag = compileShaderToBundle(library, fragment_shader_source, vk::ShaderStageFlagBits::eFragment,
                                            "golden_fullscreen.frag");

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

void writeStemProject(const std::filesystem::path &root) {
    writeTextFile(root / "project.json", makeStemProjectJson().dump(2));
    writeTextFile(root / "scene.json", "{}");
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
      "insert": "after:present",
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

RenderedCase renderCase(const GoldenCase &golden_case) {
    FastModuleContainer modules;
    const auto temp_dir = makeTempProjectDir(golden_case.name);
    if (golden_case.mode == "stem_fullscreen") {
        writeStemProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeStemProjectJson().dump());
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
    if (golden_case.mode == "clear") {
        renderClearFrame(render_target, vk::ClearColorValue{std::array{0.1f, 0.2f, 0.3f, 1.0f}});
    } else if (golden_case.mode == "fullscreen") {
        renderFullscreenFrame(render_target, solidFullscreenFragmentShader());
    } else if (golden_case.mode == "stem_fullscreen") {
        renderStemFullscreenFrame(render_target);
    } else if (golden_case.mode == "triangle") {
        renderFullscreenFrame(render_target, triangleMaskFragmentShader());
    } else if (golden_case.mode == "vat_playback") {
        renderVatPlaybackFrame(render_target, temp_dir);
    } else if (golden_case.mode == "feature_compose") {
        renderFeatureFrame(render_target);
    } else if (golden_case.mode == "debug_draw_feature") {
        renderDebugDrawFrame(render_target);
    } else {
        throw std::runtime_error("unknown golden case mode: " + golden_case.mode);
    }

    const auto pixels = render_target.readbackLastFrameRGBA8();
    const auto device_properties = GET_MODULE(VulkanManageCore).getPhysDevice().getProperties();
    GET_MODULE(VulkanManageCore).waitIdle();
    std::filesystem::remove_all(temp_dir);

    return RenderedCase{
        RgbaImage{goldenWidth, goldenHeight, pixels},
        std::string{device_properties.deviceName.data()},
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

} // namespace

TEST_CASE("golden image cases match expected output", "[golden][headless]") {
    setupLogger();
    const auto cases = discoverGoldenCases();
    REQUIRE(cases.size() == 7);

    for (const auto &golden_case : cases) {
        DYNAMIC_SECTION(golden_case.name) {
            RenderedCase rendered;
            try {
                rendered = renderCase(golden_case);
            } catch (const std::exception &ex) {
                SKIP(std::string{"Golden image rendering unavailable: "} + ex.what());
            }

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

} // namespace Pelican
