#include "../src/core/container.hpp"
#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/animation/animationservice.hpp"
#include "../src/core/animation/vrmapplication.hpp"
#include "../src/core/asset/model.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/fullscreenpass/fullscreenpasscontainer.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/log.hpp"
#include "../src/core/material/materialcontainer.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/gltf.hpp"
#include "../src/core/openxr/openxrmirrorsink.hpp"
#include "../src/core/playback/vatplayer.hpp"
#include "../src/core/renderer/debugdraw.hpp"
#include "../src/core/renderer/debugtext.hpp"
#include "../src/core/renderer/camera.hpp"
#include "../src/core/renderer/frameresources.hpp"
#include "../src/core/renderer/atlasassetresource.hpp"
#include "../src/core/renderer/spriterenderer.hpp"
#include "../src/core/renderer/spritescene.hpp"
#include "../src/core/renderer/polygoninstancecontainer.hpp"
#include "../src/core/renderer/uicontainer.hpp"
#include "../src/core/renderer/uirenderer.hpp"
#include "../src/core/renderingpass/computetask.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/renderingpass/renderingpasscontainer.hpp"
#include "../src/core/renderingpass/rendertargetcontainer.hpp"
#include "../src/core/shader/pipelinefactory.hpp"
#include "../src/core/shader/shadercompiler.hpp"
#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/shader/surfacecompiler.hpp"
#include "../src/core/userpublic/details/system/registerer.hpp"
#include "../src/core/userpublic/gamecontext.hpp"
#include "../src/core/ui/module.hpp"
#include "../src/core/watch/reloadservice.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/deletionqueue.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/core/vkcore/rendertarget.hpp"
#include "../src/core/vkcore/rendertiming.hpp"
#include "../src/core/vkcore/util.hpp"
#include "../src/project/materialformat.hpp"
#include "../src/project/materiallowering.hpp"
#include "../src/project/importmanifest.hpp"
#include "../src/project/sceneformat.hpp"
#include "skeletal_fixture.hpp"
#include "material_absolute_override_fixture.hpp"
#include "morph_fixture.hpp"
#include "synthetic_stereo_target.hpp"
#include "vat_fixture.hpp"
#include "vrm_xr_demo_fixture.hpp"
#include "golden_harness.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <picosha2.h>
#include <set>
#include <sstream>
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
    std::uint32_t width = goldenWidth;
    std::uint32_t height = goldenHeight;
};

struct RenderedCase {
    RgbaImage image;
    std::string device_name;
    nlohmann::json execution_trace;
    std::vector<std::string> plan_order;
    std::vector<std::string> gpu_timing_node_names;
    uint32_t gpu_timing_query_count = 0;
    bool gpu_timing_queries_collected = false;
    nlohmann::json gpu_timing_status;
    std::uint64_t gpu_timing_pool_create_count = 0;
    bool ui_module_created = false;
    bool ui_gpu_created = false;
    std::size_t ui_parser_invocations = 0;
    bool atlas_created = false;
    bool sprite_scene_created = false;
    bool sprite_gpu_created = false;
    std::size_t atlas_page_count = 0;
    nlohmann::json sprite_status;
    std::optional<std::uint32_t>
        gpu_visible_draw_count;
    std::optional<
        FrameGraphResourceContainer::
            HostBufferPopulation>
        gpu_draw_command_population;
    std::optional<
        FrameGraphResourceContainer::
            HostBufferPopulation>
        gpu_draw_bounds_population;
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

std::vector<GoldenCase> loadGoldenInventoryCases() {
    const auto root = sourceRoot() / "test/golden";
    const auto inventory_path = root / "inventory.json";
    std::ifstream inventory_file{inventory_path};
    if (!inventory_file) {
        throw std::runtime_error("failed to open golden inventory: " +
                                 inventory_path.string());
    }
    const auto inventory = nlohmann::json::parse(inventory_file);
    std::vector<GoldenCase> cases;
    for (const auto &inventory_case : inventory.at("cases")) {
        const auto name = inventory_case.at("name").get<std::string>();
        const auto vat_condition = inventory_case.at("vat").get<std::string>();
        if (vat_condition != "on_and_off" && vat_condition != "on_only") {
            throw std::runtime_error("unknown VAT condition in golden inventory for " + name +
                                     ": " + vat_condition);
        }
#if !PELICAN_WITH_VAT
        if (vat_condition == "on_only") {
            continue;
        }
#endif
        const auto case_root = root / name;
        const auto config_path = case_root / "case.json";
        std::ifstream file{config_path};
        if (!file) {
            throw std::runtime_error("failed to open golden case config: " +
                                     config_path.string());
        }
        const auto config = nlohmann::json::parse(file);
        const auto mode = config.at("mode").get<std::string>();
        if (mode != inventory_case.at("mode").get<std::string>()) {
            throw std::runtime_error("golden case mode does not match inventory: " + name);
        }
        const auto width = config.value("width", goldenWidth);
        const auto height = config.value("height", goldenHeight);
        if (width == 0 || height == 0) {
            throw std::runtime_error("golden case extent must be non-zero: " +
                                     case_root.string());
        }
        cases.push_back(GoldenCase{
            name,
            mode,
            case_root,
            width,
            height,
        });
    }
    std::sort(cases.begin(), cases.end(), [](const GoldenCase &lhs, const GoldenCase &rhs) {
        return lhs.name < rhs.name;
    });
    return cases;
}

std::filesystem::path makeTempProjectDir(const std::string &case_name) {
    static std::uint64_t serial = 0;
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path() /
               ("pelican_golden_" + case_name + "_" + std::to_string(nonce) + "_" +
                std::to_string(++serial));
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
    auto begun = render_target.beginFrame(nullptr);
    REQUIRE(begun.frame.has_value());
    auto token = std::move(*begun.frame);
    const auto frame = token.context();

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
    render_target.submit(std::move(token));
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

const char *computeHistoryImageShader() {
    return R"glsl(
#version 450
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(rgba8, set = 1, binding = 0) uniform readonly image2D history_input;
layout(rgba8, set = 1, binding = 1) uniform writeonly image2D current_output;
layout(std430, set = 1, binding = 2) buffer HistoryProbe {
    vec4 value;
} history_probe;
void main() {
    vec4 value = imageLoad(history_input, ivec2(0));
    imageStore(current_output, ivec2(0), value);
    history_probe.value = value;
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

    auto begun = render_target.beginFrame(nullptr);
    REQUIRE(begun.frame.has_value());
    auto token = std::move(*begun.frame);
    const auto frame = token.context();

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

    render_target.submit(std::move(token));
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

const char *openPbrGoldenFragmentTemplate() {
    return R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : enable
#extension GL_GOOGLE_cpp_style_line_directive : enable
#include "pelican_surface_v1.glsl"

float pelican_param_base_weight() { return 1.0; }
vec4 pelican_param_base_color() { return vec4(0.12, 0.32, 0.72, 1.0); }
float pelican_param_base_diffuse_roughness() { return 0.18; }
float pelican_param_base_metalness() { return 0.08; }
float pelican_param_specular_weight() { return 1.0; }
vec4 pelican_param_specular_color() { return vec4(1.0, 0.96, 0.9, 1.0); }
float pelican_param_specular_roughness() { return 0.26; }
float pelican_param_specular_ior() { return 1.5; }
float pelican_param_coat_weight() { return 0.9; }
vec4 pelican_param_coat_color() { return vec4(1.0); }
float pelican_param_coat_roughness() { return 0.08; }
float pelican_param_coat_ior() { return 1.6; }
float pelican_param_coat_darkening() { return 0.9; }
float pelican_param_emission_luminance() { return 0.015; }
vec4 pelican_param_emission_color() { return vec4(0.12, 0.2, 0.5, 1.0); }
float pelican_param_geometry_opacity() { return 1.0; }
float pelican_param_geometry_normal_scale() { return 1.0; }
float pelican_param_geometry_coat_normal_scale() { return 1.0; }
float pelican_param_alpha_cutoff() { return 0.5; }

vec4 pelican_sample_base_weight_map(vec2 uv) { return vec4(1.0); }
vec4 pelican_sample_base_color_map(vec2 uv) { return vec4(1.0); }
vec4 pelican_sample_base_diffuse_roughness_map(vec2 uv) { return vec4(1.0); }
vec4 pelican_sample_base_metalness_map(vec2 uv) { return vec4(1.0); }
vec4 pelican_sample_specular_weight_map(vec2 uv) { return vec4(1.0); }
vec4 pelican_sample_specular_color_map(vec2 uv) { return vec4(1.0); }
vec4 pelican_sample_specular_roughness_map(vec2 uv) { return vec4(1.0); }
vec4 pelican_sample_specular_ior_map(vec2 uv) { return vec4(1.0); }
vec4 pelican_sample_coat_weight_map(vec2 uv) { return vec4(1.0); }
vec4 pelican_sample_coat_color_map(vec2 uv) { return vec4(1.0); }
vec4 pelican_sample_coat_roughness_map(vec2 uv) { return vec4(1.0); }
vec4 pelican_sample_coat_ior_map(vec2 uv) { return vec4(1.0); }
vec4 pelican_sample_coat_darkening_map(vec2 uv) { return vec4(1.0); }
vec4 pelican_sample_emission_luminance_map(vec2 uv) { return vec4(1.0); }
vec4 pelican_sample_emission_color_map(vec2 uv) { return vec4(1.0); }
vec4 pelican_sample_geometry_opacity_map(vec2 uv) { return vec4(1.0); }
vec4 pelican_sample_geometry_normal_map(vec2 uv) { return vec4(0.5, 0.5, 1.0, 1.0); }
vec4 pelican_sample_geometry_coat_normal_map(vec2 uv) { return vec4(0.5, 0.5, 1.0, 1.0); }

uint pelican_light_count() { return 2u; }
PelicanLightV1 pelican_light(uint index, vec3 world_position) {
    PelicanLightV1 light;
    if (index == 0u) {
        light.direction = normalize(vec3(-0.45, 0.62, 1.0));
        light.radiance = vec3(6.0, 5.4, 4.8);
    } else {
        light.direction = normalize(vec3(0.75, -0.2, 0.55));
        light.radiance = vec3(0.8, 1.1, 1.8);
    }
    light.attenuation = 1.0;
    return light;
}
float pelican_shadow(uint light_index, vec3 world_position) { return 1.0; }
vec3 pelican_env_ambient(vec3 normal) {
    float sky = clamp(normal.y * 0.5 + 0.5, 0.0, 1.0);
    return mix(vec3(0.012, 0.016, 0.028), vec3(0.09, 0.12, 0.2), sky);
}

#include "__pelican_user_surface.glsl"

layout(location = 0) out vec4 outColor;
void main() {
    vec2 pixel = (gl_FragCoord.xy / vec2(64.0)) * 2.0 - 1.0;
    pixel.x *= 1.08;
    float radius2 = dot(pixel, pixel);
    if (radius2 > 0.82) {
        outColor = vec4(0.008, 0.011, 0.02, 1.0);
        return;
    }
    vec3 sphere_normal = normalize(vec3(pixel, sqrt(max(0.82 - radius2, 0.0))));
    PelicanSurfaceInputV1 input_data;
    input_data.uv = pixel * 0.5 + 0.5;
    input_data.vertex_color = vec4(1.0);
    input_data.world_position = sphere_normal;
    input_data.normal = sphere_normal;
    input_data.view_direction = normalize(vec3(0.0, 0.0, 3.2) - sphere_normal);
    input_data.custom0 = vec4(0.0);
    input_data.custom1 = vec4(0.0);
    PelicanSurfaceV1 surface;
    surface.base_color = vec4(1.0);
    surface.normal = sphere_normal;
    surface.metallic = 0.0;
    surface.roughness = 1.0;
    surface.occlusion = 1.0;
    surface.emissive = vec3(0.0);
    pelican_surface_v1(input_data, surface);
    vec3 lit = pelican_lighting_v1(surface, input_data);
    vec3 display_linear = lit / (lit + vec3(1.0));
    outColor = vec4(display_linear, surface.base_color.a);
}
)glsl";
}

void renderOpenPbrCoatSphereFrame(RenderTarget &render_target) {
#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto reference = std::string{"engine://surfaces/openpbr/opaque_double.surface"};
    const auto surface = parseSurfaceFormat(
        engineResourceOrThrow("surfaces/openpbr/opaque_double.surface"), reference);
    const auto composition = composeSurfaceShaders(surface, reference);
    ShaderCompiler compiler;
    ShaderCompileOptions options;
    options.virtual_includes = composition.virtual_includes;
    const auto result = compiler.compileSource(openPbrGoldenFragmentTemplate(),
                                               vk::ShaderStageFlagBits::eFragment,
                                               "engine://golden/openpbr_coat_sphere.frag",
                                               options);
    if (!result.ok) throw std::runtime_error("OpenPBR coat golden compile failed: " + result.log);
    auto &library = GET_MODULE(ShaderLibrary);
    renderFullscreenFrameWithFragment(
        render_target, library.loadFromSpirv(result.spirv, "openpbr_coat_sphere.frag"));
#else
    (void)render_target;
    throw std::runtime_error("runtime shader compiler disabled");
#endif
}

void renderUsdOpenPbrMaterialFrame(RenderTarget &render_target,
                                   const std::filesystem::path &delivery) {
    const auto readJson = [](const std::filesystem::path &path) {
        std::ifstream input{path, std::ios::binary};
        if (!input.is_open()) throw std::runtime_error("failed to open WP124 fixture: " + path.string());
        return nlohmann::json::parse(input);
    };
    const auto manifest = parseImportManifestJson(readJson(delivery / "manifest.json"));
    if (manifest.outputs.size() != 4)
        throw std::runtime_error("WP124 manifest must contain material, GLB, PNG, and scene outputs");
    const auto material_json = readJson(delivery / "materials.json");
    const auto reference = std::string{"engine://surfaces/openpbr/opaque_double.surface"};
    const auto surface = parseSurfaceFormat(
        engineResourceOrThrow("surfaces/openpbr/opaque_double.surface"), reference);
    MaterialSurfaceCatalog catalog;
    catalog.emplace(reference, surface);
    const auto material = parseMaterialFormatJson(material_json, catalog);
    if (material.materials.size() != 1)
        throw std::runtime_error("WP124 coat fixture must contain one material");
    const auto lowered = lowerMaterial(material.materials.front(), surface);
    if (lowered.target_pass != "forward_opaque" || !lowered.routing ||
        !lowered.routing->double_sided || lowered.textures.size() != 18)
        throw std::runtime_error("WP124 generated material did not lower through OpenPBR opaque-double");
    const auto &values = material_json.at("materials").at(0).at("values");
    if (std::abs(values.at("coat_weight").get<double>() - 0.9) > 1.0e-5 ||
        std::abs(values.at("coat_ior").get<double>() - 1.6) > 1.0e-5)
        throw std::runtime_error("WP124 generated coat values do not match the golden contract");
    const auto texture_reference = material_json.at("materials").at(0).at("textures")
                                       .at("base_diffuse_roughness_map").get<std::string>();
    constexpr std::string_view project_prefix = "project://";
    if (!texture_reference.starts_with(project_prefix) ||
        !std::filesystem::is_regular_file(delivery / texture_reference.substr(project_prefix.size())))
        throw std::runtime_error("WP124 localized texture output is missing");

    const auto binding =
        parsePrimitiveMaterialBindingJson(readJson(delivery / "material_bindings.json"));
    if (binding.bindings.size() != 1 || binding.bindings.front().material != lowered.name)
        throw std::runtime_error("WP124 generated binding does not select the lowered material");
    const auto scene = normalizeSceneDataJson(readJson(delivery / "scene.json"));
    if (scene.scenes.at("default_scene").at("objects").size() != 2)
        throw std::runtime_error("WP124 generated scene plus golden key light did not load");

#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto bundles = GET_MODULE(ShaderLibrary).loadFromSurface(
        surface, reference, SurfacePass::main, lowered.defines);
    auto &standard = GET_MODULE(StandardMaterialResource);
    MaterialInfo info{
        .vert_shader = bundles.vertex,
        .frag_shader = bundles.fragment,
        .base_color_texture = standard.whiteTexture(),
        .metallic_roughness_texture = standard.metallicRoughnessDefaultTexture(),
        .normal_texture = standard.normalDefaultTexture(),
        .emissive_texture = standard.emissiveDefaultTexture(),
    };
    applyLoweredMaterial(info, lowered);
    auto &materials = GET_MODULE(MaterialContainer);
    for (std::size_t index = 0; index < lowered.textures.size(); ++index) {
        const auto &texture = lowered.textures[index];
        if (!texture.reference.starts_with(project_prefix)) continue;
        const auto path = delivery / texture.reference.substr(project_prefix.size());
        if (!std::filesystem::is_regular_file(path))
            throw std::runtime_error("WP124 lowered texture is missing: " + path.string());
        info.custom_textures[index].texture = materials.registerTextureFile(path);
    }
    const auto material_id = materials.registerMaterial(std::move(info));

    auto &model = GET_MODULE(ModelAssetContainer).getModelTemplateByName("coat");
    if (model.material_primitives.size() != 1 ||
        model.material_primitives.front().primitives.size() != 1)
        throw std::runtime_error("WP124 generated GLB/binding did not resolve one primitive");
    for (auto &group : model.material_primitives) group.material = material_id;

    GET_MODULE(ECSPredefinedRegistration).reg();
    GET_MODULE(SceneLoader).load("default_scene");
    GET_MODULE(ECSCore).update();
    GET_MODULE(ECSCore).update();
    auto &camera = GET_MODULE(Camera);
    camera.setPos({0.0f, 0.0f, 2.4f});
    camera.setDir({0.0f, 0.0f, -1.0f});
    camera.setUp({0.0f, 1.0f, 0.0f});
    GET_MODULE(Renderer).render();
    GET_MODULE(VulkanManageCore).waitIdle();
    (void)render_target;
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
    writeTextFile(root / "ui" / "ui.json", R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
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

void writeSnapshotRefractionProject(const std::filesystem::path &root) {
    auto project = makeStemProjectJson();
    project["name"] = "snapshot refraction golden";
    writeTextFile(root / "project.json", project.dump(2));
    writeTextFile(root / "scene.json", R"json({
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {"default_scene": {"objects": []}}
})json");
    writeTextFile(root / "assets.json", R"json({"models":[]})json");
    writeTextFile(root / "ui" / "ui.json", R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
    writeTextFile(root / "shaders" / "fullscreen.vert", stemFullscreenVertexShader());
    writeTextFile(root / "shaders" / "opaque.frag", R"glsl(
#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(0.08 + uv.x * 0.72, 0.10 + uv.y * 0.55, 0.24, 1.0);
}
)glsl");
    writeTextFile(root / "shaders" / "refract.frag", R"glsl(
#version 450
layout(set = 1, binding = 0) uniform sampler2D pelican_screen_opaque_color_texture;
vec4 pelican_screen_opaque_color(vec2 uv) {
    return texture(pelican_screen_opaque_color_texture, uv);
}
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main() {
    vec2 bent_uv = uv + vec2(0.125 * (uv.y - 0.5), 0.0);
    vec3 behind = pelican_screen_opaque_color(bent_uv).rgb;
    outColor = vec4(mix(behind, vec3(0.05, 0.35, 0.42), 0.22), 1.0);
}
)glsl");
    writeTextFile(root / "passes" / "main.json", R"json({
  "snapshots": [{"name": "opaque_color", "after": "opaque"}],
  "render_targets": [],
  "rendering_passes": [{
    "name": "main",
    "passes": [
      {
        "name": "opaque",
        "type": "fullscreen",
        "output": {"color": "swapchain", "depth": null},
        "shader": {"vertex": "shaders/fullscreen", "fragment": "shaders/opaque"}
      },
      {
        "name": "refract",
        "type": "fullscreen",
        "canonical_anchor": "post_ldr",
        "input": ["opaque_color"],
        "output": {"color": "swapchain", "depth": null},
        "shader": {"vertex": "shaders/fullscreen", "fragment": "shaders/refract"}
      }
    ]
  }]
})json");
}

void writeExplicitOrderProject(const std::filesystem::path &root,
                               bool gpu_timing = true) {
    writeTextFile(root / "project.json", makeFeatureProjectJson().dump(2));
    writeTextFile(root / "scene.json", R"json({
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {"default_scene": {"objects": []}}
})json");
    writeTextFile(root / "assets.json", R"json({"models":[]})json");
    writeTextFile(root / "ui" / "ui.json", R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
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
    if (!gpu_timing) {
        std::ifstream stream{root / "passes" / "main.json"};
        auto config = nlohmann::json::parse(stream);
        stream.close();
        config.erase("features");
        writeTextFile(root / "passes" / "main.json", config.dump(2));
    }
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
    writeTextFile(root / "ui" / "ui_overlay.json", R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");

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
    writeTextFile(root / "ui" / "ui.json", R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
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

void writeLogicalFrameStereoProject(const std::filesystem::path &root) {
    auto project = makeFeatureProjectJson();
    project["name"] = "logical frame stereo fixture";
    writeTextFile(root / "project.json", project.dump(2));
    writeTextFile(root / "scene.json", R"json({
      "schema":"pelican.scene","version":1,
      "scenes":{"default_scene":{"objects":[{
        "name":"StereoHistoryObject","components":[
          {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
          {"name":"simplemodelview","model":"ground"}
        ]
      }]}}
    })json");
    writeTextFile(root / "assets.json",
                  R"json({"models":[{"name":"ground","path":"assets/ground.glb"}]})json");
    writeTextFile(root / "ui" / "ui.json",
                  R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
    writeTextFile(root / "shaders" / "fullscreen.vert", stemFullscreenVertexShader());
    writeTextFile(root / "shaders" / "stereo_probe.frag", R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_frame.glsl"
layout(location = 0) out vec4 outColor;

float encodeSigned(float value) { return clamp(value * 0.2 + 0.5, 0.0, 1.0); }

void main() {
    int band = int(floor(gl_FragCoord.x));
    float signal = 0.0;
    if (band == 0) signal = encodeSigned(pelicanFrame.camera_position.x);
    else if (band == 1) signal = encodeSigned(pelicanFrame.view[3][0]);
    else if (band == 2) signal = encodeSigned(pelicanFrame.projection[0][0]);
    else if (band == 3) signal = encodeSigned(pelicanFrame.previous_view[3][0]);
    else if (band == 4) signal = float(pelicanFrame.frame_index.x) / 16.0;
    else if (band == 5) signal = float(pelicanFrame.temporal_reset_epoch) / 16.0;
    else if (band == 6) {
        signal = encodeSigned(pelicanObjects.objects[0].model[3].x -
                              pelicanPreviousObjects.objects[0].model[3].x);
    } else if (band == 7) {
        signal = encodeSigned(pelicanFrame.previous_projection[0][0]);
    }
    outColor = vec4(signal, signal, signal, 1.0);
}
)glsl");
    writeTextFile(root / "passes" / "main.json", R"json({
      "xr":{"view_execution":"sequential"},
      "render_targets":[],
      "rendering_passes":[{"name":"main","passes":[{
        "name":"stereo_probe","type":"fullscreen",
        "output":{"color":"swapchain","depth":null},
        "shader":{"vertex":"shaders/fullscreen","fragment":"shaders/stereo_probe"}
      }]}]
    })json");
    std::filesystem::create_directories(root / "assets");
    std::filesystem::copy_file(sourceRoot() / "test" / "fixtures" / "ground.glb",
                               root / "assets" / "ground.glb",
                               std::filesystem::copy_options::overwrite_existing);
}

void writeTemporalAccumulationProject(const std::filesystem::path &root) {
    auto project = makeFeatureProjectJson();
    project["name"] = "temporal accumulation golden";
    writeTextFile(root / "project.json", project.dump(2));
    writeTextFile(root / "scene.json", R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
    writeTextFile(root / "assets.json", R"json({"models":[]})json");
    writeTextFile(root / "ui" / "ui.json", R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
    writeTextFile(root / "shaders" / "fullscreen.vert", stemFullscreenVertexShader());
    writeTextFile(root / "shaders" / "current.frag", R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_frame.glsl"
layout(location = 0) out vec4 outColor;
void main() {
    float frame = float(pelicanFrame.frame_index.x);
    outColor = vec4(frame * 0.25, 0.20, 0.05, 1.0);
}

)glsl");
    writeTextFile(root / "shaders" / "accumulate.frag", R"glsl(
#version 450
layout(set = 1, binding = 0) uniform sampler2D currentTexture;
layout(set = 1, binding = 1) uniform sampler2D historyTexture;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main() { outColor = mix(texture(currentTexture, uv), texture(historyTexture, uv), 0.5); }
)glsl");
    writeTextFile(root / "shaders" / "present.frag", R"glsl(
#version 450
layout(set = 1, binding = 0) uniform sampler2D accumulatedTexture;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main() { outColor = texture(accumulatedTexture, uv); }
)glsl");
    writeTextFile(root / "features" / "accumulation.json", R"json({
  "schema":"pelican.render_feature","version":1,"name":"accumulation_fixture",
  "render_targets":[{
    "name":"temporal_accum","extent_scale":1.0,"format":"R16G16B16A16_SFLOAT",
    "format_class":"explicit(R16G16B16A16_SFLOAT)","role":"color",
    "usage":["COLOR_ATTACHMENT","SAMPLED"],"history":true,
    "clear_color":[0.0,0.0,0.0,1.0]
  }],
  "passes":[{"insert":"after:post_main","pass":{
    "name":"temporal_accumulate","type":"fullscreen",
    "input":["current_color","temporal_accum@history"],
    "output":{"color":"temporal_accum","depth":null},
    "shader":{"vertex":"shaders/fullscreen","fragment":"shaders/accumulate"}
  }}]
})json");
    writeTextFile(root / "passes" / "main.json", R"json({
  "features":["features/accumulation.json"],
  "render_targets":[{
    "name":"current_color","extent_scale":1.0,"format":"R16G16B16A16_SFLOAT",
    "format_class":"explicit(R16G16B16A16_SFLOAT)","role":"color",
    "usage":["COLOR_ATTACHMENT","SAMPLED"]
  }],
  "rendering_passes":[{"name":"main","passes":[
    {"name":"current_frame","type":"fullscreen",
     "output":{"color":"current_color","depth":null},
     "shader":{"vertex":"shaders/fullscreen","fragment":"shaders/current"}},
    {"name":"temporal_present","type":"fullscreen","canonical_anchor":"post_ldr",
     "input":["temporal_accum"],"output":{"color":"swapchain","depth":null},
     "shader":{"vertex":"shaders/fullscreen","fragment":"shaders/present"}}
  ]}]
})json");
}

void writeVelocitySmokeProject(const std::filesystem::path &root) {
    auto project = makeFeatureProjectJson();
    project["name"] = "velocity smoke";
    writeTextFile(root / "project.json", project.dump(2));
    writeTextFile(root / "scene.json", R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
    writeTextFile(root / "assets.json", R"json({"models":[]})json");
    writeTextFile(root / "ui" / "ui.json", R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
    writeTextFile(root / "shaders" / "fullscreen.vert", stemFullscreenVertexShader());
    writeTextFile(root / "shaders" / "present.frag", solidFullscreenFragmentShader());
    writeTextFile(root / "passes" / "main.json", R"json({
      "features":["engine://features/velocity.json"],"render_targets":[],
      "rendering_passes":[{"name":"main","passes":[{
        "name":"present","type":"fullscreen","output":{"color":"swapchain","depth":null},
        "shader":{"vertex":"shaders/fullscreen","fragment":"shaders/present"}
      }]}]
    })json");
}

void writeSkinnedVelocityProject(const std::filesystem::path &root) {
    auto project = makeFeatureProjectJson();
    project["name"] = "skinned velocity history";
    writeTextFile(root / "project.json", project.dump(2));
    TestSkeletalFixture::writeGlb(root / "character.glb");
    writeTextFile(root / "assets.json",
                  R"json({"models":[{"name":"character","path":"character.glb"}]})json");
    writeTextFile(root / "scene.json", R"json({
      "schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[{
        "name":"Character","components":[
          {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
          {"name":"simplemodelview","model":"character"},
          {"name":"animation","clip":"character.glb#animation/Turn","speed":1.0,"loop":false,"start_time":0.0}
        ]
      }]}}})json");
    writeTextFile(root / "ui" / "ui.json",
                  R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
    writeTextFile(root / "shaders" / "fullscreen.vert", stemFullscreenVertexShader());
    writeTextFile(root / "shaders" / "present.frag", R"glsl(
#version 450
layout(set = 1, binding = 0) uniform sampler2D velocityTexture;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main() {
    vec2 velocity = abs(texture(velocityTexture, uv).xy);
    outColor = vec4(velocity * 20.0, 0.0, 1.0);
}
)glsl");
    writeTextFile(root / "passes" / "main.json", R"json({
      "features":["engine://features/velocity.json"],"render_targets":[],
      "rendering_passes":[{"name":"main","passes":[{
        "name":"present","type":"fullscreen","input":["velocity"],
        "output":{"color":"swapchain","depth":null},
        "shader":{"vertex":"shaders/fullscreen","fragment":"shaders/present"}
      }]}]
    })json");
}

void writeMorphVelocityProject(const std::filesystem::path &root) {
    auto project = makeFeatureProjectJson();
    project["name"] = "morph velocity history";
    writeTextFile(root / "project.json", project.dump(2));
    TestMorphFixture::writeGlb(root / "morph.glb");
    writeTextFile(root / "assets.json",
                  R"json({"models":[{"name":"morph","path":"morph.glb"}]})json");
    writeTextFile(root / "scene.json", R"json({
      "schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[{
        "name":"Morph","components":[
          {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
          {"name":"simplemodelview","model":"morph"}
        ]
      }]}}})json");
    writeTextFile(root / "ui" / "ui.json",
                  R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
    writeTextFile(root / "shaders" / "fullscreen.vert", stemFullscreenVertexShader());
    writeTextFile(root / "shaders" / "present.frag", R"glsl(
#version 450
layout(set = 1, binding = 0) uniform sampler2D velocityTexture;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main() {
    vec2 velocity = texture(velocityTexture, uv).rg;
    outColor = vec4(abs(velocity) * 20.0, 0.0, 1.0);
}
)glsl");
    writeTextFile(root / "passes" / "main.json", R"json({
      "features":["engine://features/velocity.json"],"render_targets":[],
      "rendering_passes":[{"name":"main","passes":[{
        "name":"present","type":"fullscreen","input":["velocity"],
        "output":{"color":"swapchain","depth":null},
        "shader":{"vertex":"shaders/fullscreen","fragment":"shaders/present"}
      }]}]
    })json");
}

uint8_t maximumVelocitySignal(const std::vector<uint8_t> &rgba) {
    uint8_t maximum = 0;
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        maximum = std::max({maximum, rgba[i], rgba[i + 1]});
    }
    return maximum;
}

void writeUiU1Project(const std::filesystem::path &root) {
    auto project = makeFeatureProjectJson();
    project["name"] = "ui u1 golden";
    writeTextFile(root / "project.json", project.dump(2));
    writeTextFile(root / "scene.json", R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
    writeTextFile(root / "assets.json", R"json({"models":[]})json");
    writeTextFile(root / "shaders" / "fullscreen.vert", stemFullscreenVertexShader());
    writeTextFile(root / "shaders" / "present.frag", R"glsl(
#version 450
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(0.015, 0.02, 0.03, 1.0); }
)glsl");
    writeTextFile(root / "passes" / "main.json", R"json({
      "features":["engine://features/ui.json"],"render_targets":[],
      "rendering_passes":[{"name":"main","passes":[{
        "name":"present","type":"fullscreen","output":{"color":"swapchain","depth":null},
        "shader":{"vertex":"shaders/fullscreen","fragment":"shaders/present"}
      }]}]
    })json");
    RgbaImage red{4, 4, std::vector<std::uint8_t>(64)};
    RgbaImage blue{4, 4, std::vector<std::uint8_t>(64)};
    for (std::size_t i = 0; i < 16; ++i) {
        red.pixels[i * 4 + 0] = 230; red.pixels[i * 4 + 1] = 45; red.pixels[i * 4 + 2] = 32; red.pixels[i * 4 + 3] = 220;
        blue.pixels[i * 4 + 0] = 20; blue.pixels[i * 4 + 1] = 105; blue.pixels[i * 4 + 2] = 245; blue.pixels[i * 4 + 3] = 210;
    }
    writePng(root / "ui" / "red.png", red);
    writePng(root / "ui" / "blue.png", blue);
    writeTextFile(root / "ui" / "red.atlas.json", R"json({"schema":"pelican.atlas","version":1,"pages":[{"image":"red.png","size":[4,4]}],"sprites":{"tile":{"page":0,"rect":[0,0,4,4]}}})json");
    writeTextFile(root / "ui" / "blue.atlas.json", R"json({"schema":"pelican.atlas","version":1,"pages":[{"image":"blue.png","size":[4,4]}],"sprites":{"tile":{"page":0,"rect":[0,0,4,4]}}})json");
    writeTextFile(root / "ui" / "ui.json", R"json({
      "schema":"pelican.ui","version":1,"key":"ui_u1_golden","revision":"wp87",
      "root":{"id":"root","type":"panel","children":[
        {"id":"nine","type":"panel","sprite":"ui/red.atlas.json#sprite/tile","sampler":"nearest","nine_patch":[1,1,1,1],"overflow":"clip",
         "layout":{"x":{"mode":"fixed","value":12},"y":{"mode":"fixed","value":12},"offsets":[2,2,0,0]},"children":[
           {"id":"blue_clipped","type":"image","sprite":"ui/blue.atlas.json#sprite/tile","sampler":"nearest","layout":{"x":{"mode":"fixed","value":10},"y":{"mode":"fixed","value":10},"offsets":[-4,-4,0,0]}}]},
        {"id":"red_top","type":"image","sprite":"ui/red.atlas.json#sprite/tile","sampler":"nearest","layout":{"x":{"mode":"fixed","value":8},"y":{"mode":"fixed","value":8},"offsets":[6,6,0,0]}},
        {"id":"blue_top","type":"image","sprite":"ui/blue.atlas.json#sprite/tile","sampler":"nearest","layout":{"x":{"mode":"fixed","value":6},"y":{"mode":"fixed","value":6},"offsets":[9,1,0,0]}}
      ]}
    })json");
}

void writeUiU2Project(const std::filesystem::path &root) {
    writeUiU1Project(root);
    auto project = makeFeatureProjectJson();
    project["name"] = "ui u2 golden";
    writeTextFile(root / "project.json", project.dump(2));
    writeTextFile(root / "ui" / "ui.json", R"json({
      "schema":"pelican.ui","version":1,"key":"ui_u2_golden","revision":"wp93",
      "root":{"id":"root","type":"panel","children":[
        {"id":"button","type":"button","text":"U","color":[28,48,68,255],
         "text_color":[255,255,255,255],
         "layout":{"x":{"mode":"fixed","value":16},"y":{"mode":"fixed","value":16},"offsets":[0,0,0,0]}}
      ]}
    })json");
}

void writeSkeletalProject(const std::filesystem::path &root) {
    auto project = makeVatProjectJson();
    project["name"] = "skeletal toon golden";
    project["basic_config"]["scene_data_json"] = "scene.json";
    project["basic_config"]["asset_data_json"] = "assets.json";
    project["basic_config"]["ui_config_json"] = "ui/ui.json";
    project["basic_config"]["rendering_config_json"] = "passes/main.json";
    project["basic_config"]["default_rendering_pass"] = "main_render";
    writeTextFile(root / "project.json", project.dump(2));
    writeTextFile(root / "scene.json", R"json({
      "schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[{
        "name":"Character","components":[
          {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
          {"name":"simplemodelview","model":"character"},
          {"name":"animation","clip":"character.glb#animation/Turn","speed":1.0,"loop":true,"start_time":0.0}
        ]}, {"name":"Key","components":[
          {"name":"light","type":"directional","direction":[0.2,-0.3,-1.0],"intensity":3.0,"color":[1.0,0.9,0.8]}
        ]}]}}})json");
    writeTextFile(root / "assets.json", R"json({"models":[{"name":"character","path":"character.glb"}]})json");
    writeTextFile(root / "ui" / "ui.json", R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
    std::filesystem::create_directories(root / "passes");
    std::filesystem::copy_file(sourceRoot() / "projects/example/passes/main_rendering_config.json",
                               root / "passes/main.json", std::filesystem::copy_options::overwrite_existing);
    TestSkeletalFixture::writeGlb(root / "character.glb");
}

void writeUsdStaticGeometryProject(const std::filesystem::path &root) {
    auto project = makeVatProjectJson();
    project["name"] = "U-USD0b static geometry golden";
    project["basic_config"]["scene_data_json"] = "scene.json";
    project["basic_config"]["asset_data_json"] = "assets.json";
    project["basic_config"]["ui_config_json"] = "ui/ui.json";
    project["basic_config"]["rendering_config_json"] = "passes/main.json";
    project["basic_config"]["default_rendering_pass"] = "main_render";
    writeTextFile(root / "project.json", project.dump(2));

    const auto delivery = sourceRoot() / "test" / "fixtures" / "usd0b" / "root_yup_m_usda";
    std::ifstream scene_file{delivery / "scene.json", std::ios::binary};
    if (!scene_file) throw std::runtime_error("failed to open U-USD0b scene fixture");
    auto scene = nlohmann::json::parse(scene_file);
    scene["scenes"]["default_scene"]["objects"].push_back({
        {"name", "UsdKeyLight"},
        {"components",
         nlohmann::json::array({nlohmann::json{
             {"name", "light"},
             {"type", "directional"},
             {"direction", {0.0, 0.0, -1.0}},
             {"intensity", 3.0},
             {"color", {1.0, 0.95, 0.85}},
         }})},
    });
    writeTextFile(root / "scene.json", scene.dump(2));
    writeTextFile(root / "assets.json", R"json({"models":[]})json");
    writeTextFile(root / "ui" / "ui.json", R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
    std::filesystem::create_directories(root / "passes");
    std::filesystem::copy_file(sourceRoot() / "projects/example/passes/main_rendering_config.json",
                               root / "passes/main.json",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(delivery / "model.glb", root / "model.glb",
                               std::filesystem::copy_options::overwrite_existing);
}

void writeUsdOpenPbrMaterialProject(const std::filesystem::path &root) {
    auto project = makeVatProjectJson();
    project["name"] = "U-USD0c generated OpenPBR material golden";
    project["basic_config"]["scene_data_json"] = "scene.json";
    project["basic_config"]["asset_data_json"] = "assets.json";
    project["basic_config"]["ui_config_json"] = "ui/ui.json";
    project["basic_config"]["rendering_config_json"] = "passes/main.json";
    project["basic_config"]["default_rendering_pass"] = "main_render";
    writeTextFile(root / "project.json", project.dump(2));

    const auto delivery = sourceRoot() / "test/fixtures/usd0c/root_materials_coat";
    std::ifstream scene_file{delivery / "scene.json", std::ios::binary};
    if (!scene_file) throw std::runtime_error("failed to open U-USD0c scene fixture");
    auto scene = nlohmann::json::parse(scene_file);
    scene["scenes"]["default_scene"]["objects"].at(0)["components"].at(1)["model"] =
        "coat";
    scene["scenes"]["default_scene"]["objects"].push_back({
        {"name", "UsdCoatKeyLight"},
        {"components",
         nlohmann::json::array({nlohmann::json{
             {"name", "light"},
             {"type", "directional"},
             {"direction", {0.25, -0.35, 1.0}},
             {"intensity", 3.0},
             {"color", {1.0, 0.95, 0.88}},
         }})},
    });
    writeTextFile(root / "scene.json", scene.dump(2));
    writeTextFile(root / "assets.json", R"json({"models":[{
      "name":"coat","path":"model.glb","material_bindings":"material_bindings.json"
    }]})json");
    writeTextFile(root / "ui" / "ui.json", R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
    std::filesystem::create_directories(root / "passes");
    std::filesystem::copy_file(sourceRoot() / "projects/example/passes/main_rendering_config.json",
                               root / "passes/main.json",
                               std::filesystem::copy_options::overwrite_existing);
    for (const auto &name : {"manifest.json", "materials.json", "material_bindings.json",
                             "model.glb"}) {
        std::filesystem::copy_file(delivery / name, root / name,
                                   std::filesystem::copy_options::overwrite_existing);
    }
    std::filesystem::copy(delivery / "textures", root / "textures",
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing);
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
    writeTextFile(root / "ui" / "ui.json", R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
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

void writeParentTransformProject(const std::filesystem::path &root) {
    writeDebugDrawProject(root);
    writeTextFile(root / "scene.json", R"json({
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": [
        {
          "name": "RotatingParent",
          "components": [
            {"name":"transform", "pos":[0,0,0], "rotation":[0,0,0.7071067811865476,0.7071067811865476], "scale":[1,1,1]}
          ]
        },
        {
          "name": "OrbitingChild",
          "parent": "RotatingParent",
          "components": [
            {"name":"transform", "pos":[0.5,0,0], "rotation":[0,0,0,1], "scale":[1,1,1]}
          ]
        }
      ]
    }
  }
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
    writeTextFile(root / "ui" / "ui.json", R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
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
    writeTextFile(root / "ui" / "ui.json", R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
    writeTextFile(root / "shaders" / "fullscreen.vert", stemFullscreenVertexShader());
    writeTextFile(root / "shaders" / "hdr_source.frag", hdrSourceFragmentShader());
    writeTextFile(root / "shaders" / "copy_input.frag", copyInputFragmentShader());
    writeTextFile(root / "shaders" / "history_image.comp",
                  computeHistoryImageShader());
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
                          {"name", "overlay"},
                          {"type", "fullscreen"},
                          {"output", {{"color", "lit_color"}, {"depth", nullptr}}},
                          {"color_load_op", "load"},
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

nlohmann::json makeBLayerShadowRenderingConfig(
    std::string_view feature_reference) {
    auto config = nlohmann::json{
        {"pipeline",
         {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
        {"render_strategy",
         {{"name", "golden.shadow_b_layer"}}},
        {"graph_transforms",
         nlohmann::json::array(
             {{{"name", "golden.shadow_b_layer.identity"}}})},
    };
    if (!feature_reference.empty()) {
        config["features"] =
            nlohmann::json::array({std::string{feature_reference}});
    }
    return config;
}

nlohmann::json makeTaaRenderingConfig() {
    auto config = makeShadowRenderingConfig(false);
    config["render_targets"].push_back({
        {"name", "lit_color"},
        {"extent_scale", 1.0},
        {"format", "B8G8R8A8_UNORM"},
        {"format_class", "scene"},
        {"role", "color"},
        {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
    });
    auto &passes = config["rendering_passes"].at(0).at("passes");
    auto lighting = std::find_if(passes.begin(), passes.end(), [](const auto &pass) {
        return pass.value("name", std::string{}) == "lighting_pass";
    });
    if (lighting == passes.end()) {
        throw std::runtime_error("TAA golden fixture requires lighting_pass");
    }
    (*lighting)["output"]["color"] = "lit_color";
    passes.push_back({
        {"name", "taa_present"},
        {"type", "fullscreen"},
        {"canonical_anchor", "post_ldr"},
        {"input", nlohmann::json::array({"lit_color"})},
        {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
        {"shader", {{"vertex", "shaders/fullscreen"},
                    {"fragment", "shaders/copy_input"}}},
    });
    config["features"] = nlohmann::json::array({
        "engine://features/velocity.json",
        nlohmann::json{
            {"ref", "engine://features/taa.json"},
            {"parameters", {
                {"scene_color", "lit_color"},
                {"velocity", "velocity"},
                {"depth", "offscreen_depth"},
                {"downstream_color", "lit_color"},
                {"alpha", 0.1},
                {"disocclusion_tau", 0.1},
                {"depth_epsilon", 0.00001},
            }},
        },
    });
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
    writeTextFile(root / "ui" / "ui.json", R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
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

void writeGpuDrawProject(
    const std::filesystem::path &root,
    std::uint32_t produced_count,
    bool force_cpu) {
    writeShadowProject(root, false);
    auto config =
        makeShadowRenderingConfig(false);
    config["buffers"] =
        nlohmann::json::array({
            {
                {"name", "draw_candidates"},
                {"size", 40},
                {"host_source",
                 "scene_draw_commands_v1"},
                {"command_layout",
                 "indexed_draw"},
            },
            {
                {"name", "visible_draws"},
                {"size", 40},
                {"command_layout",
                 "indexed_draw"},
            },
            {
                {"name", "visible_draw_count"},
                {"size", 4},
                {"command_layout", "draw_count"},
            },
        });
    config["compute_tasks"] =
        nlohmann::json::array({
            {
                {"name", "build_visible_draws"},
                {"shader",
                 "shaders/build_visible_draws"},
                {"reads",
                 nlohmann::json::array(
                     {"draw_candidates"})},
                {"writes",
                 nlohmann::json::array(
                     {"visible_draws",
                      "visible_draw_count"})},
                {"before",
                 nlohmann::json::array(
                     {"gbuffer_pass"})},
                {"dispatch",
                 {{"groups",
                   nlohmann::json::array(
                       {1, 1, 1})}}},
                {"schedule", "per_frame"},
            },
        });
    auto &passes =
        config["rendering_passes"][0]["passes"];
    const auto geometry = std::find_if(
        passes.begin(), passes.end(),
        [](const auto &pass) {
            return pass.value(
                       "name", std::string{}) ==
                   "gbuffer_pass";
        });
    if (geometry == passes.end()) {
        throw std::runtime_error(
            "GPU draw golden requires gbuffer_pass");
    }
    (*geometry)["material_range"] = {
        {"start", 0},
        {"count", 1},
    };
    (*geometry)["gpu_draw_source"] = {
        {"commands", "visible_draws"},
        {"count", "visible_draw_count"},
        {"max_draw_count", 2},
    };
    if (force_cpu) {
        (*geometry)["gpu_draw_source"]
                   ["execution"] =
            "cpu";
    }
    writeTextFile(
        root / "passes" / "main.json",
        config.dump(2));
    writeTextFile(
        root / "shaders" /
            "build_visible_draws.comp",
        "#version 450\n"
        "layout(local_size_x=1,local_size_y=1,local_size_z=1) in;\n"
        "struct DrawCommand{uint indexCount;uint instanceCount;"
        "uint firstIndex;int vertexOffset;uint firstInstance;};\n"
        "layout(std430,set=1,binding=0) readonly buffer Candidates{"
        "DrawCommand values[];} candidates;\n"
        "layout(std430,set=1,binding=1) writeonly buffer Visible{"
        "DrawCommand values[];} visible;\n"
        "layout(std430,set=1,binding=2) buffer Count{uint value;} "
        "draw_count;\n"
        "void main(){visible.values[0]=candidates.values[0];"
        "visible.values[1]=candidates.values[1];draw_count.value=" +
            std::to_string(produced_count) +
            "u;}\n");
}

void writeGpuOcclusionProject(
    const std::filesystem::path &root,
    bool candidate_visible,
    bool force_cpu) {
    writeShadowProject(root, false);
    writeTextFile(
        root / "scene.json",
        nlohmann::json{
            {"schema", "pelican.scene"},
            {"version", 1},
            {"scenes",
             {{"default_scene",
               {{"objects",
                 nlohmann::json::array({
                     {
                         {"name", "Occluder"},
                         {"components",
                          nlohmann::json::array({
                              {
                                  {"name", "transform"},
                                  {"pos",
                                   nlohmann::json::array(
                                       {0.0, -0.12, 0.0})},
                                  {"rotation",
                                   nlohmann::json::array(
                                       {0.0, 0.0, 0.0,
                                        1.0})},
                                  {"scale",
                                   nlohmann::json::array(
                                       {1.35, 1.35,
                                        0.12})},
                              },
                              {
                                  {"name",
                                   "simplemodelview"},
                                  {"model", "ground"},
                              },
                          })},
                     },
                     {
                         {"name", "Candidate"},
                         {"components",
                          nlohmann::json::array({
                              {
                                  {"name", "transform"},
                                  {"pos",
                                   nlohmann::json::array(
                                       {candidate_visible
                                            ? 2.1
                                            : 0.0,
                                        0.12, 1.2})},
                                  {"rotation",
                                   nlohmann::json::array(
                                       {0.0, 0.0, 0.0,
                                        1.0})},
                                  {"scale",
                                   nlohmann::json::array(
                                       {0.32, 0.32,
                                        0.32})},
                              },
                              {
                                  {"name",
                                   "simplemodelview"},
                                  {"model", "ground"},
                              },
                          })},
                     },
                     {
                         {"name", "Sun"},
                         {"components",
                          nlohmann::json::array({
                              {
                                  {"name", "light"},
                                  {"type",
                                   "directional"},
                                  {"direction",
                                   nlohmann::json::array(
                                       {0.35, -1.0,
                                        -0.25})},
                                  {"intensity", 3.0},
                                  {"color",
                                   nlohmann::json::array(
                                       {1.0, 0.94,
                                        0.82})},
                              },
                          })},
                     },
                 })}}}}},
        }.dump(2));

    auto config =
        makeShadowRenderingConfig(false);
    auto &targets = config["render_targets"];
    targets.push_back({
        {"name",
         "occlusion_gbuffer_albedo"},
        {"extent_scale", 1.0},
        {"format", "B8G8R8A8_UNORM"},
        {"format_class", "scene"},
        {"usage",
         nlohmann::json::array(
             {"COLOR_ATTACHMENT"})},
    });
    targets.push_back({
        {"name",
         "occlusion_gbuffer_normal"},
        {"extent_scale", 1.0},
        {"format",
         "R16G16B16A16_SFLOAT"},
        {"format_class", "data"},
        {"usage",
         nlohmann::json::array(
             {"COLOR_ATTACHMENT"})},
    });
    targets.push_back({
        {"name",
         "occlusion_gbuffer_material"},
        {"extent_scale", 1.0},
        {"format", "R8G8B8A8_UNORM"},
        {"format_class", "data"},
        {"usage",
         nlohmann::json::array(
             {"COLOR_ATTACHMENT"})},
    });
    targets.push_back({
        {"name",
         "occlusion_gbuffer_worldpos"},
        {"extent_scale", 1.0},
        {"format",
         "R16G16B16A16_SFLOAT"},
        {"format_class", "data"},
        {"usage",
         nlohmann::json::array(
             {"COLOR_ATTACHMENT"})},
    });
    targets.push_back({
        {"name",
         "occlusion_gbuffer_emissive"},
        {"extent_scale", 1.0},
        {"format", "R8G8B8A8_UNORM"},
        {"format_class", "scene"},
        {"usage",
         nlohmann::json::array(
             {"COLOR_ATTACHMENT"})},
    });
    targets.push_back({
        {"name", "occlusion_depth"},
        {"extent_scale", 1.0},
        {"format", "D32_SFLOAT"},
        {"format_class", "data"},
        {"usage",
         nlohmann::json::array({
             "DEPTH_STENCIL_ATTACHMENT",
             "SAMPLED",
         })},
    });
    targets.push_back({
        {"name", "occlusion_pyramid"},
        {"extent_scale", 1.0},
        {"format", "R32_SFLOAT"},
        {"format_class", "data"},
        {"usage",
         nlohmann::json::array(
             {"STORAGE", "SAMPLED"})},
        {"mip_levels", "full"},
    });

    config["buffers"] =
        nlohmann::json::array({
            {
                {"name", "draw_candidates"},
                {"size", 40},
                {"host_source",
                 "scene_draw_commands_v1"},
                {"command_layout",
                 "indexed_draw"},
            },
            {
                {"name", "draw_bounds"},
                {"size", 64},
                {"host_source",
                 "scene_draw_bounds_v1"},
            },
            {
                {"name", "visible_draws"},
                {"size", 40},
                {"command_layout",
                 "indexed_draw"},
            },
            {
                {"name", "visible_draw_count"},
                {"size", 4},
                {"command_layout", "draw_count"},
            },
        });

    auto &passes =
        config["rendering_passes"][0]["passes"];
    const auto geometry = std::find_if(
        passes.begin(), passes.end(),
        [](const auto &pass) {
            return pass.value(
                       "name", std::string{}) ==
                   "gbuffer_pass";
        });
    if (geometry == passes.end()) {
        throw std::runtime_error(
            "GPU occlusion golden requires gbuffer_pass");
    }
    (*geometry)["material_range"] = {
        {"start", 0},
        {"count", 1},
    };
    (*geometry)["gpu_draw_source"] = {
        {"commands", "visible_draws"},
        {"count", "visible_draw_count"},
        {"max_draw_count", 2},
    };
    if (force_cpu) {
        (*geometry)["gpu_draw_source"]
                   ["execution"] =
            "cpu";
    }
    const auto geometry_index =
        static_cast<std::size_t>(
            std::distance(
                passes.begin(), geometry));
    auto depth_prepass = *geometry;
    depth_prepass["name"] =
        "occlusion_depth_prepass";
    depth_prepass.erase(
        "gpu_draw_source");
    depth_prepass["output"] = {
        {"color",
         nlohmann::json::array({
             "occlusion_gbuffer_albedo",
             "occlusion_gbuffer_normal",
             "occlusion_gbuffer_material",
             "occlusion_gbuffer_worldpos",
             "occlusion_gbuffer_emissive",
         })},
        {"depth", "occlusion_depth"},
    };
    passes.insert(
        passes.begin() +
            static_cast<
                nlohmann::json::difference_type>(
                geometry_index),
        std::move(depth_prepass));

    auto tasks = nlohmann::json::array();
    tasks.push_back({
        {"name", "occlusion_depth_seed"},
        {"shader",
         "shaders/occlusion_depth_seed"},
        {"reads",
         nlohmann::json::array(
             {"occlusion_depth"})},
        {"writes",
         nlohmann::json::array(
             {"occlusion_pyramid"})},
        {"after",
         nlohmann::json::array(
             {"occlusion_depth_prepass"})},
        {"before",
         nlohmann::json::array(
             {"occlusion_depth_reduce_1"})},
        {"resource_ports",
         {
             {"scene_depth",
              {
                  {"resource",
                   "occlusion_depth"},
                  {"access", "sampled"},
                  {"sampling",
                   {
                       {"filter", "nearest"},
                       {"address",
                        "clamp_to_edge"},
                   }},
              }},
             {"pyramid_seed",
              {
                  {"resource",
                   "occlusion_pyramid"},
                  {"access", "storage"},
                  {"subresource",
                   {
                       {"mip", 0},
                   }},
              }},
         }},
        {"dispatch",
         {{"groups",
           nlohmann::json::array(
               {2, 2, 1})}}},
        {"schedule", "per_frame"},
    });
    for (std::uint32_t mip = 1;
         mip < 5; ++mip) {
        tasks.push_back({
            {"name",
             "occlusion_depth_reduce_" +
                 std::to_string(mip)},
            {"shader",
             "shaders/occlusion_depth_reduce"},
            {"reads",
             nlohmann::json::array(
                 {"occlusion_pyramid"})},
            {"writes",
             nlohmann::json::array(
                 {"occlusion_pyramid"})},
            {"after",
             nlohmann::json::array(
                 {mip == 1
                      ? "occlusion_depth_seed"
                      : "occlusion_depth_reduce_" +
                            std::to_string(
                                mip - 1)})},
            {"before",
             nlohmann::json::array(
                 {mip == 4
                      ? "occlusion_cull"
                      : "occlusion_depth_reduce_" +
                            std::to_string(
                                mip + 1)})},
            {"resource_ports",
             {
                 {"source_depth",
                  {
                      {"resource",
                       "occlusion_pyramid"},
                      {"access", "sampled"},
                      {"sampling",
                       {
                           {"filter", "nearest"},
                           {"address",
                            "clamp_to_edge"},
                       }},
                      {"subresource",
                       {
                           {"mip", mip - 1},
                       }},
                  }},
                 {"reduced_depth",
                  {
                      {"resource",
                       "occlusion_pyramid"},
                      {"access", "storage"},
                      {"subresource",
                       {
                           {"mip", mip},
                       }},
                  }},
             }},
            {"dispatch",
             {{"groups",
               nlohmann::json::array(
                   {1, 1, 1})}}},
            {"schedule", "per_frame"},
        });
    }
    tasks.push_back({
        {"name", "occlusion_count_reset"},
        {"shader",
         "shaders/occlusion_count_reset"},
        {"writes",
         nlohmann::json::array(
             {"visible_draw_count"})},
        {"before",
         nlohmann::json::array(
             {"occlusion_cull"})},
        {"dispatch",
         {{"groups",
           nlohmann::json::array(
               {1, 1, 1})}}},
        {"schedule", "per_frame"},
    });
    tasks.push_back({
        {"name", "occlusion_cull"},
        {"shader",
         "shaders/occlusion_cull"},
        {"reads",
         nlohmann::json::array(
             {"draw_candidates",
              "draw_bounds",
              "occlusion_pyramid"})},
        {"writes",
         nlohmann::json::array(
             {"visible_draws",
              "visible_draw_count"})},
        {"after",
         nlohmann::json::array(
             {"occlusion_depth_reduce_4",
              "occlusion_count_reset"})},
        {"before",
         nlohmann::json::array(
             {"gbuffer_pass"})},
        {"resource_ports",
         {
             {"depth_pyramid",
              {
                  {"resource",
                   "occlusion_pyramid"},
                  {"access", "sampled"},
                  {"sampling",
                   {
                       {"filter", "nearest"},
                       {"address",
                        "clamp_to_edge"},
                   }},
                  {"subresource",
                   {
                       {"mip", 0},
                       {"mip_count", 5},
                   }},
              }},
         }},
        {"dispatch",
         {{"groups",
           nlohmann::json::array(
               {1, 1, 1})}}},
        {"schedule", "per_frame"},
    });
    config["compute_tasks"] =
        std::move(tasks);
    writeTextFile(
        root / "passes" / "main.json",
        config.dump(2));

    writeTextFile(
        root / "shaders" /
            "occlusion_depth_seed.comp",
        R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=8,local_size_y=8,local_size_z=1) in;
void main() {
    ivec2 coordinate = ivec2(gl_GlobalInvocationID.xy);
    ivec2 output_size = pelican_size_pyramid_seed();
    if (any(greaterThanEqual(coordinate, output_size))) return;
    vec2 uv = (vec2(coordinate) + vec2(0.5)) /
              vec2(output_size);
    pelican_store_pyramid_seed(
        coordinate,
        vec4(pelican_sample_scene_depth(uv).r));
}
)glsl");
    writeTextFile(
        root / "shaders" /
            "occlusion_depth_reduce.comp",
        R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=8,local_size_y=8,local_size_z=1) in;
void main() {
    ivec2 coordinate = ivec2(gl_GlobalInvocationID.xy);
    ivec2 output_size = pelican_size_reduced_depth();
    if (any(greaterThanEqual(coordinate, output_size))) return;
    ivec2 source_size = pelican_size_source_depth();
    ivec2 base = coordinate * 2;
    float farthest = 0.0;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            ivec2 source = min(
                base + ivec2(x, y),
                source_size - ivec2(1));
            vec2 uv = (vec2(source) + vec2(0.5)) /
                      vec2(source_size);
            farthest = max(
                farthest,
                pelican_sample_source_depth(uv).r);
        }
    }
    pelican_store_reduced_depth(
        coordinate, vec4(farthest));
}
)glsl");
    writeTextFile(
        root / "shaders" /
            "occlusion_count_reset.comp",
        R"glsl(
#version 450
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
layout(std430,set=1,binding=0) buffer Count {
    uint value;
} visible_count;
void main() {
    visible_count.value = 0u;
}
)glsl");
    writeTextFile(
        root / "shaders" /
            "occlusion_cull.comp",
        R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_frame.glsl"
#include "pelican_resource_ports.glsl"

layout(local_size_x=64,local_size_y=1,local_size_z=1) in;

struct DrawCommand {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
struct DrawBounds {
    vec4 minimum;
    vec4 maximum;
};
layout(std430,set=1,binding=0) readonly buffer Candidates {
    DrawCommand values[];
} candidates;
layout(std430,set=1,binding=1) readonly buffer Bounds {
    DrawBounds values[];
} bounds;
layout(std430,set=1,binding=3) writeonly buffer Visible {
    DrawCommand values[];
} visible;
layout(std430,set=1,binding=4) buffer Count {
    uint value;
} visible_count;

bool survivesOcclusion(DrawBounds bound) {
    if (bound.minimum.w < 0.5) return true;

    vec2 ndc_min = vec2(1.0);
    vec2 ndc_max = vec2(-1.0);
    float nearest_depth = 1.0;
    for (uint corner = 0u; corner < 8u; ++corner) {
        vec3 world = vec3(
            (corner & 1u) != 0u
                ? bound.maximum.x
                : bound.minimum.x,
            (corner & 2u) != 0u
                ? bound.maximum.y
                : bound.minimum.y,
            (corner & 4u) != 0u
                ? bound.maximum.z
                : bound.minimum.z);
        vec4 clip = pelicanFrame.projection *
                    pelicanFrame.view *
                    vec4(world, 1.0);
        if (clip.w <= 0.00001) return true;
        vec3 ndc = clip.xyz / clip.w;
        ndc_min = min(ndc_min, ndc.xy);
        ndc_max = max(ndc_max, ndc.xy);
        nearest_depth = min(nearest_depth, ndc.z);
    }

    // Frustum rejection is deliberately left to a later policy. This task
    // only rejects objects for which the hierarchy proves full occlusion.
    if (ndc_max.x < -1.0 || ndc_min.x > 1.0 ||
        ndc_max.y < -1.0 || ndc_min.y > 1.0) {
        return true;
    }
    vec2 uv_min = clamp(
        ndc_min * 0.5 + 0.5,
        vec2(0.0), vec2(1.0));
    vec2 uv_max = clamp(
        ndc_max * 0.5 + 0.5,
        vec2(0.0), vec2(1.0));
    vec2 base_size =
        vec2(pelican_size_lod_depth_pyramid(0));
    vec2 pixel_span =
        max((uv_max - uv_min) * base_size,
            vec2(1.0));
    float lod = floor(log2(max(
        pixel_span.x, pixel_span.y)));
    lod = clamp(
        lod, 0.0,
        float(pelican_mip_count_depth_pyramid() - 1u));

    float farthest_depth = 0.0;
    farthest_depth = max(
        farthest_depth,
        pelican_sample_lod_depth_pyramid(
            uv_min, lod).r);
    farthest_depth = max(
        farthest_depth,
        pelican_sample_lod_depth_pyramid(
            vec2(uv_max.x, uv_min.y), lod).r);
    farthest_depth = max(
        farthest_depth,
        pelican_sample_lod_depth_pyramid(
            vec2(uv_min.x, uv_max.y), lod).r);
    farthest_depth = max(
        farthest_depth,
        pelican_sample_lod_depth_pyramid(
            uv_max, lod).r);
    return nearest_depth <=
           farthest_depth + 0.0005;
}

void main() {
    uint candidate = gl_GlobalInvocationID.x;
    uint candidate_count = min(
        uint(candidates.values.length()),
        uint(bounds.values.length()));
    if (candidate >= candidate_count) return;
    if (!survivesOcclusion(bounds.values[candidate])) return;

    uint output_index = atomicAdd(
        visible_count.value, 1u);
    if (output_index < uint(visible.values.length())) {
        visible.values[output_index] =
            candidates.values[candidate];
    }
}
)glsl");
}

void writeBLayerShadowProject(const std::filesystem::path &root,
                              std::string_view mode) {
    writeShadowProject(root, false);

    std::string feature_reference;
    if (mode == "shadow_b_layer_engine") {
        feature_reference =
            "engine://features/shadow_directional.json";
    } else if (mode == "shadow_b_layer_project") {
        feature_reference =
            "project://features/shadow_directional.json";
        writeTextFile(
            root / "features" / "shadow_directional.json",
            engineResourceOrThrow(
                "features/shadow_directional.json"));
    } else if (mode != "shadow_b_layer_off") {
        throw std::runtime_error(
            "unknown B-layer shadow golden mode: " +
            std::string{mode});
    }

    writeTextFile(
        root / "passes" / "main.json",
        makeBLayerShadowRenderingConfig(feature_reference).dump(2));
}

void writeMorphSkinnedShadowProject(const std::filesystem::path &root) {
    writeShadowProject(root, true);
    TestMorphFixture::writeGlb(root / "assets" / "morph.glb",
                               {.skinned = true, .mesh_weight = 1.0});
    writeTextFile(root / "assets.json", R"json({
      "models":[
        {"name":"ground","path":"assets/ground.glb"},
        {"name":"morph","path":"assets/morph.glb"}
      ]
    })json");
    writeTextFile(root / "scene.json", R"json({
      "schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[
        {"name":"Ground","components":[
          {"name":"transform","pos":[0,-0.72,0],"rotation":[0,0,0,1],"scale":[3,0.05,3]},
          {"name":"simplemodelview","model":"ground"}
        ]},
        {"name":"MorphedCaster","components":[
          {"name":"transform","pos":[-0.3,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
          {"name":"simplemodelview","model":"morph"}
        ]},
        {"name":"Sun","components":[
          {"name":"light","type":"directional","direction":[0.35,-1.0,-0.25],"intensity":3.0,"color":[1.0,0.94,0.82]}
        ]}
      ]}}})json");
}

void writeMaterialInstanceOverrideProject(const std::filesystem::path &root) {
    writeShadowProject(root, false);
    TestMorphFixture::writeGlb(root / "assets" / "tint.glb");
    writeTextFile(root / "assets.json", R"json({
      "models":[{"name":"tint","path":"assets/tint.glb"}]
    })json");
    writeTextFile(root / "scene.json", R"json({
      "schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[
        {"name":"TintLeft","components":[
          {"name":"transform","pos":[-0.72,0,0],"rotation":[0,0,0,1],"scale":[0.9,0.9,0.9]},
          {"name":"simplemodelview","model":"tint"}
        ]},
        {"name":"TintRight","components":[
          {"name":"transform","pos":[0.72,0,0],"rotation":[0,0,0,1],"scale":[0.9,0.9,0.9]},
          {"name":"simplemodelview","model":"tint"}
        ]},
        {"name":"TintSun","components":[
          {"name":"light","type":"directional","direction":[0.1,-0.2,-1.0],"intensity":4.0,"color":[1.0,0.95,0.9]}
        ]}
      ]}}})json");
}

void writeVrmExpressionProject(const std::filesystem::path &root) {
    writeShadowProject(root, false);
    TestMorphFixture::writeGlb(root / "assets" / "expression.vrm",
                               {.skinned = true, .vrm_expression = true});
    writeTextFile(root / "assets.json", R"json({
      "models":[{"name":"expression","path":"assets/expression.vrm"}]
    })json");
    writeTextFile(root / "scene.json", R"json({
      "schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[
        {"name":"ExpressionLeft","components":[
          {"name":"transform","pos":[-0.72,0,0],"rotation":[0,0,0,1],"scale":[0.9,0.9,0.9]},
          {"name":"simplemodelview","model":"expression"}
        ]},
        {"name":"ExpressionRight","components":[
          {"name":"transform","pos":[0.72,0,0],"rotation":[0,0,0,1],"scale":[0.9,0.9,0.9]},
          {"name":"simplemodelview","model":"expression"}
        ]},
        {"name":"ExpressionSun","components":[
          {"name":"light","type":"directional","direction":[0.1,-0.2,-1.0],"intensity":4.0,"color":[1.0,0.95,0.9]}
        ]}
      ]}}})json");
}

void writeVrmXrDemoGoldenProject(const std::filesystem::path &root) {
    writeShadowProject(root, false);
    TestVrmXrDemoFixture::writeGlb(root / "assets" / "vrm_xr_character.vrm");
    writeTextFile(root / "assets.json", R"json({
      "models":[{"name":"vrm_xr_character","path":"assets/vrm_xr_character.vrm"}]
    })json");
    writeTextFile(root / "scene.json", R"json({
      "schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[
        {"name":"VrmXrGolden","components":[
          {"name":"transform","pos":[0,1.0,0],"rotation":[0,0,1,0],"scale":[0.8,0.8,0.8]},
          {"name":"simplemodelview","model":"vrm_xr_character"}
        ]},
        {"name":"VrmXrSun","components":[
          {"name":"light","type":"directional","direction":[0.1,-0.2,1.0],"intensity":4.0,"color":[1.0,0.95,0.9]}
        ]}
      ]}}})json");
}

void writeMaterialAbsoluteOverrideProject(const std::filesystem::path &root) {
    writeShadowProject(root, false);
    TestMaterialAbsoluteOverrideFixture::writeGlb(
        root / "assets" / "absolute.glb");
    writeTextFile(root / "assets.json", R"json({
      "models":[{"name":"absolute","path":"assets/absolute.glb"}]
    })json");
    writeTextFile(root / "scene.json", R"json({
      "schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[
        {"name":"AbsoluteBottom","components":[
          {"name":"transform","pos":[0,-0.48,0],"rotation":[0,0,0,1],"scale":[0.9,0.9,0.9]},
          {"name":"simplemodelview","model":"absolute"}
        ]},
        {"name":"AbsoluteTop","components":[
          {"name":"transform","pos":[0,0.48,0],"rotation":[0,0,0,1],"scale":[0.9,0.9,0.9]},
          {"name":"simplemodelview","model":"absolute"}
        ]},
        {"name":"AbsoluteSun","components":[
          {"name":"light","type":"directional","direction":[0.1,-0.2,-1.0],"intensity":4.0,"color":[1.0,0.95,0.9]}
        ]}
      ]}}})json");
}

void writeTaaProject(const std::filesystem::path &root, bool orthographic) {
    writeShadowProject(root, false);
    writeTextFile(root / "shaders" / "copy_input.frag", copyInputFragmentShader());
    writeTextFile(root / "passes" / "main.json", makeTaaRenderingConfig().dump(2));
    if (!orthographic) {
        return;
    }

    std::ifstream scene_file{root / "scene.json", std::ios::binary};
    auto scene = nlohmann::json::parse(scene_file);
    scene["scenes"]["default_scene"]["objects"].push_back({
        {"name", "TaaOrthoCamera"},
        {"components", nlohmann::json::array({nlohmann::json{
            {"name", "camera"},
            {"type", "orthographic"},
            {"xmag", 5.0},
            {"ymag", 3.0},
            {"znear", 0.1},
            {"zfar", 20.0},
        }})},
    });
    writeTextFile(root / "scene.json", scene.dump(2));
}

bool isStrictSpriteGoldenMode(std::string_view mode) {
    return mode == "sprite_pixel_zoom1" || mode == "sprite_pixel_zoom2_odd" ||
           mode == "sprite_pixel_zoom3_policy" || mode == "sprite_billboard";
}

bool isSpriteGoldenMode(std::string_view mode) {
    return mode == "sprite_ortho_atlas" || mode == "sprite_depth" ||
           mode == "sprite_multi_atlas" || mode == "sprite_parent" ||
           mode == "sprite_ownership" || isStrictSpriteGoldenMode(mode);
}

nlohmann::json writeSpriteProject(const std::filesystem::path &root,
                                  const GoldenCase &golden_case,
                                  bool gpu_timing = false) {
    const auto mode = std::string_view{golden_case.mode};
    auto project = makeFeatureProjectJson();
    project["name"] = std::string{mode};
    project["basic_config"]["window_size"] = {
        {"width", golden_case.width}, {"height", golden_case.height}};
    if (isStrictSpriteGoldenMode(mode)) {
        project["basic_config"]["sprite"] = {{"pixels_per_unit", 4.0}};
    }
    writeTextFile(root / "project.json", project.dump(2));

    RgbaImage page0{32, 32, std::vector<std::uint8_t>(32 * 32 * 4)};
    RgbaImage page1{32, 32, std::vector<std::uint8_t>(32 * 32 * 4)};
    RgbaImage page2{8, 8, std::vector<std::uint8_t>(8 * 8 * 4)};
    for (std::uint32_t y = 0; y < 32; ++y) {
        for (std::uint32_t x = 0; x < 32; ++x) {
            const auto i = static_cast<std::size_t>((y * 32 + x) * 4);
            const std::array<std::uint8_t, 4> a = x < 16
                ? std::array<std::uint8_t, 4>{245, 60, 35, static_cast<std::uint8_t>(y < 16 ? 255 : 190)}
                : std::array<std::uint8_t, 4>{30, 225, 85, static_cast<std::uint8_t>(y < 16 ? 255 : 190)};
            const std::array<std::uint8_t, 4> b = y < 16
                ? std::array<std::uint8_t, 4>{40, 100, 245, 255}
                : std::array<std::uint8_t, 4>{245, 210, 35, 255};
            std::copy(a.begin(), a.end(), page0.pixels.begin() + static_cast<std::ptrdiff_t>(i));
            std::copy(b.begin(), b.end(), page1.pixels.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 8; ++x) {
            const auto i = static_cast<std::size_t>((y * 8 + x) * 4);
            const std::array<std::uint8_t, 4> texel{
                static_cast<std::uint8_t>(28 + x * 27),
                static_cast<std::uint8_t>(36 + y * 25),
                static_cast<std::uint8_t>(45 + ((x * 3 + y * 5) % 8) * 25),
                255};
            std::copy(texel.begin(), texel.end(),
                      page2.pixels.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    writePng(root / "assets/page0.png", page0);
    writePng(root / "assets/page1.png", page1);
    writePng(root / "assets/page2.png", page2);
    writeTextFile(root / "assets/atlas.json", R"json({
      "schema":"pelican.atlas","version":1,
      "pages":[{"image":"page0.png","size":[32,32]},{"image":"page1.png","size":[32,32]},
               {"image":"page2.png","size":[8,8]}],
      "sprites":{
        "page0":{"page":0,"rect":[0,0,32,32]},
        "red":{"page":0,"rect":[0,0,16,32]},
        "green":{"page":0,"rect":[16,0,32,32]},
        "page1":{"page":1,"rect":[0,0,32,32]},
        "pixel_even":{"page":2,"rect":[0,0,4,6]},
        "pixel_odd_edge":{"page":2,"rect":[5,3,8,8]},
        "pixel_tiny":{"page":2,"rect":[3,0,5,3]},
        "pixel_dot":{"page":2,"rect":[7,0,8,2]}
      }
    })json");

    nlohmann::json assets{{"models", nlohmann::json::array()},
                          {"textures", nlohmann::json::array({
                              {{"name", "atlas"}, {"path", "assets/atlas.json"}, {"sampler", "nearest"}}
                          })}};
    if (mode == "sprite_depth") {
        assets["models"].push_back({{"name", "ground"}, {"path", "assets/ground.glb"}});
        std::filesystem::copy_file(sourceRoot() / "test/fixtures/ground.glb", root / "assets/ground.glb",
                                   std::filesystem::copy_options::overwrite_existing);
    }
    writeTextFile(root / "assets.json", assets.dump(2));

    auto transform = [](std::array<float, 3> pos, std::array<float, 4> rotation = {0, 0, 0, 1},
                        std::array<float, 3> scale = {1, 1, 1}) {
        return nlohmann::json{{"name", "transform"}, {"pos", pos}, {"rotation", rotation}, {"scale", scale}};
    };
    auto sprite = [](std::string texture, std::array<float, 2> size, int layer = 0,
                     std::array<bool, 2> flip = {false, false},
                     std::array<float, 4> color = {1, 1, 1, 1}) {
        return nlohmann::json{{"name", "sprite_view"}, {"texture", std::move(texture)}, {"size", size},
                              {"pivot", {0.5, 0.5}}, {"flip", flip}, {"layer", layer}, {"color", color}};
    };
    auto default_sprite = [](std::string texture, int layer = 0,
                             std::string billboard = "none") {
        return nlohmann::json{{"name", "sprite_view"}, {"texture", std::move(texture)},
                              {"pivot", {0.5, 0.5}}, {"flip", {false, false}},
                              {"layer", layer}, {"color", {1, 1, 1, 1}},
                              {"billboard", std::move(billboard)}};
    };
    const auto strict_camera = [&](std::uint32_t zoom, std::string sort = "z") {
        const auto xmag = static_cast<double>(golden_case.width) / (8.0 * zoom);
        const auto ymag = static_cast<double>(golden_case.height) / (8.0 * zoom);
        return nlohmann::json{{"name", "pixel_camera"}, {"components", {
            nlohmann::json{{"name", "camera"}, {"type", "orthographic"},
                           {"xmag", xmag}, {"ymag", ymag},
                           {"znear", 0.1}, {"zfar", 20.0},
                           {"sprite", {{"pixel_perfect", "strict"}, {"sort", std::move(sort)}}}}
        }}};
    };
    nlohmann::json objects = nlohmann::json::array();
    if (mode == "sprite_ortho_atlas") {
        objects.push_back({{"name", "ortho"}, {"components", {
            nlohmann::json{{"name", "camera"}, {"type", "orthographic"}, {"xmag", 2.0},
                           {"ymag", 2.0}, {"znear", 0.1}, {"zfar", 20.0}}
        }}});
        objects.push_back({{"name", "base"}, {"components", {transform({-0.55f, 0, 0}), sprite("atlas#sprite/page0", {1.4f, 1.4f}, -1)}}});
        objects.push_back({{"name", "flip"}, {"components", {transform({0.55f, 0, 0}), sprite("atlas#sprite/page0", {1.4f, 1.4f}, 1, {true, false}, {0.7f, 0.8f, 1.0f, 0.8f})}}});
        objects.push_back({{"name", "tint"}, {"components", {transform({0, 0.55f, 0.1f}), sprite("atlas#sprite/green", {0.8f, 0.8f}, 2, {false, true}, {1.0f, 0.45f, 0.65f, 0.75f})}}});
    } else if (mode == "sprite_multi_atlas") {
        objects.push_back({{"name", "page0"}, {"components", {transform({-0.55f, 0, 0}), sprite("atlas#sprite/page0", {1.1f, 1.4f})}}});
        objects.push_back({{"name", "page1"}, {"components", {transform({0.55f, 0, 0}), sprite("atlas#sprite/page1", {1.1f, 1.4f})}}});
    } else if (mode == "sprite_parent") {
        objects.push_back({{"name", "parent"}, {"components", {transform({0, 0, 0}, {0, 0, 0.38268343f, 0.92387953f})}}});
        objects.push_back({{"name", "child"}, {"parent", "parent"},
                           {"components", {transform({0.55f, 0, 0}), sprite("atlas#sprite/page1", {1.0f, 0.65f})}}});
    } else if (mode == "sprite_depth") {
        objects.push_back({{"name", "occluder"}, {"components", {transform({0, 0, 0.7f}, {0, 0, 0, 1}, {0.65f, 0.65f, 0.65f}),
                                                                            nlohmann::json{{"name", "simplemodelview"}, {"model", "ground"}}}}});
        objects.push_back({{"name", "behind"}, {"components", {transform({0, 0, 0}), sprite("atlas#sprite/page0", {1.8f, 1.8f})}}});
        objects.push_back({{"name", "front"}, {"components", {transform({0.75f, -0.55f, 1.2f}), sprite("atlas#sprite/page1", {0.6f, 0.6f}, 2)}}});
    } else if (mode == "sprite_pixel_zoom1") {
        objects.push_back(strict_camera(1, "declaration"));
        objects.push_back({{"name", "fractional_default"}, {"components", {
            transform({0.17f, -0.21f, 0.0f}), default_sprite("atlas#sprite/pixel_even")}}});
        objects.push_back({{"name", "explicit_size"}, {"components", {
            transform({-0.73f, 0.48f, 0.1f}),
            sprite("atlas#sprite/pixel_tiny", {0.5f, 0.75f}, 1)}}});
    } else if (mode == "sprite_pixel_zoom2_odd") {
        objects.push_back(strict_camera(2, "y_down"));
        objects.push_back({{"name", "odd_atlas_edge"}, {"components", {
            transform({0.12f, -0.04f, 0.0f}),
            sprite("atlas#sprite/pixel_odd_edge", {0.75f, 1.25f})}}});
    } else if (mode == "sprite_pixel_zoom3_policy") {
        objects.push_back(strict_camera(3));
        objects.push_back({{"name", "integer_non_uniform"}, {"components", {
            transform({-0.34f, 0.18f, 0.0f}, {0, 0, 0, 1}, {2.0f, 1.0f, 1.0f}),
            default_sprite("atlas#sprite/pixel_dot")}}});
        objects.push_back({{"name", "fractional_scale"}, {"components", {
            transform({0.32f, -0.28f, 0.1f}, {0, 0, 0, 1}, {1.5f, 1.0f, 1.0f}),
            default_sprite("atlas#sprite/pixel_dot", 1)}}});
        objects.push_back({{"name", "rotated"}, {"components", {
            transform({0.28f, 0.28f, 0.2f}, {0, 0, 0.25881905f, 0.96592583f}),
            default_sprite("atlas#sprite/pixel_tiny", 2)}}});
    } else if (mode == "sprite_billboard") {
        objects.push_back(strict_camera(1));
        objects.push_back({{"name", "full_billboard"}, {"components", {
            transform({0, 0, 0}), default_sprite("atlas#sprite/pixel_even", 0, "full")}}});
    } else {
        objects.push_back({{"name", "owned"}, {"components", {transform({0, 0, 0}), sprite("atlas#sprite/page1", {1.35f, 1.35f})}}});
    }
    writeTextFile(root / "scene.json", nlohmann::json{{"schema", "pelican.scene"}, {"version", 1},
        {"scenes", {{"default_scene", {{"objects", objects}}}}}}.dump(2));

    const bool with_ui = mode == "sprite_ownership";
    writeTextFile(root / "ui/ui.json", with_ui
        ? R"json({"schema":"pelican.ui","version":1,"key":"sprite_ui","root":{"id":"root","type":"panel","children":[{"id":"tag","type":"image","sprite":"assets/atlas.json#sprite/page1","sampler":"nearest","color":[255,255,255,210],"layout":{"x":{"mode":"fixed","value":4},"y":{"mode":"fixed","value":4},"offsets":[0,0,8,8]}}]}})json"
        : R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
    writeTextFile(root / "shaders/fullscreen.vert", stemFullscreenVertexShader());
    writeTextFile(root / "shaders/white.frag", R"glsl(#version 450
layout(location=0) out vec4 outColor;
void main(){outColor=vec4(1.0);})glsl");
    auto rendering = makeShadowRenderingConfig(false);
    rendering["features"] = nlohmann::json::array({"engine://features/sprite.json"});
    if (with_ui) rendering["features"].push_back("engine://features/ui.json");
    if (gpu_timing) rendering["features"].push_back("engine://features/gpu_timing.json");
    writeTextFile(root / "passes/main.json", rendering.dump(2));
    return project;
}

void writeComputeProject(const std::filesystem::path &root,
                         bool gpu_timing = false) {
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
    writeTextFile(root / "ui" / "ui.json", R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
    writeTextFile(root / "shaders" / "fullscreen.vert", stemFullscreenVertexShader());
    writeTextFile(root / "shaders" / "write_color.comp", computeWriteColorShader());
    writeTextFile(root / "shaders" / "build_dispatch.comp", R"glsl(#version 450
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
layout(std430,set=1,binding=0) buffer DispatchArguments {
    uint x;
    uint y;
    uint z;
} dispatch_arguments;
void main(){
    dispatch_arguments.x=1;
    dispatch_arguments.y=1;
    dispatch_arguments.z=1;
})glsl");
    writeTextFile(root / "shaders" / "compute_present.frag", computeBufferFragmentShader());
    writeTextFile(root / "passes" / "main.json", R"json({
  "buffers": [
    {"name": "compute_color", "size": 16, "lifetime": "persistent"},
    {
      "name": "dispatch_arguments",
      "size": 12,
      "lifetime": "persistent",
      "command_layout": "compute_dispatch"
    }
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
      "name": "build_dispatch",
      "shader": "shaders/build_dispatch",
      "writes": ["dispatch_arguments"],
      "dispatch": {"groups": [1, 1, 1]},
      "schedule": "per_frame"
    },
    {
      "name": "write_color",
      "shader": "shaders/write_color",
      "writes": ["compute_color"],
      "before": ["present"],
      "dispatch": {
        "indirect": {
          "buffer": "dispatch_arguments"
        }
      },
      "schedule": "per_frame"
    }
  ]
})json");
    if (gpu_timing) {
        std::ifstream stream{root / "passes" / "main.json"};
        auto config = nlohmann::json::parse(stream);
        stream.close();
        config["features"] = nlohmann::json::array(
            {"engine://features/gpu_timing.json"});
        writeTextFile(root / "passes" / "main.json", config.dump(2));
    }
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
    writeTextFile(root / "ui" / "ui.json", R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
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
    writeTextFile(root / "ui" / "ui.json", R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
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

void renderSkeletalToonFrame(RenderTarget &render_target) {
#if PELICAN_RUNTIME_SHADER_COMPILER
    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    time.setTime(0.5);

    const auto example = sourceRoot() / "projects" / "example";
    std::ifstream surface_file{example / "shaders" / "toon.surface", std::ios::binary};
    const std::string surface_source{std::istreambuf_iterator<char>{surface_file},
                                     std::istreambuf_iterator<char>{}};
    const auto surface = parseSurfaceFormat(surface_source, "project://shaders/toon.surface");
    const auto bundles = GET_MODULE(ShaderLibrary).loadFromSurface(
        surface, "project://shaders/toon.surface", SurfacePass::main, {"PELICAN_SKINNED"});

    std::ifstream material_file{example / "materials" / "toon.material.json", std::ios::binary};
    const auto material_json = nlohmann::json::parse(material_file);
    MaterialSurfaceCatalog catalog;
    catalog.emplace("project://shaders/toon.surface", surface);
    const auto authored = parseMaterialFormatJson(material_json, catalog).materials.front();
    const auto lowered = lowerMaterial(authored, surface);
    auto &standard = GET_MODULE(StandardMaterialResource);
    MaterialInfo info{
        .vert_shader = bundles.vertex,
        .frag_shader = bundles.fragment,
        .skinned = true,
        .base_color_texture = standard.whiteTexture(),
        .metallic_roughness_texture = standard.metallicRoughnessDefaultTexture(),
        .normal_texture = standard.normalDefaultTexture(),
        .emissive_texture = standard.emissiveDefaultTexture(),
    };
    applyLoweredMaterial(info, lowered);
    info.render_state.cull = SurfaceCullMode::none;
    const auto toon_material = GET_MODULE(MaterialContainer).registerMaterial(std::move(info));
    auto &model = GET_MODULE(ModelAssetContainer).getModelTemplateByName("character");
    for (auto &group : model.material_primitives) group.material = toon_material;

    GET_MODULE(ECSPredefinedRegistration).reg();
    GET_MODULE(SceneLoader).load("default_scene");
    GET_MODULE(ECSCore).update();

    auto &camera = GET_MODULE(Camera);
    const glm::vec3 position{0.0f, 1.0f, -2.5f};
    const glm::vec3 target{0.25f, 1.0f, 0.0f};
    camera.setPos(position);
    camera.setDir(glm::normalize(target - position));
    camera.setUp({0.0f, 1.0f, 0.0f});
    GET_MODULE(Renderer).render();
    GET_MODULE(VulkanManageCore).waitIdle();
    (void)render_target;
#else
    (void)render_target;
    throw std::runtime_error("runtime shader compiler disabled");
#endif
}

void renderUsdStaticGeometryFrame(RenderTarget &render_target) {
    GET_MODULE(ECSPredefinedRegistration).reg();
    GET_MODULE(SceneLoader).load("default_scene");
    GET_MODULE(ECSCore).update();
    GET_MODULE(ECSCore).update();

    auto &camera = GET_MODULE(Camera);
    const glm::vec3 position{0.5f, 0.5f, 2.0f};
    const glm::vec3 target{0.5f, 0.5f, 0.0f};
    camera.setPos(position);
    camera.setDir(glm::normalize(target - position));
    camera.setUp({0.0f, 1.0f, 0.0f});
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

void renderBLayerShadowFrame(RenderTarget &render_target,
                             std::string_view mode) {
#if PELICAN_RUNTIME_SHADER_COMPILER
    const bool shadow_enabled =
        mode != "shadow_b_layer_off";
    const auto expected_provider =
        mode == "shadow_b_layer_project"
            ? std::string{
                  "project://features/shadow_directional.json"}
            : std::string{
                  "engine://features/shadow_directional.json"};

    const auto rendering_pass_id =
        GET_MODULE(RenderingPassContainer)
            .getRenderingPassIdByName("main_render");
    const auto execution =
        GET_MODULE(FrameGraphRuntimeContainer).find(
            rendering_pass_id);
    if (execution == nullptr ||
        execution->render_pipeline == nullptr) {
        throw std::runtime_error(
            "B-layer shadow golden requires a compiled render pipeline");
    }

    const auto &pipeline = *execution->render_pipeline;
    const bool has_shadow_define =
        std::find(pipeline.shader_defines.begin(),
                  pipeline.shader_defines.end(),
                  "PELICAN_FEATURE_SHADOW") !=
        pipeline.shader_defines.end();
    if (has_shadow_define != shadow_enabled) {
        throw std::runtime_error(
            "B-layer shadow feature define does not match the golden mode");
    }
    if (!shadow_enabled) {
        if (!pipeline.surface_resource_contracts.empty()) {
            throw std::runtime_error(
                "feature-off B-layer shadow golden unexpectedly "
                "compiled a surface resource");
        }
    } else {
        if (pipeline.surface_resource_contracts.size() != 1) {
            throw std::runtime_error(
                "B-layer shadow golden requires exactly one typed "
                "surface resource");
        }
        const auto &resource =
            pipeline.surface_resource_contracts.front();
        if (resource.contract.name !=
                "directional_shadow" ||
            resource.provider_reference !=
                expected_provider) {
            throw std::runtime_error(
                "B-layer shadow golden compiled the wrong typed "
                "surface resource provider");
        }
    }

    const auto surface_reference = std::string{
        "engine://surfaces/openpbr/opaque_single.surface"};
    const auto surface = parseSurfaceFormat(
        engineResourceOrThrow(
            "surfaces/openpbr/opaque_single.surface"),
        surface_reference);
    MaterialSurfaceCatalog surfaces{
        {surface_reference, surface}};
    const auto material_document =
        parseMaterialFormatJson(
            nlohmann::json::parse(R"json({
              "schema":"pelican.material",
              "version":1,
              "materials":[{
                "name":"shadow_receiver",
                "surface":"engine://surfaces/openpbr/opaque_single.surface",
                "values":{
                  "base_color":[0.62,0.68,0.78,1.0],
                  "base_diffuse_roughness":0.72,
                  "coat_weight":0.35
                },
                "routing":{
                  "alpha_mode":"opaque",
                  "double_sided":false
                }
              }]
            })json"),
            surfaces);
    const auto lowered = lowerMaterial(
        material_document.materials.front(), surface);
    if (lowered.route !=
        MaterialRouteClass::forward_opaque) {
        throw std::runtime_error(
            "B-layer shadow golden material did not route to "
            "forward_opaque");
    }

    const auto shaders =
        GET_MODULE(ShaderLibrary)
            .loadFromSurfaceForMaterial(
                surface, surface_reference, lowered,
                pipeline.shader_defines);
    auto &standard =
        GET_MODULE(StandardMaterialResource);
    MaterialInfo material{
        .vert_shader = shaders.vertex,
        .frag_shader = shaders.fragment,
        .base_color_texture = standard.whiteTexture(),
        .metallic_roughness_texture =
            standard.metallicRoughnessDefaultTexture(),
        .normal_texture = standard.normalDefaultTexture(),
        .emissive_texture =
            standard.emissiveDefaultTexture(),
    };
    applyLoweredMaterialForRoute(material, lowered);
    const auto material_id =
        GET_MODULE(MaterialContainer).registerMaterial(
            std::move(material));
    if (!isValidMaterialId(material_id)) {
        throw std::runtime_error(
            "failed to register B-layer shadow golden material");
    }

    auto &model =
        GET_MODULE(ModelAssetContainer)
            .getModelTemplateByName("ground");
    if (model.material_primitives.empty()) {
        throw std::runtime_error(
            "B-layer shadow golden model has no material primitives");
    }
    for (auto &group : model.material_primitives) {
        group.material = material_id;
    }

    renderShadowFrame(render_target);
#else
    (void)render_target;
    (void)mode;
    throw std::runtime_error(
        "runtime shader compiler disabled");
#endif
}

void renderMaterialInstanceOverrideFrame(RenderTarget &render_target) {
    GET_MODULE(ECSPredefinedRegistration).reg();
    GET_MODULE(SceneLoader).load("default_scene");
    GET_MODULE(ECSCore).update();
    GET_MODULE(ECSCore).update();

    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto left_model = instances.modelInstanceIdForTesting(0);
    const auto right_model = instances.modelInstanceIdForTesting(1);
    PublishMaterialInstanceOverrideDescV1 left;
    left.instance = instances.animationInstance(left_model);
    left.frame_revision = 1;
    left.values.mask = materialOverrideBaseColor | materialOverrideEmissive |
                       materialOverrideUvTransform;
    left.values.base_color_factor = {4.0f, 0.15f, 0.1f, 1.0f};
    left.values.emissive_factor = {1.0f, 0.2f, 0.2f, 1.0f};
    left.values.uv_offset = {0.25f, 0.0f};
    left.values.uv_scale = {0.75f, 1.0f};
    left.values.uv_rotation = 0.2f;
    REQUIRE(instances.publishMaterialInstanceOverride(left_model, left) ==
            Animation::Status::ok);

    auto right = left;
    right.instance = instances.animationInstance(right_model);
    right.values.base_color_factor = {0.1f, 1.4f, 0.15f, 1.0f};
    right.values.emissive_factor = {0.2f, 1.0f, 0.2f, 1.0f};
    right.values.uv_offset = {-0.125f, 0.125f};
    right.values.uv_scale = {1.5f, 0.5f};
    right.values.uv_rotation = -0.35f;
    REQUIRE(instances.publishMaterialInstanceOverride(right_model, right) ==
            Animation::Status::ok);

    auto &camera = GET_MODULE(Camera);
    camera.setPos({0.0f, 0.0f, 3.2f});
    camera.setDir({0.0f, 0.0f, -1.0f});
    camera.setUp({0.0f, 1.0f, 0.0f});
    GET_MODULE(Renderer).render();
    GET_MODULE(VulkanManageCore).waitIdle();
    (void)render_target;
}

void renderMaterialAbsoluteOverrideFrame(RenderTarget &render_target) {
    GET_MODULE(ECSPredefinedRegistration).reg();
    GET_MODULE(SceneLoader).load("default_scene");
    GET_MODULE(ECSCore).update();
    GET_MODULE(ECSCore).update();

    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto bottom_model = instances.modelInstanceIdForTesting(0);
    const auto top_model = instances.modelInstanceIdForTesting(1);
    PublishMaterialInstanceAbsoluteOverrideDescV2 bottom;
    bottom.instance = instances.animationInstance(bottom_model);
    bottom.frame_revision = 1;
    bottom.source_material_index = 0;
    bottom.values.mask = materialOverrideAll;
    bottom.values.base_color_factor = {1.0f, 0.03f, 0.02f, 1.0f};
    bottom.values.emissive_factor = {0.0f, 0.0f, 0.0f, 1.0f};
    bottom.values.uv_offset = {0.2f, 0.0f};
    bottom.values.uv_scale = {0.8f, 1.0f};
    REQUIRE(instances.publishMaterialInstanceAbsoluteOverride(
                bottom_model, bottom) == Animation::Status::ok);

    auto top = bottom;
    top.instance = instances.animationInstance(top_model);
    top.values.base_color_factor = {0.02f, 0.08f, 1.0f, 1.0f};
    top.values.uv_offset = {-0.1f, 0.1f};
    top.values.uv_scale = {1.2f, 0.7f};
    REQUIRE(instances.publishMaterialInstanceAbsoluteOverride(
                top_model, top) == Animation::Status::ok);
    REQUIRE(instances.materialAbsoluteOverrideFrameForTesting(
                bottom_model, 1) == nullptr);
    REQUIRE(instances.materialAbsoluteOverrideFrameForTesting(
                top_model, 1) == nullptr);

    auto &camera = GET_MODULE(Camera);
    camera.setPos({0.0f, 0.0f, 3.2f});
    camera.setDir({0.0f, 0.0f, -1.0f});
    camera.setUp({0.0f, 1.0f, 0.0f});
    GET_MODULE(Renderer).render();
    GET_MODULE(VulkanManageCore).waitIdle();
    (void)render_target;
}

Animation::InstanceHandle resolveExpressionInstance(
    Animation::AnimationServiceV1 &service, std::string_view object_name) {
    Animation::ResolveAnimationSinkDescV1 sink{};
    sink.struct_size = sizeof(sink);
    sink.version = Animation::descriptorVersionV1;
    sink.object_name = object_name.data();
    sink.object_name_size = static_cast<std::uint32_t>(object_name.size());
    sink.sink_kind = Animation::AnimationSinkKind::skeletal_pose;
    REQUIRE(service.resolve_sink(service.context, &sink) ==
            Animation::Status::ok);
    Animation::ResolveAnimationInstanceDescV1 instance{};
    instance.struct_size = sizeof(instance);
    instance.version = Animation::descriptorVersionV1;
    instance.sink = sink.sink;
    REQUIRE(service.resolve_instance(service.context, &instance) ==
            Animation::Status::ok);
    return instance.instance;
}

void setGoldenExpressionInput(Vrm::ApplicationServiceV1 &service,
                              Animation::InstanceHandle instance,
                              float happy, float yaw) {
    Vrm::ExpressionWeightV1 weight{};
    constexpr char name[] = "happy";
    weight.name = name;
    weight.name_size = sizeof(name) - 1;
    weight.value = happy;
    Vrm::SetExpressionInputDescV1 input{};
    input.struct_size = sizeof(input);
    input.version = Vrm::applicationDescriptorVersionV1;
    input.instance = instance;
    input.weights = &weight;
    input.weight_count = 1;
    input.input_revision = 1;
    input.look_at_yaw_degrees = yaw;
    input.flags = Vrm::expression_input_look_at;
    REQUIRE(service.set_expression_inputs(service.context, &input) ==
            Animation::Status::ok);
}

void renderVrmExpressionFrame(RenderTarget &render_target, bool enabled) {
    Vrm::applicationServiceRuntime().reset();
    Animation::animationServiceRuntime().reset();
    GET_MODULE(ECSPredefinedRegistration).reg();
    GET_MODULE(SceneLoader).load("default_scene");
    GET_MODULE(ECSCore).update();
    GET_MODULE(ECSCore).update();

    Vrm::ApplicationServiceV1 application{};
    application.struct_size = sizeof(application);
    application.version = Vrm::applicationDescriptorVersionV1;
    REQUIRE(Vrm::getApplicationServiceV1(Vrm::applicationServiceVersionV1,
                                         &application) ==
            Animation::Status::ok);
    Animation::ApiV1 api{};
    api.struct_size = sizeof(api);
    api.version = Animation::descriptorVersionV1;
    REQUIRE(Animation::getApiV1(Animation::abiVersionV1, &api) ==
            Animation::Status::ok);
    Animation::AnimationServiceV1 animation{};
    animation.struct_size = sizeof(animation);
    animation.version = Animation::descriptorVersionV1;
    REQUIRE(api.get_animation_service(api.context,
                                      Animation::animationServiceVersionV1,
                                      &animation) == Animation::Status::ok);
    const auto left = resolveExpressionInstance(animation, "ExpressionLeft");
    const auto right = resolveExpressionInstance(animation, "ExpressionRight");
    setGoldenExpressionInput(application, left, enabled ? 0.25f : 0.0f,
                             enabled ? -22.5f : 0.0f);
    setGoldenExpressionInput(application, right, enabled ? 1.0f : 0.0f,
                             enabled ? 22.5f : 0.0f);

    constexpr std::uint64_t revision = 123;
    REQUIRE(Animation::animationServiceRuntime().runAllPhases(revision) ==
            Animation::Status::ok);
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto left_model = instances.modelInstanceIdForTesting(
        static_cast<std::uint32_t>(left.identity - 1));
    const auto right_model = instances.modelInstanceIdForTesting(
        static_cast<std::uint32_t>(right.identity - 1));
    const auto &left_morph =
        instances.morphWeightFrameForTesting(left_model);
    const auto &right_morph =
        instances.morphWeightFrameForTesting(right_model);
    REQUIRE(left_morph.current_revision == revision);
    REQUIRE(right_morph.current_revision == revision);
    REQUIRE(left_morph.current.size() == 1);
    REQUIRE(right_morph.current.size() == 1);
    if (enabled) REQUIRE(left_morph.current != right_morph.current);
    const auto *left_material = instances.materialAbsoluteOverrideFrameForTesting(
        left_model, 0);
    const auto *right_material = instances.materialAbsoluteOverrideFrameForTesting(
        right_model, 0);
    REQUIRE(left_material != nullptr);
    REQUIRE(right_material != nullptr);
    REQUIRE(left_material->current_revision == revision);
    REQUIRE(right_material->current_revision == revision);
    if (enabled)
        REQUIRE(left_material->current.base_color_factor !=
                right_material->current.base_color_factor);

    auto &camera = GET_MODULE(Camera);
    camera.setPos({0.0f, 0.0f, 3.2f});
    camera.setDir({0.0f, 0.0f, -1.0f});
    camera.setUp({0.0f, 1.0f, 0.0f});
    GET_MODULE(Renderer).render();
    GET_MODULE(VulkanManageCore).waitIdle();
    Vrm::applicationServiceRuntime().reset();
    Animation::animationServiceRuntime().reset();
    (void)render_target;
}

void renderSpriteFrame(RenderTarget &render_target, std::string_view mode) {
    GET_MODULE(ECSPredefinedRegistration).reg();
    GET_MODULE(SceneLoader).load("default_scene");
    GET_MODULE(ECSCore).update();
    GET_MODULE(ECSCore).update();
    auto &camera = GET_MODULE(Camera);
    if (mode == "sprite_billboard") {
        camera.setPos({2.0f, 1.0f, 4.0f});
        camera.setDir(glm::normalize(glm::vec3{-2.0f, -1.0f, -4.0f}));
    } else if (mode == "sprite_pixel_zoom2_odd") {
        camera.setPos({0.03125f, -0.0625f, 4.0f});
        camera.setDir({0.0f, 0.0f, -1.0f});
    } else {
        camera.setPos({0.0f, 0.0f, 4.0f});
        camera.setDir({0.0f, 0.0f, -1.0f});
    }
    camera.setUp({0.0f, 1.0f, 0.0f});
    GET_MODULE(ECSCore).update();
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

void renderParentTransformFrame(RenderTarget &render_target) {
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &scene = GET_MODULE(SceneLoader);
    scene.load("default_scene");
    GET_MODULE(ECSCore).update();
    const auto child = scene.objectTransform("OrbitingChild");

    auto &debug_draw = GET_MODULE(DebugDraw);
    debug_draw.line({0.0f, 0.0f, 0.0f}, child.pos, {0.15f, 0.55f, 1.0f, 1.0f});
    debug_draw.line(child.pos + glm::vec3{-0.12f, 0.0f, 0.0f},
                    child.pos + glm::vec3{0.12f, 0.0f, 0.0f},
                    {1.0f, 0.15f, 0.05f, 1.0f});
    debug_draw.line(child.pos + glm::vec3{0.0f, -0.12f, 0.0f},
                    child.pos + glm::vec3{0.0f, 0.12f, 0.0f},
                    {1.0f, 0.15f, 0.05f, 1.0f});
    GET_MODULE(Renderer).render();
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

void renderTemporalAccumulationFrames(RenderTarget &render_target) {
    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    for (int i = 0; i < 3; ++i) {
        time.advance();
        GET_MODULE(Renderer).render();
    }
    GET_MODULE(VulkanManageCore).waitIdle();
    (void)render_target;
}

bool isHdrGoldenMode(const std::string &mode) {
    return mode == "hdr_off" || mode == "hdr_on";
}

bool isShadowGoldenMode(const std::string &mode) {
    return mode == "shadow_off" || mode == "shadow_on";
}

bool isGpuDrawGoldenMode(
    const std::string &mode) {
    return mode == "gpu_draw_count_zero" ||
           mode == "gpu_draw_count_one" ||
           mode == "gpu_draw_count_max" ||
           mode == "gpu_draw_count_overflow" ||
           mode == "gpu_draw_force_cpu";
}

bool isGpuOcclusionGoldenMode(
    const std::string &mode) {
    return mode ==
               "gpu_occlusion_hidden" ||
           mode ==
               "gpu_occlusion_visible" ||
           mode ==
               "gpu_occlusion_force_cpu";
}

std::uint32_t gpuDrawProducedCount(
    const std::string &mode) {
    if (mode == "gpu_draw_count_zero" ||
        mode == "gpu_draw_force_cpu") {
        return 0;
    }
    if (mode == "gpu_draw_count_one") {
        return 1;
    }
    if (mode == "gpu_draw_count_max") {
        return 2;
    }
    if (mode == "gpu_draw_count_overflow") {
        return 99;
    }
    throw std::runtime_error(
        "unknown GPU draw golden mode: " +
        mode);
}

bool isBLayerShadowGoldenMode(
    const std::string &mode) {
    return mode == "shadow_b_layer_off" ||
           mode == "shadow_b_layer_engine" ||
           mode == "shadow_b_layer_project";
}

bool isTaaGoldenMode(const std::string &mode) {
    return mode == "taa_static" || mode == "taa_camera_motion" ||
           mode == "taa_object_motion" || mode == "taa_disocclusion" ||
           mode == "taa_resize" || mode == "taa_set_time" || mode == "taa_ortho";
}

void renderTaaGoldenFrames(RenderTarget &render_target, const std::string &mode) {
    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    time.setTime(0.0);
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &scene = GET_MODULE(SceneLoader);
    scene.load("default_scene");
    GET_MODULE(ECSCore).update();
    GET_MODULE(ECSCore).update();

    auto &camera = GET_MODULE(Camera);
    const glm::vec3 initial_position{0.0f, 2.0f, -4.5f};
    const glm::vec3 initial_target{0.0f, 0.25f, 0.0f};
    camera.setPos(initial_position);
    camera.setDir(glm::normalize(initial_target - initial_position));
    camera.setUp({0.0f, 1.0f, 0.0f});

    auto &renderer = GET_MODULE(Renderer);
    const auto renderFrame = [&] {
        time.advance();
        GET_MODULE(ECSCore).update();
        renderer.render();
    };
    const auto renderFrames = [&](int count) {
        for (int i = 0; i < count; ++i) {
            renderFrame();
        }
    };

    if (mode == "taa_camera_motion") {
        renderFrames(4);
        const glm::vec3 moved_position{0.35f, 2.0f, -4.5f};
        camera.setPos(moved_position);
        camera.setDir(glm::normalize(initial_target - moved_position));
        renderFrames(4);
    } else if (mode == "taa_object_motion") {
        renderFrames(4);
        auto transform = scene.objectTransform("Caster");
        transform.pos.x += 0.65f;
        scene.applyObjectTransform("Caster", transform);
        renderFrames(4);
    } else if (mode == "taa_disocclusion") {
        renderFrames(4);
        auto transform = scene.objectTransform("Caster");
        transform.pos.x = 3.0f;
        scene.applyObjectTransform("Caster", transform);
        renderFrames(1);
    } else if (mode == "taa_resize") {
        renderFrames(4);
        GET_MODULE(VulkanManageCore).waitIdle();
        renderer.recreateRenderTargetsAndRebindForTesting(
            {goldenWidth / 2, goldenHeight / 2});
        renderer.recreateRenderTargetsAndRebindForTesting({goldenWidth, goldenHeight});
        renderFrames(1);
    } else if (mode == "taa_set_time") {
        renderFrames(4);
        time.setTime(3.0);
        renderFrames(1);
    } else {
        renderFrames(8);
    }
    GET_MODULE(VulkanManageCore).waitIdle();
    (void)render_target;
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
           golden_case.mode != "triangle" && golden_case.mode != "surface_toon" &&
           golden_case.mode != "openpbr_coat_sphere";
}

RenderedCase renderCase(const GoldenCase &golden_case, bool gpu_labels = false,
                        bool gpu_timing = true) {
    FastModuleContainer modules;
    const auto temp_dir = makeTempProjectDir(golden_case.name);
    if (golden_case.mode == "stem_fullscreen") {
        writeStemProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeStemProjectJson().dump());
    } else if (golden_case.mode == "snapshot_refraction") {
        writeSnapshotRefractionProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        auto project = makeStemProjectJson();
        project["name"] = "snapshot refraction golden";
        GET_MODULE(ProjectSource).setProjectData(project.dump());
    } else if (golden_case.mode == "explicit_order") {
        writeExplicitOrderProject(temp_dir, gpu_timing);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeFeatureProjectJson().dump());
    } else if (golden_case.mode == "vat_playback") {
        writeVatProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeVatProjectJson().dump());
    } else if (golden_case.mode == "skeletal_toon") {
        writeSkeletalProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        auto project = makeVatProjectJson();
        project["basic_config"]["scene_data_json"] = "scene.json";
        project["basic_config"]["asset_data_json"] = "assets.json";
        project["basic_config"]["ui_config_json"] = "ui/ui.json";
        project["basic_config"]["rendering_config_json"] = "passes/main.json";
        project["basic_config"]["default_rendering_pass"] = "main_render";
        GET_MODULE(ProjectSource).setProjectData(project.dump());
    } else if (golden_case.mode == "usd0b_static_geometry") {
        writeUsdStaticGeometryProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        auto project = makeVatProjectJson();
        project["name"] = "U-USD0b static geometry golden";
        project["basic_config"]["scene_data_json"] = "scene.json";
        project["basic_config"]["asset_data_json"] = "assets.json";
        project["basic_config"]["ui_config_json"] = "ui/ui.json";
        project["basic_config"]["rendering_config_json"] = "passes/main.json";
        project["basic_config"]["default_rendering_pass"] = "main_render";
        GET_MODULE(ProjectSource).setProjectData(project.dump());
    } else if (golden_case.mode == "morph_skinned_shadow") {
        writeMorphSkinnedShadowProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeShadowProjectJson().dump());
    } else if (golden_case.mode == "usd0c_openpbr_material") {
        writeUsdOpenPbrMaterialProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        auto project = makeVatProjectJson();
        project["name"] = "U-USD0c generated OpenPBR material golden";
        project["basic_config"]["scene_data_json"] = "scene.json";
        project["basic_config"]["asset_data_json"] = "assets.json";
        project["basic_config"]["ui_config_json"] = "ui/ui.json";
        project["basic_config"]["rendering_config_json"] = "passes/main.json";
        project["basic_config"]["default_rendering_pass"] = "main_render";
        GET_MODULE(ProjectSource).setProjectData(project.dump());
    } else if (golden_case.mode == "material_instance_override") {
        writeMaterialInstanceOverrideProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeShadowProjectJson().dump());
    } else if (golden_case.mode == "vrm_expression_off" ||
               golden_case.mode == "vrm_expression_on") {
        writeVrmExpressionProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeShadowProjectJson().dump());
    } else if (golden_case.mode == "vrm_xr_demo") {
        writeVrmXrDemoGoldenProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeShadowProjectJson().dump());
    } else if (golden_case.mode == "material_absolute_override") {
        writeMaterialAbsoluteOverrideProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeShadowProjectJson().dump());
    } else if (golden_case.mode == "feature_compose") {
        writeFeatureProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeFeatureProjectJson().dump());
    } else if (golden_case.mode == "temporal_accumulation") {
        writeTemporalAccumulationProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        auto project = makeFeatureProjectJson();
        project["name"] = "temporal accumulation golden";
        GET_MODULE(ProjectSource).setProjectData(project.dump());
    } else if (golden_case.mode == "ui_u1") {
        writeUiU1Project(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        auto project = makeFeatureProjectJson();
        project["name"] = "ui u1 golden";
        GET_MODULE(ProjectSource).setProjectData(project.dump());
    } else if (golden_case.mode == "ui_u2") {
        writeUiU2Project(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        auto project = makeFeatureProjectJson();
        project["name"] = "ui u2 golden";
        GET_MODULE(ProjectSource).setProjectData(project.dump());
    } else if (isSpriteGoldenMode(golden_case.mode)) {
        const auto project = writeSpriteProject(temp_dir, golden_case);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(project.dump());
    } else if (golden_case.mode == "debug_draw_feature") {
        writeDebugDrawProject(temp_dir);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeFeatureProjectJson().dump());
    } else if (golden_case.mode == "parent_transform") {
        writeParentTransformProject(temp_dir);
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
    } else if (isBLayerShadowGoldenMode(
                   golden_case.mode)) {
        writeBLayerShadowProject(temp_dir,
                                 golden_case.mode);
        GET_MODULE(PathResolver).setup(temp_dir, false);
        auto project = makeShadowProjectJson();
        project["basic_config"]
               ["default_rendering_pass"] =
            "main_render";
        GET_MODULE(ProjectSource).setProjectData(
            project.dump());
    } else if (isShadowGoldenMode(golden_case.mode)) {
        writeShadowProject(temp_dir, golden_case.mode == "shadow_on");
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(makeShadowProjectJson().dump());
    } else if (isGpuDrawGoldenMode(
                   golden_case.mode)) {
        writeGpuDrawProject(
            temp_dir,
            gpuDrawProducedCount(
                golden_case.mode),
            golden_case.mode ==
                "gpu_draw_force_cpu");
        GET_MODULE(PathResolver).setup(
            temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(
            makeShadowProjectJson().dump());
    } else if (isGpuOcclusionGoldenMode(
                   golden_case.mode)) {
        writeGpuOcclusionProject(
            temp_dir,
            golden_case.mode ==
                "gpu_occlusion_visible",
            golden_case.mode ==
                "gpu_occlusion_force_cpu");
        GET_MODULE(PathResolver).setup(
            temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(
            makeShadowProjectJson().dump());
    } else if (isTaaGoldenMode(golden_case.mode)) {
        writeTaaProject(temp_dir, golden_case.mode == "taa_ortho");
        GET_MODULE(PathResolver).setup(temp_dir, false);
        auto project = makeShadowProjectJson();
        project["name"] = "TAA golden";
        GET_MODULE(ProjectSource).setProjectData(project.dump());
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
    launch_config.gpu_labels = gpu_labels;
    launch_config.headless_extent = vk::Extent2D{golden_case.width, golden_case.height};
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
    } else if (golden_case.mode == "snapshot_refraction") {
        renderFeatureFrame(render_target);
    } else if (golden_case.mode == "triangle") {
        renderFullscreenFrame(render_target, triangleMaskFragmentShader());
    } else if (golden_case.mode == "surface_toon") {
        renderSurfaceToonFrame(render_target);
    } else if (golden_case.mode == "openpbr_coat_sphere") {
        renderOpenPbrCoatSphereFrame(render_target);
    } else if (golden_case.mode == "usd0c_openpbr_material") {
        renderUsdOpenPbrMaterialFrame(render_target, temp_dir);
    } else if (golden_case.mode == "explicit_order") {
        renderFeatureFrame(render_target);
    } else if (golden_case.mode == "vat_playback") {
        renderVatPlaybackFrame(render_target, temp_dir);
    } else if (golden_case.mode == "skeletal_toon") {
        renderSkeletalToonFrame(render_target);
    } else if (golden_case.mode == "usd0b_static_geometry") {
        renderUsdStaticGeometryFrame(render_target);
    } else if (golden_case.mode == "morph_skinned_shadow") {
        renderShadowFrame(render_target);
    } else if (golden_case.mode == "material_instance_override") {
        renderMaterialInstanceOverrideFrame(render_target);
    } else if (golden_case.mode == "vrm_xr_demo") {
        renderShadowFrame(render_target);
    } else if (golden_case.mode == "vrm_expression_off" ||
               golden_case.mode == "vrm_expression_on") {
        renderVrmExpressionFrame(render_target,
                                 golden_case.mode == "vrm_expression_on");
    } else if (golden_case.mode == "material_absolute_override") {
        renderMaterialAbsoluteOverrideFrame(render_target);
    } else if (golden_case.mode == "feature_compose") {
        renderFeatureFrame(render_target);
    } else if (golden_case.mode == "temporal_accumulation") {
        renderTemporalAccumulationFrames(render_target);
    } else if (golden_case.mode == "ui_u1" || golden_case.mode == "ui_u2") {
        renderFeatureFrame(render_target);
    } else if (isSpriteGoldenMode(golden_case.mode)) {
        renderSpriteFrame(render_target, golden_case.mode);
    } else if (golden_case.mode == "debug_draw_feature") {
        renderDebugDrawFrame(render_target);
    } else if (golden_case.mode == "parent_transform") {
        renderParentTransformFrame(render_target);
    } else if (golden_case.mode == "debug_text_feature") {
        renderDebugTextFrame(render_target);
    } else if (golden_case.mode == "collider_debug_draw") {
        renderColliderDebugDrawFrame(render_target);
    } else if (isHdrGoldenMode(golden_case.mode)) {
        renderDebugTextFrame(render_target);
    } else if (isBLayerShadowGoldenMode(
                   golden_case.mode)) {
        renderBLayerShadowFrame(render_target,
                                golden_case.mode);
    } else if (isShadowGoldenMode(golden_case.mode)) {
        renderShadowFrame(render_target);
    } else if (isGpuDrawGoldenMode(
                   golden_case.mode)) {
        renderShadowFrame(render_target);
    } else if (isGpuOcclusionGoldenMode(
                   golden_case.mode)) {
        renderShadowFrame(render_target);
    } else if (isTaaGoldenMode(golden_case.mode)) {
        renderTaaGoldenFrames(render_target, golden_case.mode);
    } else if (golden_case.mode == "compute_buffer") {
        renderComputeFrame(render_target);
    } else if (golden_case.mode == "orthographic_camera") {
        renderOrthographicCameraFrame(render_target);
    } else if (golden_case.mode == "orbit_camera_controller") {
        renderOrbitCameraControllerFrame(render_target);
    } else {
        throw std::runtime_error("unknown golden case mode: " + golden_case.mode);
    }

    if (golden_case.mode == "explicit_order" && gpu_timing) {
        GET_MODULE(RenderTiming).flush();
    }
    std::optional<std::uint32_t>
        gpu_visible_draw_count;
    std::optional<
        FrameGraphResourceContainer::
            HostBufferPopulation>
        gpu_draw_command_population;
    std::optional<
        FrameGraphResourceContainer::
            HostBufferPopulation>
        gpu_draw_bounds_population;
    if (isGpuOcclusionGoldenMode(
            golden_case.mode)) {
        auto &vkcore =
            GET_MODULE(VulkanManageCore);
        auto &resources =
            GET_MODULE(
                FrameGraphResourceContainer);
        const auto &source =
            resources.buffer(
                "visible_draw_count");
        auto staging = vkcore.allocBuf(
            sizeof(std::uint32_t),
            vk::BufferUsageFlagBits::
                eTransferDst,
            vma::MemoryUsage::
                eAutoPreferHost,
            vma::AllocationCreateFlagBits::
                eHostAccessRandom);
        GET_MODULE(VulkanUtils)
            .executeOneTimeCmd(
                [&](vk::CommandBuffer command) {
                    vk::BufferMemoryBarrier
                        barrier;
                    barrier.srcAccessMask =
                        vk::AccessFlagBits::
                            eShaderWrite;
                    barrier.dstAccessMask =
                        vk::AccessFlagBits::
                            eTransferRead;
                    barrier.srcQueueFamilyIndex =
                        VK_QUEUE_FAMILY_IGNORED;
                    barrier.dstQueueFamilyIndex =
                        VK_QUEUE_FAMILY_IGNORED;
                    barrier.buffer =
                        source.buffer.get();
                    barrier.offset = 0;
                    barrier.size =
                        sizeof(
                            std::uint32_t);
                    command.pipelineBarrier(
                        vk::PipelineStageFlagBits::
                            eComputeShader,
                        vk::PipelineStageFlagBits::
                            eTransfer,
                        {}, {}, {barrier}, {});
                    command.copyBuffer(
                        source.buffer.get(),
                        staging.buffer.get(),
                        vk::BufferCopy{
                            0, 0,
                            sizeof(
                                std::uint32_t)});
                },
                true);
        const auto bytes =
            vkcore.readBuf(
                staging,
                sizeof(std::uint32_t));
        std::uint32_t value = 0;
        std::memcpy(
            &value, bytes.data(),
            sizeof(value));
        gpu_visible_draw_count =
            value;
        gpu_draw_command_population =
            resources.hostBufferPopulation(
                resources.getBufferIdByName(
                    "draw_candidates"));
        gpu_draw_bounds_population =
            resources.hostBufferPopulation(
                resources.getBufferIdByName(
                    "draw_bounds"));
    }
    const auto pixels = render_target.readbackLastFrameRGBA8();
    const auto device_properties = GET_MODULE(VulkanManageCore).getPhysDevice().getProperties();
    const auto sprite_status = FastModuleContainer::isInitialized<SpriteScene>()
                                   ? GET_MODULE(SpriteScene).statusJson()
                                   : nlohmann::json{};
    GET_MODULE(VulkanManageCore).waitIdle();
    std::filesystem::remove_all(temp_dir);

    return RenderedCase{
        RgbaImage{golden_case.width, golden_case.height, pixels},
        std::string{device_properties.deviceName.data()},
        renderer != nullptr ? renderer->lastExecutionTraceForTesting() : nlohmann::json{},
        renderer != nullptr ? renderer->currentFramePlanOrderForTesting() : std::vector<std::string>{},
        RenderTiming::__get().has_value() ? RenderTiming::__get()->lastFrameNodeNamesForTesting()
                                          : std::vector<std::string>{},
        RenderTiming::__get().has_value() ? RenderTiming::__get()->lastFrameQueryCountForTesting() : 0,
        RenderTiming::__get().has_value() && RenderTiming::__get()->allGpuQueriesCollectedForTesting(),
        RenderTiming::__get().has_value() ? RenderTiming::__get()->statusJson()
                                          : disabledGpuTimingStatusJson(),
        RenderTiming::__get().has_value()
            ? RenderTiming::__get()->queryPoolCreateCountForTesting()
            : 0,
        FastModuleContainer::isInitialized<ui::UiModule>(),
        FastModuleContainer::isInitialized<UIContainer>() && FastModuleContainer::isInitialized<UiRenderer>(),
        FastModuleContainer::isInitialized<ui::UiModule>() ? GET_MODULE(ui::UiModule).parserInvocationsForTesting() : 0,
        FastModuleContainer::isInitialized<AtlasAssetResource>(),
        FastModuleContainer::isInitialized<SpriteScene>(),
        FastModuleContainer::isInitialized<SpriteRenderer>() && GET_MODULE(SpriteRenderer).hasGpuBuffersForTesting(),
        FastModuleContainer::isInitialized<AtlasAssetResource>() ? GET_MODULE(AtlasAssetResource).pageCountForTesting() : 0,
        sprite_status,
        gpu_visible_draw_count,
        gpu_draw_command_population,
        gpu_draw_bounds_population,
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

void writeJitterCaptureProject(const std::filesystem::path &root,
                               const nlohmann::json &projection_jitter) {
    writeTextFile(root / "project.json", makeFeatureProjectJson().dump(2));
    writeTextFile(root / "scene.json", R"json({
      "schema":"pelican.scene","version":1,
      "scenes":{"default_scene":{"objects":[]}}
    })json");
    writeTextFile(root / "assets.json", R"json({})json");
    writeTextFile(root / "ui" / "ui.json",
                  R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
    writeTextFile(root / "features" / "jitter.json",
                  nlohmann::json{
                      {"schema", "pelican.render_feature"},
                      {"version", 1},
                      {"name", "jitter_capture"},
                      {"projection_jitter", projection_jitter},
                  }.dump(2));
    writeTextFile(root / "shaders" / "fullscreen.vert", stemFullscreenVertexShader());
    writeTextFile(root / "shaders" / "jitter_capture.frag", R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_frame.glsl"
layout(location = 0) out vec4 outColor;
void main() {
    vec2 offset_px = pelicanFrame.jitter_ndc * pelicanFrame.resolution.xy * 0.5;
    outColor = vec4(offset_px + vec2(0.5), float(pelicanFrame.frame_index.x) / 8.0, 1.0);
}
)glsl");
    writeTextFile(root / "passes" / "main.json", R"json({
      "features":["features/jitter.json"],
      "render_targets":[],
      "rendering_passes":[{"name":"main","passes":[{
        "name":"jitter_capture","type":"fullscreen",
        "output":{"color":"swapchain","depth":null},
        "shader":{"vertex":"shaders/fullscreen","fragment":"shaders/jitter_capture"}
      }]}]
    })json");
}

struct JitterCapture {
    std::vector<std::vector<std::uint8_t>> frames;
    nlohmann::json frame_plan;
};

JitterCapture captureJitterFrames(std::string_view run_name,
                                  const nlohmann::json &projection_jitter) {
    FastModuleContainer modules;
    const auto root = makeTempProjectDir("jitter_capture_" + std::string{run_name});
    writeJitterCaptureProject(root, projection_jitter);
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setProjectData(makeFeatureProjectJson().dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.shader_hot_reload = false;
    launch.headless_extent = vk::Extent2D{goldenWidth, goldenHeight};

    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    auto &renderer = GET_MODULE(Renderer);
    auto &target = GET_MODULE(RenderTarget);
    JitterCapture capture;
    capture.frame_plan = renderer.currentFramePlanJson();
    for (std::uint32_t frame = 1; frame <= 8; ++frame) {
        time.advance();
        renderer.render();
        GET_MODULE(VulkanManageCore).waitIdle();
        capture.frames.push_back(target.readbackLastFrameRGBA8());
    }
    std::filesystem::remove_all(root);
    return capture;
}

std::vector<std::uint8_t> readDepthTargetBytes(GlobalRenderTargetId target_id) {
    auto &targets = GET_MODULE(RenderTargetContainer);
    const auto metadata = targets.getMetadata(target_id);
    if (metadata.format != vk::Format::eD32Sfloat ||
        !(metadata.usage & vk::ImageUsageFlagBits::eTransferSrc)) {
        throw std::runtime_error("shadow probe requires a transfer-src D32 target");
    }
    const auto &image = targets.getImage(target_id);
    const auto byte_count = static_cast<vk::DeviceSize>(metadata.extent.width) *
                            metadata.extent.height * sizeof(float);
    auto &vkcore = GET_MODULE(VulkanManageCore);
    auto staging = vkcore.allocBuf(byte_count, vk::BufferUsageFlagBits::eTransferDst,
                                   vma::MemoryUsage::eAutoPreferHost,
                                   vma::AllocationCreateFlagBits::eHostAccessRandom);
    auto &utils = GET_MODULE(VulkanUtils);
    utils.executeOneTimeCmd(
        [&](vk::CommandBuffer command) {
            utils.changeImageLayoutCmd(
                command, image, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::ImageLayout::eTransferSrcOptimal,
                {.src_stage = vk::PipelineStageFlagBits::eFragmentShader,
                 .dst_stage = vk::PipelineStageFlagBits::eTransfer,
                 .src_access = vk::AccessFlagBits::eShaderRead,
                 .dst_access = vk::AccessFlagBits::eTransferRead});
            vk::BufferImageCopy copy;
            copy.imageSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1};
            copy.imageExtent = image.extent;
            command.copyImageToBuffer(image.image.get(), vk::ImageLayout::eTransferSrcOptimal,
                                      staging.buffer.get(), copy);
            utils.changeImageLayoutCmd(
                command, image, vk::ImageLayout::eTransferSrcOptimal,
                vk::ImageLayout::eShaderReadOnlyOptimal,
                {.src_stage = vk::PipelineStageFlagBits::eTransfer,
                 .dst_stage = vk::PipelineStageFlagBits::eFragmentShader,
                 .src_access = vk::AccessFlagBits::eTransferRead,
                 .dst_access = vk::AccessFlagBits::eShaderRead});
        },
        true);
    return vkcore.readBuf(staging, byte_count);
}

struct ShadowProbeCapture {
    std::vector<std::uint8_t> shadow_bytes;
    std::uint64_t draw_count = 0;
};

ShadowProbeCapture captureShadowProbe(bool jitter_enabled) {
    FastModuleContainer modules;
    const auto root = makeTempProjectDir(jitter_enabled ? "shadow_jitter_on" : "shadow_jitter_off");
    writeShadowProject(root, true);
    writeTextFile(root / "features" / "shadow_probe.json",
                  std::string{R"json({
      "schema":"pelican.render_feature","version":1,"name":"shadow_probe",
      "render_target_overrides":{"shadow_map":{"usage":["TRANSFER_SRC"]}})json"} +
                      (jitter_enabled
                           ? R"json(,"projection_jitter":{"pattern":"halton23","phases":8})json"
                           : std::string{}) +
                      "}");
    auto rendering = makeShadowRenderingConfig(true);
    rendering["features"].push_back("features/shadow_probe.json");
    writeTextFile(root / "passes" / "main.json", rendering.dump(2));
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setProjectData(makeShadowProjectJson().dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.shader_hot_reload = false;
    launch.headless_extent = vk::Extent2D{goldenWidth, goldenHeight};
    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    time.advance();
    renderShadowFrame(GET_MODULE(RenderTarget));

    ShadowProbeCapture capture;
    for (const auto &draw : GET_MODULE(PolygonInstanceContainer).getDrawCalls()) {
        capture.draw_count += draw.draw_count;
    }
    const auto shadow = GET_MODULE(RenderTargetContainer).getRenderTargetIdByName("shadow_map");
    capture.shadow_bytes = readDepthTargetBytes(shadow);
    std::filesystem::remove_all(root);
    return capture;
}

float srgbToLinear(std::uint8_t encoded) {
    const auto value = static_cast<float>(encoded) / 255.0f;
    return value <= 0.04045f ? value / 12.92f
                             : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

} // namespace

void GoldenHarness::runProjectionJitterEquivalence() {
    setupLogger();
    requireGoldenVulkanDevice();
#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto halton_declaration =
        nlohmann::json{{"pattern", "halton23"}, {"phases", 8}};
    const auto table_declaration =
        loadJsonFile(sourceRoot() / "test/fixtures/render_features/projection_jitter_table.json")
            .at("halton23_equivalent");
    const auto first = captureJitterFrames("first", halton_declaration);
    const auto second = captureJitterFrames("second", halton_declaration);
    const auto table = captureJitterFrames("table", table_declaration);
    REQUIRE(first.frames == second.frames);
    REQUIRE(table.frames == first.frames);
    REQUIRE(first.frame_plan.at("projection_jitter") == nlohmann::json{
        {"provider", "jitter_capture"}, {"pattern", "halton23"}, {"phases", 8}});
    auto expected_table_metadata = table_declaration;
    expected_table_metadata["provider"] = "jitter_capture";
    expected_table_metadata["phases"] = 8;
    REQUIRE(table.frame_plan.at("projection_jitter") == expected_table_metadata);

    const std::vector<glm::vec2> expected{
        {0.0f, -1.0f / 6.0f}, {-1.0f / 4.0f, 1.0f / 6.0f},
        {1.0f / 4.0f, -7.0f / 18.0f}, {-3.0f / 8.0f, -1.0f / 18.0f},
        {1.0f / 8.0f, 5.0f / 18.0f}, {-1.0f / 8.0f, -5.0f / 18.0f},
        {3.0f / 8.0f, 1.0f / 18.0f}, {-7.0f / 16.0f, 7.0f / 18.0f},
    };
    REQUIRE(first.frames.size() == expected.size());
    for (std::size_t frame = 0; frame < expected.size(); ++frame) {
        CAPTURE(frame);
        REQUIRE(first.frames[frame].size() == goldenWidth * goldenHeight * 4);
        REQUIRE(srgbToLinear(first.frames[frame][0]) ==
                Catch::Approx(expected[frame].x + 0.5f).margin(0.015f));
        REQUIRE(srgbToLinear(first.frames[frame][1]) ==
                Catch::Approx(expected[frame].y + 0.5f).margin(0.015f));
    }
#else
    SKIP("projection jitter capture requires the runtime shader compiler");
#endif
}

void GoldenHarness::runProjectionJitterShadow() {
    setupLogger();
    requireGoldenVulkanDevice();
#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto off = captureShadowProbe(false);
    const auto on = captureShadowProbe(true);
    REQUIRE(off.draw_count > 0);
    REQUIRE(on.draw_count == off.draw_count);
    REQUIRE(on.shadow_bytes == off.shadow_bytes);
#else
    SKIP("projection jitter shadow probe requires the runtime shader compiler");
#endif
}

void GoldenHarness::runTaaDeterminism() {
    setupLogger();
    requireGoldenVulkanDevice();
#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto golden_root = sourceRoot() / "test/golden/taa_static";
    const auto first = renderCase(GoldenCase{
        "taa_repeat_first", "taa_static", golden_root, goldenWidth, goldenHeight});
    const auto second = renderCase(GoldenCase{
        "taa_repeat_second", "taa_static", golden_root, goldenWidth, goldenHeight});
    REQUIRE(first.image.width == second.image.width);
    REQUIRE(first.image.height == second.image.height);
    REQUIRE(first.image.pixels == second.image.pixels);
    REQUIRE(first.plan_order == second.plan_order);
#else
    SKIP("TAA deterministic capture requires the runtime shader compiler");
#endif
}

void GoldenHarness::runBLayerShadowEquivalence() {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    requireGoldenVulkanDevice();
    const auto golden_root =
        sourceRoot() / "test/golden";
    const auto off = renderCase(GoldenCase{
        "shadow_b_layer_equivalence_off",
        "shadow_b_layer_off",
        golden_root / "shadow_b_layer_off",
        goldenWidth,
        goldenHeight});
    const auto engine = renderCase(GoldenCase{
        "shadow_b_layer_equivalence_engine",
        "shadow_b_layer_engine",
        golden_root / "shadow_b_layer_engine",
        goldenWidth,
        goldenHeight});
    const auto project = renderCase(GoldenCase{
        "shadow_b_layer_equivalence_project",
        "shadow_b_layer_project",
        golden_root / "shadow_b_layer_project",
        goldenWidth,
        goldenHeight});

    REQUIRE(engine.image.width == project.image.width);
    REQUIRE(engine.image.height == project.image.height);
    REQUIRE(engine.image.pixels ==
            project.image.pixels);
    REQUIRE(engine.image.pixels.size() ==
            off.image.pixels.size());
    REQUIRE(engine.image.pixels != off.image.pixels);

    std::size_t changed_bytes = 0;
    for (std::size_t index = 0;
         index < engine.image.pixels.size();
         ++index) {
        changed_bytes +=
            engine.image.pixels[index] !=
            off.image.pixels[index];
    }
    REQUIRE(changed_bytes > 0);
#else
    SKIP("B-layer directional-shadow golden requires "
         "the runtime shader compiler");
#endif
}

void GoldenHarness::runGpuDrawIndirect() {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    requireGoldenVulkanDevice();
    const auto root =
        sourceRoot() / "test/golden/shadow_off";
    const auto baseline =
        renderCase(GoldenCase{
            "gpu_draw_cpu_baseline",
            "shadow_off", root,
            goldenWidth, goldenHeight});
    const auto zero =
        renderCase(GoldenCase{
            "gpu_draw_count_zero",
            "gpu_draw_count_zero", root,
            goldenWidth, goldenHeight});
    const auto one =
        renderCase(GoldenCase{
            "gpu_draw_count_one",
            "gpu_draw_count_one", root,
            goldenWidth, goldenHeight});
    const auto maximum =
        renderCase(GoldenCase{
            "gpu_draw_count_max",
            "gpu_draw_count_max", root,
            goldenWidth, goldenHeight});
    const auto overflow =
        renderCase(GoldenCase{
            "gpu_draw_count_overflow",
            "gpu_draw_count_overflow", root,
            goldenWidth, goldenHeight});
    const auto forced_cpu =
        renderCase(GoldenCase{
            "gpu_draw_force_cpu",
            "gpu_draw_force_cpu", root,
            goldenWidth, goldenHeight});

    REQUIRE(
        maximum.image.pixels ==
        baseline.image.pixels);
    REQUIRE(
        overflow.image.pixels ==
        baseline.image.pixels);
    REQUIRE(
        forced_cpu.image.pixels ==
        baseline.image.pixels);
    REQUIRE(
        zero.image.pixels !=
        baseline.image.pixels);
    REQUIRE(
        one.image.pixels !=
        zero.image.pixels);
    REQUIRE(
        one.image.pixels !=
        baseline.image.pixels);

    const auto producer =
        std::find(
            maximum.plan_order.begin(),
            maximum.plan_order.end(),
            "build_visible_draws");
    const auto consumer =
        std::find(
            maximum.plan_order.begin(),
            maximum.plan_order.end(),
            "gbuffer_pass");
    REQUIRE(producer !=
            maximum.plan_order.end());
    REQUIRE(consumer !=
            maximum.plan_order.end());
    REQUIRE(producer < consumer);
#else
    SKIP("GPU-written draw golden requires the runtime shader compiler");
#endif
}

void GoldenHarness::runGpuOcclusionCulling() {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    requireGoldenVulkanDevice();
    const auto root =
        sourceRoot() /
        "test/golden/shadow_off";
    const auto hidden =
        renderCase(GoldenCase{
            "gpu_occlusion_hidden",
            "gpu_occlusion_hidden", root,
            goldenWidth, goldenHeight});
    const auto visible =
        renderCase(GoldenCase{
            "gpu_occlusion_visible",
            "gpu_occlusion_visible", root,
            goldenWidth, goldenHeight});
    const auto forced_cpu =
        renderCase(GoldenCase{
            "gpu_occlusion_force_cpu",
            "gpu_occlusion_force_cpu", root,
            goldenWidth, goldenHeight});

    REQUIRE(
        hidden.gpu_visible_draw_count ==
        std::optional<std::uint32_t>{1});
    REQUIRE(
        visible.gpu_visible_draw_count ==
        std::optional<std::uint32_t>{2});
    REQUIRE(
        forced_cpu.gpu_visible_draw_count ==
        std::optional<std::uint32_t>{1});
    REQUIRE(
        hidden.image.pixels ==
        forced_cpu.image.pixels);
    REQUIRE(
        hidden.image.pixels !=
        visible.image.pixels);

    for (const auto *capture :
         {&hidden, &visible, &forced_cpu}) {
        REQUIRE(
            capture
                ->gpu_draw_command_population
                .has_value());
        REQUIRE(
            capture
                ->gpu_draw_bounds_population
                .has_value());
        REQUIRE(
            capture
                ->gpu_draw_command_population
                ->source_records == 2);
        REQUIRE(
            capture
                ->gpu_draw_command_population
                ->written_records == 2);
        REQUIRE(
            capture
                ->gpu_draw_bounds_population
                ->source_records == 2);
        REQUIRE(
            capture
                ->gpu_draw_bounds_population
                ->written_records == 2);
    }

    const auto node =
        [&](std::string_view name) {
            return std::find(
                hidden.plan_order.begin(),
                hidden.plan_order.end(),
                name);
        };
    const auto prepass =
        node("occlusion_depth_prepass");
    const auto seed =
        node("occlusion_depth_seed");
    const auto reduce =
        node("occlusion_depth_reduce_4");
    const auto cull =
        node("occlusion_cull");
    const auto geometry =
        node("gbuffer_pass");
    REQUIRE(
        prepass !=
        hidden.plan_order.end());
    REQUIRE(
        seed !=
        hidden.plan_order.end());
    REQUIRE(
        reduce !=
        hidden.plan_order.end());
    REQUIRE(
        cull !=
        hidden.plan_order.end());
    REQUIRE(
        geometry !=
        hidden.plan_order.end());
    REQUIRE(prepass < seed);
    REQUIRE(seed < reduce);
    REQUIRE(reduce < cull);
    REQUIRE(cull < geometry);
#else
    SKIP(
        "GPU occlusion golden requires the runtime shader compiler");
#endif
}

void GoldenHarness::runGoldenImages() {
    setupLogger();
    requireGoldenVulkanDevice();
    const auto cases = loadGoldenInventoryCases();

    for (const auto &golden_case : cases) {
        DYNAMIC_SECTION(golden_case.name) {
            const auto rendered = renderCase(golden_case);
            if (golden_case.mode == "ui_u1" || golden_case.mode == "ui_u2") {
                REQUIRE(rendered.ui_module_created);
                REQUIRE(rendered.ui_gpu_created);
                REQUIRE(rendered.ui_parser_invocations == 1);
            }
            if (golden_case.mode == "stem_fullscreen") {
                REQUIRE_FALSE(rendered.ui_module_created);
                REQUIRE_FALSE(rendered.ui_gpu_created);
                REQUIRE(rendered.ui_parser_invocations == 0);
            }
            if (isSpriteGoldenMode(golden_case.mode)) {
                REQUIRE(rendered.atlas_created);
                REQUIRE(rendered.sprite_scene_created);
                REQUIRE(rendered.sprite_gpu_created);
                REQUIRE(rendered.ui_module_created == (golden_case.mode == "sprite_ownership"));
                REQUIRE(rendered.ui_gpu_created == (golden_case.mode == "sprite_ownership"));
                if (golden_case.mode == "sprite_ownership") REQUIRE(rendered.atlas_page_count == 2);
                if (isStrictSpriteGoldenMode(golden_case.mode)) {
                    const auto &pixel = rendered.sprite_status.at("pixel_perfect");
                    REQUIRE(pixel.at("requested") == true);
                    REQUIRE(pixel.at("active") == true);
                    REQUIRE(pixel.at("method") == "render_only_quantization");
                    REQUIRE(pixel.at("content_viewport").at("width") == golden_case.width);
                    REQUIRE(pixel.at("content_viewport").at("height") == golden_case.height);

                    if (golden_case.mode == "sprite_pixel_zoom1") {
                        REQUIRE(rendered.sprite_status.at("sort") == "declaration");
                        REQUIRE(pixel.at("integer_zoom") == 1);
                        REQUIRE(pixel.at("eligible") == 2);
                        REQUIRE(pixel.at("downgraded") == 0);
                    } else if (golden_case.mode == "sprite_pixel_zoom2_odd") {
                        REQUIRE(rendered.sprite_status.at("sort") == "y_down");
                        REQUIRE(pixel.at("integer_zoom") == 2);
                        REQUIRE(pixel.at("eligible") == 1);
                        REQUIRE(pixel.at("downgraded") == 0);
                    } else if (golden_case.mode == "sprite_pixel_zoom3_policy") {
                        REQUIRE(pixel.at("integer_zoom") == 3);
                        REQUIRE(pixel.at("eligible") == 1);
                        REQUIRE(pixel.at("downgraded") == 2);
                        REQUIRE(pixel.at("downgrades").at("non_integer_texel_scale") == 1);
                        REQUIRE(pixel.at("downgrades").at("rotated_or_tilted") == 1);
                    } else if (golden_case.mode == "sprite_billboard") {
                        REQUIRE(pixel.at("integer_zoom") == 1);
                        REQUIRE(pixel.at("eligible") == 0);
                        REQUIRE(pixel.at("downgraded") == 1);
                        REQUIRE(pixel.at("downgrades").at("billboard") == 1);
                    }
                }
            }
            if (golden_case.mode == "ui_u1") {
                REQUIRE(rendered.atlas_created);
                REQUIRE_FALSE(rendered.sprite_scene_created);
                REQUIRE_FALSE(rendered.sprite_gpu_created);
                REQUIRE(rendered.atlas_page_count == 3);
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

void GoldenHarness::runLogicalFrameStereo() {
#if PELICAN_RUNTIME_SHADER_COMPILER && PELICAN_WITH_OPENXR
    setupLogger();
    requireGoldenVulkanDevice();
    FastModuleContainer modules;
    const auto root = makeTempProjectDir("logical_frame_stereo");
    writeLogicalFrameStereoProject(root);
    GET_MODULE(PathResolver).setup(root, false);
    auto project = makeFeatureProjectJson();
    project["name"] = "logical frame stereo fixture";
    GET_MODULE(ProjectSource).setProjectData(project.dump());

    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.shader_hot_reload = false;
    launch.headless_extent = vk::Extent2D{goldenWidth, goldenHeight};
    // Multi-view execution belongs to the compiled XR graph variant. Keep this
    // synthetic target sequential so the fixture continues to validate the
    // per-view command path independently of the multiview GPU regression.
    // Initialize ordinary Vulkan first; this is a renderer contract fixture,
    // not an OpenXR runtime/bootstrap fixture.
    (void)GET_MODULE(VulkanManageCore);
    launch.xr_active = true;

    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    GET_MODULE(ECSPredefinedRegistration).reg();
    GET_MODULE(SceneLoader).load("default_scene");
    GET_MODULE(ECSCore).update();

    auto make_view = [](float view_x, float projection_x, float camera_x) {
        RenderViewParameters result;
        result.view[3][0] = view_x;
        result.projection[0][0] = projection_x;
        result.camera_position = {camera_x, 0.0f, 0.0f};
        return result;
    };
    const std::array first_views{
        make_view(-0.75f, 0.75f, -1.0f),
        make_view(0.75f, 1.25f, 1.0f),
    };
    const std::array second_views{
        make_view(-0.5f, 0.9f, -1.5f),
        make_view(0.5f, 1.4f, 1.5f),
    };
    auto &renderer = GET_MODULE(Renderer);
    renderer.selectGraphVariant(RenderGraphVariant::xr);
    auto &flat_target = GET_MODULE(RenderTarget);
    Test::VulkanSyntheticStereoTarget stereo_target{
        launch.headless_extent, flat_target.getSwapchainFormat()};

    time.advance();
    renderer.renderLogicalFrame(stereo_target, first_views);
    const auto first_snapshots = renderer.lastViewSnapshotsForTesting();
    const auto first_left = stereo_target.readback(0);
    const auto first_right = stereo_target.readback(1);

    REQUIRE(stereo_target.logicalBeginCount() == 1);
    REQUIRE(stereo_target.logicalEndCount() == 1);
    REQUIRE(stereo_target.submissionCount() == 1);
    REQUIRE(stereo_target.viewBeginCount() == 2);
    REQUIRE(stereo_target.viewEndCount() == 2);
    REQUIRE(GET_MODULE(DeletionQueue).currentFrameForTesting() == 1);
    REQUIRE(GET_MODULE(RenderTargetContainer).historyFrameIndex() == 1);
    REQUIRE(GET_MODULE(PolygonInstanceContainer)
                .temporalHistoryAdvanceCountForTesting() == 1);
    REQUIRE(GET_MODULE(PolygonInstanceContainer)
                .compiledDrawQueueForTesting()
                .sortViewCount() == 1);

    REQUIRE(first_snapshots.size() == 2);
    for (std::size_t view = 0; view < first_views.size(); ++view) {
        REQUIRE(first_snapshots[view].view == first_views[view].view);
        REQUIRE(first_snapshots[view].projection_non_jittered ==
                first_views[view].projection);
        REQUIRE(first_snapshots[view].camera_position ==
                first_views[view].camera_position);
        REQUIRE(first_snapshots[view].previous_view == first_views[view].view);
        REQUIRE(first_snapshots[view].previous_camera_position ==
                first_views[view].camera_position);
        REQUIRE_FALSE(first_snapshots[view].historyValid());
    }
    REQUIRE(first_snapshots[0].temporal_reset_epoch ==
            first_snapshots[1].temporal_reset_epoch);

    auto &instances = GET_MODULE(PolygonInstanceContainer);
    REQUIRE(instances.instanceCountForTesting() == 1);
    const auto instance = instances.modelInstanceIdForTesting(0);
    instances.setTrs(instance, {1.0f, 0.0f, 0.0f},
                     glm::quat{1.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});

    time.advance();
    renderer.renderLogicalFrame(stereo_target, second_views);
    const auto second_snapshots = renderer.lastViewSnapshotsForTesting();
    const auto second_left = stereo_target.readback(0);
    const auto second_right = stereo_target.readback(1);

    REQUIRE(stereo_target.logicalBeginCount() == 2);
    REQUIRE(stereo_target.logicalEndCount() == 2);
    REQUIRE(stereo_target.submissionCount() == 2);
    REQUIRE(stereo_target.viewBeginCount() == 4);
    REQUIRE(stereo_target.viewEndCount() == 4);
    REQUIRE(GET_MODULE(DeletionQueue).currentFrameForTesting() == 2);
    REQUIRE(GET_MODULE(RenderTargetContainer).historyFrameIndex() == 0);
    REQUIRE(instances.temporalHistoryAdvanceCountForTesting() == 2);
    REQUIRE(instances.previousModelMatrixForTesting(instance) ==
            instances.currentModelMatrixForTesting(instance));

    REQUIRE(second_snapshots.size() == 2);
    for (std::size_t view = 0; view < second_views.size(); ++view) {
        REQUIRE(second_snapshots[view].view == second_views[view].view);
        REQUIRE(second_snapshots[view].projection_non_jittered ==
                second_views[view].projection);
        REQUIRE(second_snapshots[view].camera_position ==
                second_views[view].camera_position);
        REQUIRE(second_snapshots[view].previous_view == first_views[view].view);
        REQUIRE(second_snapshots[view].previous_projection_non_jittered ==
                first_views[view].projection);
        REQUIRE(second_snapshots[view].previous_camera_position ==
                first_views[view].camera_position);
        REQUIRE(second_snapshots[view].historyValid());
        REQUIRE(second_snapshots[view].temporal_reset_epoch ==
                first_snapshots[view].temporal_reset_epoch);
    }

    auto &frame_resources = GET_MODULE(FrameResources);
    REQUIRE(frame_resources.viewCountForTesting() == 2);
    REQUIRE(frame_resources.slotCountForTesting() == 4);
    const auto slot_00 = frame_resources.slotBufferForTesting(0, 0);
    const auto slot_01 = frame_resources.slotBufferForTesting(0, 1);
    const auto slot_10 = frame_resources.slotBufferForTesting(1, 0);
    const auto slot_11 = frame_resources.slotBufferForTesting(1, 1);
    REQUIRE(slot_00 != slot_01);
    REQUIRE(slot_00 != slot_10);
    REQUIRE(slot_00 != slot_11);
    REQUIRE(slot_01 != slot_10);
    REQUIRE(slot_01 != slot_11);
    REQUIRE(slot_10 != slot_11);
    REQUIRE(frame_resources.slotDataForTesting(0, 0).frame_index.x == 1);
    REQUIRE(frame_resources.slotDataForTesting(0, 1).frame_index.x == 1);
    REQUIRE(frame_resources.slotDataForTesting(1, 0).frame_index.x == 2);
    REQUIRE(frame_resources.slotDataForTesting(1, 1).frame_index.x == 2);
    REQUIRE(frame_resources.slotDataForTesting(1, 0).camera_position.x ==
            second_views[0].camera_position.x);
    REQUIRE(frame_resources.slotDataForTesting(1, 1).camera_position.x ==
            second_views[1].camera_position.x);

    const auto signal = [](const std::vector<std::uint8_t> &pixels,
                           std::uint32_t band) {
        return pixels.at(static_cast<std::size_t>(band) * 4);
    };
    REQUIRE(signal(first_left, 0) != signal(first_right, 0));
    REQUIRE(signal(first_left, 1) != signal(first_right, 1));
    REQUIRE(signal(first_left, 2) != signal(first_right, 2));
    REQUIRE(signal(second_left, 0) != signal(second_right, 0));
    REQUIRE(signal(second_left, 1) != signal(second_right, 1));
    REQUIRE(signal(second_left, 2) != signal(second_right, 2));
    REQUIRE(signal(second_left, 3) == signal(first_left, 1));
    REQUIRE(signal(second_right, 3) == signal(first_right, 1));
    REQUIRE(signal(second_left, 7) == signal(first_left, 2));
    REQUIRE(signal(second_right, 7) == signal(first_right, 2));
    REQUIRE(signal(first_left, 4) == signal(first_right, 4));
    REQUIRE(signal(second_left, 4) == signal(second_right, 4));
    REQUIRE(signal(first_left, 4) != signal(second_left, 4));
    REQUIRE(signal(first_left, 5) == signal(first_right, 5));
    REQUIRE(signal(first_left, 5) == signal(second_left, 5));
    REQUIRE(signal(second_left, 5) == signal(second_right, 5));
    REQUIRE(signal(second_left, 6) == signal(second_right, 6));
    REQUIRE(signal(second_left, 6) > signal(first_left, 6));

    GET_MODULE(VulkanManageCore).waitIdle();
    std::filesystem::remove_all(root);
#else
    SKIP("logical-frame stereo fixture requires the runtime shader compiler and OpenXR");
#endif
}

void GoldenHarness::runOpenXrTaaTransition() {
#if PELICAN_RUNTIME_SHADER_COMPILER && PELICAN_WITH_OPENXR
    setupLogger();
    requireGoldenVulkanDevice();
    FastModuleContainer modules;
    const auto root = makeTempProjectDir("wp133_xr_taa_transition");
    writeTaaProject(root, false);
    {
        std::ifstream config_file{root / "passes" / "main.json",
                                  std::ios::binary};
        auto config = nlohmann::json::parse(config_file);
        config["features"].push_back("engine://features/ui.json");
        config["features"].push_back("engine://features/gpu_timing.json");
        config["draw_sort"] = {
            {"opaque", {{"provider", "state_batched_v1"}}},
            {"transparent", {{"provider", "back_to_front_v1"}}},
            {"xr_view_policy", "per_view"},
        };
        // This fixture validates flat/XR graph transitions with a deliberately
        // sequential synthetic target. Multiview execution has its own
        // array-image GPU regression, so keep this target/plan contract
        // explicit instead of letting the device-dependent auto policy choose.
        config["xr"] = {
            {"view_execution", "sequential"},
        };
        writeTextFile(root / "passes" / "main.json", config.dump(2));
    }
    GET_MODULE(PathResolver).setup(root, false);
    auto project = makeShadowProjectJson();
    project["name"] = "WP133 XR TAA transition";
    GET_MODULE(ProjectSource).setProjectData(project.dump());

    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.shader_hot_reload = false;
    launch.headless_extent = vk::Extent2D{goldenWidth, goldenHeight};
    // This gate exercises the renderer's precompiled XR variant against a
    // synthetic Vulkan stereo target; no runtime/device discovery is involved.
    (void)GET_MODULE(VulkanManageCore);
    launch.xr_active = true;

    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    GET_MODULE(ECSPredefinedRegistration).reg();
    GET_MODULE(SceneLoader).load("default_scene");
    GET_MODULE(ECSCore).update();

    auto &renderer = GET_MODULE(Renderer);
    REQUIRE(renderer.hasXrGraphVariant());
    REQUIRE(renderer.xrExcludedFeatures() ==
            std::vector<std::string>{"velocity", "taa", "ui"});
    renderer.setExecutionTracingForTesting(true);
    renderer.selectGraphVariant(RenderGraphVariant::xr);
    const auto xr_plan = renderer.currentFramePlanOrderForTesting();
    for (const auto forbidden : {"velocity_pass", "taa_resolve", "taa_composite",
                                 "pelican_ui"}) {
        REQUIRE(std::find(xr_plan.begin(), xr_plan.end(), forbidden) == xr_plan.end());
    }

    // The display-class target is resolved to the flat presentation extent,
    // while a runtime can recommend a different per-eye extent.  Exercise that
    // mismatch so the engine-owned mirror intermediate stays copy-compatible
    // with display across the XR target resize.
    Test::VulkanSyntheticStereoTarget stereo_target{
        vk::Extent2D{goldenWidth / 2, goldenHeight / 2},
        GET_MODULE(RenderTarget).getSwapchainFormat()};
    time.advance();
    const auto xr_logical_frame = time.frameIndex();
    std::array xr_views{RenderViewParameters{}, RenderViewParameters{}};
    xr_views[0].view[3][0] = -0.03f;
    xr_views[0].camera_position.x = -0.03f;
    xr_views[0].first_person_view = true;
    xr_views[1].view[3][0] = 0.03f;
    xr_views[1].camera_position.x = 0.03f;
    xr_views[1].first_person_view = true;
    renderer.renderLogicalFrame(stereo_target, xr_views);
    const auto xr_snapshots = renderer.lastViewSnapshotsForTesting();
    REQUIRE(xr_snapshots.size() == 2);
    REQUIRE(xr_snapshots[0].jitter_ndc == glm::vec2{0.0f});
    REQUIRE(xr_snapshots[1].jitter_ndc == glm::vec2{0.0f});
    REQUIRE(GET_MODULE(PolygonInstanceContainer)
                .compiledDrawQueueForTesting()
                .sortViewCount() == 2);
    const auto &targets = GET_MODULE(RenderTargetContainer);
    const auto display = targets.getRenderTargetIdByName("display");
    const auto mirror = targets.getRenderTargetIdByName(
        std::string{OpenXr::xr_mirror_intermediate_name});
    REQUIRE(targets.getMetadata(mirror).extent ==
            targets.getMetadata(display).extent);
    const auto &enter_trace = renderer.graphVariantTransitionTraceForTesting();
    REQUIRE(enter_trace.size() == 1);
    REQUIRE(enter_trace.at(0).at("from") == "flat");
    REQUIRE(enter_trace.at(0).at("to") == "xr");
    REQUIRE(enter_trace.at(0).at("temporal_reset_requests") == 1);
    REQUIRE_FALSE(enter_trace.at(0).at("projection_jitter").get<bool>());

    GET_MODULE(VulkanManageCore).waitIdle();
    auto &render_timing = GET_MODULE(RenderTiming);
    render_timing.flush();
    const auto timing_status = render_timing.statusJson();
    REQUIRE(timing_status.at("enabled").get<bool>());
    REQUIRE(timing_status.at("supported").get<bool>());
    REQUIRE(timing_status.at("history_count") == 1);
    REQUIRE(timing_status.at("dropped_samples") == 0);
    REQUIRE(timing_status.at("views").size() == 3);
    REQUIRE(timing_status.at("views").at(0).at("view_index") == 0);
    REQUIRE(timing_status.at("views").at(0).at("label") == "left");
    REQUIRE(timing_status.at("views").at(1).at("view_index") == 1);
    REQUIRE(timing_status.at("views").at(1).at("label") == "right");
    REQUIRE(timing_status.at("views").at(2).at("view_index") == 2);
    REQUIRE(timing_status.at("views").at(2).at("label") == "mirror");
    REQUIRE(timing_status.at("query_pool").at("create_count") == 1);
    REQUIRE(timing_status.at("query_pool").at("pending_ranges") == 0);

    std::array<std::size_t, 3> timing_samples_per_view{};
    std::size_t unsupported_anchor_bodies = 0;
    std::set<std::string> timing_identities;
    for (const auto &sample : timing_status.at("nodes")) {
        REQUIRE(sample.at("logical_frame") == xr_logical_frame);
        REQUIRE(sample.at("graph_variant") == "xr");
        const auto view_index = sample.at("view_index").get<std::uint32_t>();
        REQUIRE(view_index < timing_samples_per_view.size());
        ++timing_samples_per_view.at(view_index);
        REQUIRE(timing_identities.insert(sample.at("identity").get<std::string>()).second);
        if (sample.at("node_kind") == "anchor" && sample.at("subrange") == "body" &&
            !sample.at("supported").get<bool>()) {
            REQUIRE(sample.at("reason") == "no_gpu_work");
            REQUIRE(sample.at("ms") == 0.0);
            ++unsupported_anchor_bodies;
        }
    }
    const auto &xr_execution_trace =
        renderer.lastExecutionTraceForTesting();
    REQUIRE(xr_execution_trace.at("views").size() == 2);
    const auto timed_node_count =
        [](const nlohmann::json &view_trace) {
            return static_cast<std::size_t>(
                std::count_if(
                    view_trace.at("nodes").begin(),
                    view_trace.at("nodes").end(),
                    [](const nlohmann::json &node) {
                        return node.at("kind") !=
                               "engine_owned_copy";
                    }));
        };
    REQUIRE(timing_samples_per_view.at(0) ==
            timed_node_count(
                xr_execution_trace.at("views").at(0)) *
                2);
    REQUIRE(timing_samples_per_view.at(1) ==
            timed_node_count(
                xr_execution_trace.at("views").at(1)) *
                2);
    REQUIRE(timing_samples_per_view.at(2) == 2);
    REQUIRE(unsupported_anchor_bodies > 0);

    time.advance();
    renderer.render();
    const auto flat_plan = renderer.currentFramePlanOrderForTesting();
    for (const auto restored : {"velocity_pass", "taa_resolve", "taa_composite",
                                "pelican_ui"}) {
        REQUIRE(std::find(flat_plan.begin(), flat_plan.end(), restored) != flat_plan.end());
    }
    const auto flat_epoch =
        renderer.lastViewSnapshotsForTesting().at(0).temporal_reset_epoch;
    const auto &return_trace = renderer.graphVariantTransitionTraceForTesting();
    REQUIRE(return_trace.size() == 2);
    REQUIRE(return_trace.at(1).at("from") == "xr");
    REQUIRE(return_trace.at(1).at("to") == "flat");
    REQUIRE(return_trace.at(1).at("temporal_reset_requests") == 1);
    REQUIRE(return_trace.at(1).at("projection_jitter").get<bool>());
    REQUIRE(return_trace.at(1).at("reset_epochs").size() == 1);
    REQUIRE(return_trace.at(1).at("reset_epochs").at(0) == flat_epoch);

    time.advance();
    renderer.render();
    REQUIRE(renderer.graphVariantTransitionTraceForTesting().size() == 2);
    REQUIRE(renderer.lastViewSnapshotsForTesting().at(0).temporal_reset_epoch ==
            flat_epoch);
    std::string capture_error;
    try {
        (void)GET_MODULE(RenderTarget).readbackLastFrameRGBA8();
    } catch (const std::exception &e) {
        capture_error = e.what();
    }
    REQUIRE(capture_error ==
            "legacy capture is unavailable while OpenXR is active; source=flat is required");
    GET_MODULE(VulkanManageCore).waitIdle();
    std::filesystem::remove_all(root);
#else
    SKIP("WP133 XR/TAA transition requires OpenXR and the runtime shader compiler");
#endif
}

void GoldenHarness::runVelocityFeature() {
    setupLogger();
    requireGoldenVulkanDevice();
    FastModuleContainer modules;
    const auto root = makeTempProjectDir("velocity_smoke");
    writeVelocitySmokeProject(root);
    GET_MODULE(PathResolver).setup(root, false);
    auto project = makeFeatureProjectJson();
    project["name"] = "velocity smoke";
    GET_MODULE(ProjectSource).setProjectData(project.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.shader_hot_reload = false;
    launch.headless_extent = vk::Extent2D{goldenWidth, goldenHeight};
    GET_MODULE(Renderer).render();
    GET_MODULE(VulkanManageCore).waitIdle();
    REQUIRE(GET_MODULE(RenderingPassContainer).isFeatureEnabled("velocity"));
    std::filesystem::remove_all(root);
}

void GoldenHarness::runSetTimeSkinnedVelocity() {
    setupLogger();
    requireGoldenVulkanDevice();
    FastModuleContainer modules;
    const auto root = makeTempProjectDir("skinned_velocity_history");
    writeSkinnedVelocityProject(root);
    GET_MODULE(PathResolver).setup(root, false);
    auto project = makeFeatureProjectJson();
    project["name"] = "skinned velocity history";
    GET_MODULE(ProjectSource).setProjectData(project.dump());

    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.shader_hot_reload = false;
    launch.headless_extent = vk::Extent2D{goldenWidth, goldenHeight};

    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    time.setTime(0.0);
    GET_MODULE(ECSPredefinedRegistration).reg();
    GET_MODULE(SceneLoader).load("default_scene");
    GET_MODULE(ECSCore).update();

    auto &camera = GET_MODULE(Camera);
    const glm::vec3 position{0.0f, 1.0f, 4.0f};
    camera.setPos(position);
    camera.setDir(glm::normalize(glm::vec3{0.0f, 1.0f, 0.0f} - position));
    camera.setUp({0.0f, 1.0f, 0.0f});

    auto &renderer = GET_MODULE(Renderer);
    auto &render_target = GET_MODULE(RenderTarget);
    renderer.render();
    const auto initial_signal = maximumVelocitySignal(render_target.readbackLastFrameRGBA8());

    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto instance = instances.modelInstanceIdForTesting(0);
    const auto model_before = instances.currentModelMatrixForTesting(instance);
    time.setTime(0.5);
    GET_MODULE(ECSCore).update();
    renderer.render();
    const auto first_seek_signal = maximumVelocitySignal(render_target.readbackLastFrameRGBA8());
    const auto model_after = instances.currentModelMatrixForTesting(instance);

    time.setTime(1.0);
    GET_MODULE(ECSCore).update();
    renderer.render();
    const auto second_seek_signal = maximumVelocitySignal(render_target.readbackLastFrameRGBA8());
    GET_MODULE(VulkanManageCore).waitIdle();

    INFO("velocity signal (RGBA8, shader scale 20): initial=" << static_cast<int>(initial_signal)
         << " first seek=" << static_cast<int>(first_seek_signal)
         << " second seek=" << static_cast<int>(second_seek_signal));
    REQUIRE(model_before == model_after);
    REQUIRE(initial_signal <= 1);
    REQUIRE(first_seek_signal <= 1);
    REQUIRE(second_seek_signal <= 1);
    std::filesystem::remove_all(root);
}

void GoldenHarness::runMorphVelocity() {
    setupLogger();
    requireGoldenVulkanDevice();
    FastModuleContainer modules;
    const auto root = makeTempProjectDir("morph_velocity_history");
    writeMorphVelocityProject(root);
    GET_MODULE(PathResolver).setup(root, false);
    auto project = makeFeatureProjectJson();
    project["name"] = "morph velocity history";
    GET_MODULE(ProjectSource).setProjectData(project.dump());

    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.shader_hot_reload = false;
    launch.headless_extent = vk::Extent2D{goldenWidth, goldenHeight};
    GET_MODULE(ECSPredefinedRegistration).reg();
    GET_MODULE(SceneLoader).load("default_scene");
    GET_MODULE(ECSCore).update();

    auto &camera = GET_MODULE(Camera);
    camera.setPos({0.0f, 0.0f, 4.0f});
    camera.setDir({0.0f, 0.0f, -1.0f});
    camera.setUp({0.0f, 1.0f, 0.0f});
    auto &renderer = GET_MODULE(Renderer);
    auto &render_target = GET_MODULE(RenderTarget);
    renderer.render();
    const auto initial = maximumVelocitySignal(render_target.readbackLastFrameRGBA8());

    auto &models = GET_MODULE(ModelAssetContainer);
    const auto &model = models.getModelTemplateByName("morph");
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const float on = 1.0f;
    PublishMorphWeightFrameDescV1 frame{
        .layout_generation = model.morph_targets->generation,
        .frame_revision = 1,
        .weights = &on,
        .weight_count = 1,
    };
    const auto instance = instances.modelInstanceIdForTesting(0);
    REQUIRE(instances.publishMorphWeightFrame(instance, frame) ==
            Animation::Status::ok);
    renderer.render();
    const auto moved = maximumVelocitySignal(render_target.readbackLastFrameRGBA8());

    instances.resetTemporalHistory();
    renderer.render();
    const auto reset = maximumVelocitySignal(render_target.readbackLastFrameRGBA8());
    GET_MODULE(VulkanManageCore).waitIdle();

    INFO("morph velocity signal (RGBA8, shader scale 20): initial="
         << static_cast<int>(initial) << " moved=" << static_cast<int>(moved)
         << " reset=" << static_cast<int>(reset));
    REQUIRE(initial <= 1);
    REQUIRE(moved >= 16);
    REQUIRE(reset <= 1);
    std::filesystem::remove_all(root);
}

std::string loadTextFile(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        throw std::runtime_error("failed to open text fixture: " + path.string());
    }
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::string canonicalTraceLineForCase(const std::string &trace, const std::string &case_name) {
    const auto prefix = case_name + ": ";
    std::istringstream stream{trace};
    std::string line;
    std::string match;
    while (std::getline(stream, line)) {
        if (!line.starts_with(prefix)) {
            continue;
        }
        if (!match.empty()) {
            throw std::runtime_error("duplicate canonical frame-plan trace for golden case: " +
                                     case_name);
        }
        match = line + "\n";
    }
    if (match.empty()) {
        throw std::runtime_error("missing canonical frame-plan trace for golden case: " +
                                 case_name);
    }
    return match;
}

void GoldenHarness::runRgba8Hashes() {
    setupLogger();
    requireGoldenVulkanDevice();
    const bool update_fixtures = updateRgba8HashFixturesRequested();
#if !PELICAN_WITH_VAT
    if (update_fixtures) {
        FAIL("RGBA8 aggregate fixture update requires PELICAN_WITH_VAT=ON");
    }
#endif
    const auto fixture_path = rgba8HashFixturePath();
    const auto expected = update_fixtures ? nlohmann::json::object() : loadJsonFile(fixture_path);
    nlohmann::json captured = nlohmann::json::object();

    for (const auto &golden_case : loadGoldenInventoryCases()) {
        CAPTURE(golden_case.name);
        const auto labels_off = renderCase(golden_case, false);
        const auto labels_on = renderCase(golden_case, true);
        REQUIRE(labels_on.image.pixels == labels_off.image.pixels);
        const auto hash = picosha2::hash256_hex_string(labels_off.image.pixels.begin(),
                                                       labels_off.image.pixels.end());
        const auto labels_on_hash = picosha2::hash256_hex_string(
            labels_on.image.pixels.begin(), labels_on.image.pixels.end());
        captured[golden_case.name] = hash;
        if (!update_fixtures) {
            REQUIRE(hash == expected.at(golden_case.name).get<std::string>());
            REQUIRE(labels_on_hash == expected.at(golden_case.name).get<std::string>());
        }
    }

    if (update_fixtures) {
        writeTextFile(fixture_path, captured.dump(2) + "\n");
    }
}

void GoldenHarness::runGpuTimingIdentity() {
    setupLogger();
    requireGoldenVulkanDevice();
    const auto cases = loadGoldenInventoryCases();
    const auto found = std::find_if(cases.begin(), cases.end(), [](const auto &golden_case) {
        return golden_case.mode == "explicit_order";
    });
    REQUIRE(found != cases.end());

    const auto timing_off_first = renderCase(*found, false, false);
    const auto timing_off_second = renderCase(*found, false, false);
    const auto timing_on_first = renderCase(*found, false, true);
    const auto timing_on_second = renderCase(*found, false, true);

    REQUIRE(timing_off_first.image.pixels == timing_off_second.image.pixels);
    REQUIRE(timing_on_first.image.pixels == timing_on_second.image.pixels);
    REQUIRE(timing_off_first.image.pixels == timing_on_first.image.pixels);
    REQUIRE(timing_off_first.execution_trace == timing_on_first.execution_trace);
    REQUIRE_FALSE(timing_off_first.gpu_timing_status.at("enabled").get<bool>());
    REQUIRE(timing_off_first.gpu_timing_pool_create_count == 0);

    const auto &status = timing_on_first.gpu_timing_status;
    REQUIRE(status.at("schema_version") == 2);
    REQUIRE(status.at("enabled").get<bool>());
    REQUIRE(status.at("supported").get<bool>());
    REQUIRE(status.at("reason") == "enabled");
    REQUIRE(status.at("history_capacity") == 120);
    REQUIRE(status.at("history_count") == 1);
    REQUIRE(status.at("dropped_samples") == 0);
    REQUIRE(status.at("logical_frame_history")
                .size() == 1);
    REQUIRE(status.at("logical_frame_history")
                .at(0)
                .at("logical_frame") == 0);
    REQUIRE(status.at("logical_frame_averages")
                .size() == 1);
    REQUIRE(status.at("logical_frame_averages")
                .at(0)
                .at("graph_variant") == "flat");
    REQUIRE(status.at("logical_frame_averages")
                .at(0)
                .at("frame_count") == 1);
    REQUIRE(status.at("views").size() == 1);
    REQUIRE(status.at("views").at(0).at("view_index") == 0);
    REQUIRE(status.at("views").at(0).at("label") == "flat");
    REQUIRE(status.at("nodes").size() == timing_on_first.plan_order.size() * 2);
    REQUIRE(status.at("query_pool").at("create_count") == 1);
    REQUIRE(status.at("query_pool").at("pending_ranges") == 0);
    REQUIRE(timing_on_first.gpu_timing_pool_create_count == 1);
    REQUIRE(timing_on_second.gpu_timing_pool_create_count == 1);

    for (std::size_t ordinal = 0; ordinal < timing_on_first.plan_order.size(); ++ordinal) {
        const auto &barriers = status.at("nodes").at(ordinal * 2);
        const auto &body = status.at("nodes").at(ordinal * 2 + 1);
        REQUIRE(barriers.at("logical_frame") == 0);
        REQUIRE(barriers.at("graph_variant") == "flat");
        REQUIRE(barriers.at("view_index") == 0);
        REQUIRE(barriers.at("node_ordinal") == ordinal);
        REQUIRE(barriers.at("node_name") == timing_on_first.plan_order.at(ordinal));
        REQUIRE(barriers.at("subrange") == "barriers");
        REQUIRE(body.at("subrange") == "body");
        REQUIRE(barriers.at("identity").get<std::string>().ends_with("/barriers"));
        REQUIRE(body.at("identity").get<std::string>().ends_with("/body"));
    }
}

void GoldenHarness::runGpuTimingRing() {
    setupLogger();
    requireGoldenVulkanDevice();
    FastModuleContainer modules;
    const auto root = makeTempProjectDir("wp143_gpu_timing_ring");
    writeExplicitOrderProject(root, true);
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setProjectData(makeFeatureProjectJson().dump());

    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.shader_hot_reload = false;
    launch.headless_extent = vk::Extent2D{goldenWidth, goldenHeight};
    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    auto &renderer = GET_MODULE(Renderer);
    auto &target = GET_MODULE(RenderTarget);

    std::vector<std::uint8_t> first_pixels;
    for (std::uint64_t frame = 1; frame <= 122; ++frame) {
        time.advance();
        renderer.render();
        if (frame == 1) first_pixels = target.readbackLastFrameRGBA8();
    }
    const auto final_pixels = target.readbackLastFrameRGBA8();
    REQUIRE(final_pixels == first_pixels);

    auto &timing = GET_MODULE(RenderTiming);
    timing.flush();
    const auto status = timing.statusJson();
    const auto plan = renderer.currentFramePlanOrderForTesting();
    REQUIRE(timing.queryPoolCreateCountForTesting() == 1);
    REQUIRE(timing.allGpuQueriesCollectedForTesting());
    REQUIRE(status.at("history_capacity") == 120);
    REQUIRE(status.at("history_count") == 120);
    REQUIRE(status.at("dropped_samples") == 0);
    REQUIRE(status.at("logical_frame_history")
                .size() == 120);
    REQUIRE(status.at("logical_frame_history")
                .front()
                .at("logical_frame") == 3);
    REQUIRE(status.at("logical_frame_history")
                .back()
                .at("logical_frame") == 122);
    REQUIRE(status.at("logical_frame_averages")
                .at(0)
                .at("frame_count") == 120);
    REQUIRE(status.at("nodes").size() == 120 * plan.size() * 2);
    REQUIRE(status.at("nodes").front().at("logical_frame") == 3);
    REQUIRE(status.at("nodes").back().at("logical_frame") == 122);
    REQUIRE(status.at("views").size() == 1);
    REQUIRE(status.at("views").at(0).at("logical_frame") == 122);
    REQUIRE(status.at("query_pool").at("frame_slots") == in_flight_frames_num);
    REQUIRE(status.at("query_pool").at("range_slots") == 1);
    REQUIRE(status.at("query_pool").at("pending_ranges") == 0);

    GET_MODULE(VulkanManageCore).waitIdle();
    std::filesystem::remove_all(root);
}

void GoldenHarness::runGpuTimingCompute() {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    requireGoldenVulkanDevice();
    FastModuleContainer modules;
    const auto root = makeTempProjectDir("wp143_gpu_timing_compute");
    writeComputeProject(root, true);
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setProjectData(makeComputeProjectJson().dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.shader_hot_reload = false;
    launch.headless_extent = vk::Extent2D{goldenWidth, goldenHeight};

    auto &target = GET_MODULE(RenderTarget);
    renderFeatureFrame(target);
    auto &timing = GET_MODULE(RenderTiming);
    timing.flush();
    const auto status = timing.statusJson();
    bool compute_barriers = false;
    bool compute_body = false;
    for (const auto &sample : status.at("nodes")) {
        if (sample.at("node_kind") != "compute" ||
            sample.at("node_name") != "write_color") continue;
        if (sample.at("subrange") == "barriers") compute_barriers = true;
        if (sample.at("subrange") == "body") {
            compute_body = true;
            REQUIRE(sample.at("supported").get<bool>());
            REQUIRE(sample.at("reason").is_null());
        }
    }
    REQUIRE(compute_barriers);
    REQUIRE(compute_body);
    REQUIRE(timing.queryPoolCreateCountForTesting() == 1);
    REQUIRE(timing.allGpuQueriesCollectedForTesting());
    std::filesystem::remove_all(root);
#else
    SKIP("GPU timing compute fixture requires the runtime shader compiler");
#endif
}

void GoldenHarness::runGpuTimingSprite() {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    requireGoldenVulkanDevice();
    const auto cases = loadGoldenInventoryCases();
    const auto found = std::find_if(cases.begin(), cases.end(), [](const auto &golden_case) {
        return golden_case.mode == "sprite_billboard";
    });
    REQUIRE(found != cases.end());

    FastModuleContainer modules;
    const auto root = makeTempProjectDir("wp143_gpu_timing_sprite");
    const auto project = writeSpriteProject(root, *found, true);
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setProjectData(project.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.shader_hot_reload = false;
    launch.headless_extent = vk::Extent2D{found->width, found->height};

    auto &target = GET_MODULE(RenderTarget);
    (void)GET_MODULE(Renderer);
    renderSpriteFrame(target, found->mode);
    REQUIRE(GET_MODULE(SpriteScene).commandCountForTesting() > 0);
    auto &timing = GET_MODULE(RenderTiming);
    timing.flush();
    const auto status = timing.statusJson();
    bool sprite_body = false;
    for (const auto &sample : status.at("nodes")) {
        if (sample.at("node_kind") == "anchor" &&
            sample.at("node_name") == "__anchor_sprite" &&
            sample.at("subrange") == "body") {
            sprite_body = true;
            REQUIRE(sample.at("supported").get<bool>());
            REQUIRE(sample.at("reason").is_null());
        }
    }
    REQUIRE(sprite_body);
    REQUIRE(timing.queryPoolCreateCountForTesting() == 1);
    REQUIRE(timing.allGpuQueriesCollectedForTesting());
    std::filesystem::remove_all(root);
#else
    SKIP("GPU timing sprite fixture requires the runtime shader compiler");
#endif
}

void GoldenHarness::runRendererTrace() {
    setupLogger();
    requireGoldenVulkanDevice();
    const bool update_fixtures = updateRendererTraceFixturesRequested();
#if !PELICAN_WITH_VAT
    if (update_fixtures) {
        FAIL("renderer aggregate fixture update requires PELICAN_WITH_VAT=ON");
    }
#endif
    const auto fixture_path = rendererTraceFixturePath();
    const auto expected = update_fixtures ? nlohmann::json::object() : loadJsonFile(fixture_path);
    const auto plan_trace_path = canonicalFramePlanTraceFixturePath();
    const auto expected_plan_trace = update_fixtures ? std::string{} : loadTextFile(plan_trace_path);
    nlohmann::json captured = nlohmann::json::object();
    std::string captured_plan_trace;
    std::string active_expected_plan_trace;

    for (const auto &golden_case : loadGoldenInventoryCases()) {
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
        if (!update_fixtures) {
            active_expected_plan_trace +=
                canonicalTraceLineForCase(expected_plan_trace, golden_case.name);
        }

        REQUIRE(executed_order == rendered.plan_order);
        if (golden_case.mode == "explicit_order") {
            REQUIRE(rendered.gpu_timing_node_names == rendered.plan_order);
            REQUIRE(rendered.gpu_timing_query_count == rendered.plan_order.size() * 4);
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
        REQUIRE(captured_plan_trace == active_expected_plan_trace);
    }
}

void GoldenHarness::runFullscreenRebind() {
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
    auto &render_targets = GET_MODULE(RenderTargetContainer);
    auto &compute_tasks = GET_MODULE(ComputeTaskContainer);
    const auto compute_history = render_targets.registerRenderTarget(
        "compute_history", {goldenWidth, goldenHeight}, "rgba8", "test", 1.0f,
        std::nullopt, vk::Format::eR8G8B8A8Unorm,
        vk::ImageUsageFlagBits::eColorAttachment |
            vk::ImageUsageFlagBits::eSampled |
            vk::ImageUsageFlagBits::eStorage,
        vma::MemoryUsage::eAutoPreferDevice, true);
    REQUIRE(isConcreteRenderTarget(compute_history));
    GET_MODULE(FrameGraphResourceContainer)
        .registerBuffers({FrameGraphBufferDefinition{
            "history_probe", 16, true}});
    ComputeTaskDefinition history_task;
    history_task.name = "history_probe_task";
    history_task.shader =
        makeShaderReference("shaders/history_image", ShaderStage::compute);
    history_task.reads = {"compute_history@history"};
    history_task.writes = {"compute_history", "history_probe"};
    const auto compute_task_id = compute_tasks.registerComputeTask(
        history_task,
        ComputeTaskRuntimeDependencies{GET_MODULE(ShaderLibrary),
                                       GET_MODULE(PathResolver), render_targets,
                                       GET_MODULE(FrameGraphResourceContainer)});
    const auto initial_compute_views_0 =
        compute_tasks.boundImageViewsForTesting(compute_task_id, 0);
    const auto initial_compute_views_1 =
        compute_tasks.boundImageViewsForTesting(compute_task_id, 1);
    REQUIRE(initial_compute_views_0 == std::vector<vk::ImageView>{
                                           render_targets.getImageViewForFrame(
                                               compute_history, true, 0),
                                           render_targets.getImageViewForFrame(
                                               compute_history, false, 0)});
    REQUIRE(initial_compute_views_1 == std::vector<vk::ImageView>{
                                           render_targets.getImageViewForFrame(
                                               compute_history, true, 1),
                                           render_targets.getImageViewForFrame(
                                               compute_history, false, 1)});
    REQUIRE(initial_compute_views_0 != initial_compute_views_1);
    const auto initial_compute_revision =
        compute_tasks.bindingRevisionForTesting(compute_task_id);
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
    writeTextFile(shader_path, std::string{copyInputFragmentShader()} + "\n// hot reload rebind probe\n");
    const auto shader_key = watch::makeAssetKey("shaders/copy_input.frag");
    REQUIRE(GET_MODULE(watch::ReloadService).applyRequestForTesting(
        {shader_key, watch::ReloadKind::modified, {}, 1}));
    REQUIRE(GET_MODULE(DeletionQueue).pendingCountForTesting() > 0);

    renderer.render();
    GET_MODULE(VulkanManageCore).waitIdle();
    REQUIRE(renderer.imageMemoryDependencyCountForTesting() > 0);
    const auto hot_reload_revision = fullscreen_passes.inputBindingRevisionForTesting(*input_pass);
    REQUIRE(hot_reload_revision > initial_revision);
    REQUIRE(fullscreen_passes.boundInputImageViewsForTesting(*input_pass) == initial_views);
    const auto hot_reload_compute_revision =
        compute_tasks.bindingRevisionForTesting(compute_task_id);
    REQUIRE(hot_reload_compute_revision > initial_compute_revision);
    REQUIRE(compute_tasks.boundImageViewsForTesting(compute_task_id, 0) ==
            initial_compute_views_0);
    REQUIRE(compute_tasks.boundImageViewsForTesting(compute_task_id, 1) ==
            initial_compute_views_1);

    writeTextFile(shader_path,
                  "#version 450\nlayout(location = 0) out vec4 outColor;\n"
                  "void main() { this_is_not_valid; }\n");
    REQUIRE_FALSE(GET_MODULE(watch::ReloadService).applyRequestForTesting(
        {shader_key, watch::ReloadKind::modified, {}, 1}));
    renderer.render();
    GET_MODULE(VulkanManageCore).waitIdle();
    REQUIRE(fullscreen_passes.inputBindingRevisionForTesting(*input_pass) ==
            hot_reload_revision);
    REQUIRE(fullscreen_passes.boundInputImageViewsForTesting(*input_pass) == initial_views);

    const auto lit_color = render_targets.getRenderTargetIdByName("lit_color");
    const auto old_view = render_targets.getImageView(lit_color);
    renderer.recreateRenderTargetsAndRebindForTesting(vk::Extent2D{goldenWidth, goldenHeight});
    const auto new_view = render_targets.getImageView(lit_color);

    REQUIRE(new_view != old_view);
    REQUIRE(fullscreen_passes.inputBindingRevisionForTesting(*input_pass) > hot_reload_revision);
    REQUIRE(fullscreen_passes.boundInputImageViewsForTesting(*input_pass) ==
            std::vector<vk::ImageView>{new_view});
    REQUIRE(compute_tasks.bindingRevisionForTesting(compute_task_id) >
            hot_reload_compute_revision);
    REQUIRE(compute_tasks.boundImageViewsForTesting(compute_task_id, 0) ==
            std::vector<vk::ImageView>{
                render_targets.getImageViewForFrame(compute_history, true, 0),
                render_targets.getImageViewForFrame(compute_history, false, 0)});
    REQUIRE(compute_tasks.boundImageViewsForTesting(compute_task_id, 1) ==
            std::vector<vk::ImageView>{
                render_targets.getImageViewForFrame(compute_history, true, 1),
                render_targets.getImageViewForFrame(compute_history, false, 1)});

    renderer.render();
    GET_MODULE(VulkanManageCore).waitIdle();
    std::filesystem::remove_all(temp_dir);
}

} // namespace Pelican
