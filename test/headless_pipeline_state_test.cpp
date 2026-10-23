// PSN-1 A1: the generic raster pass draws with the pipeline-state vocabulary
// the pipeline factory used to hard-code. On the GPU:
//   (a) polygon_mode line leaves a triangle's inside unpainted,
//   (b) of two quads on one plane the one drawn second with depth_bias wins,
//   (c) pass A writes stencil 1 and pass B, testing equal 1, draws only
//       inside A,
//   (d) line_width 3 draws a three-pixel line when the device has wideLines;
//   (e) without it, the document is refused at registration by
//       pipeline.state_unsupported@1 -- which the second case checks on any
//       device by masking the fact the production check reads.

#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/renderingpass/renderingpasscontainer.hpp"
#include "../src/core/fullscreenpass/fullscreenpasscontainer.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/core/vkcore/runtimecapabilities.hpp"
#include "../src/project/renderpipeline.hpp"
#include "render_graph_root_test_support.hpp"
#include "renderer_testaccess.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace Pelican {
namespace {

#if PELICAN_RUNTIME_SHADER_COMPILER

using Json = nlohmann::json;
using Texel = std::array<std::uint8_t, 4>;

constexpr std::uint32_t psn1Extent = 32;

std::filesystem::path makePsn1ProjectDir() {
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_psn1_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    return root;
}

void writePsn1File(const std::filesystem::path &path, const std::string &text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out << text;
    out.close();
    if (!out) throw std::runtime_error("cannot write fixture: " + path.string());
}

// One vertex shader for every pass: PSN1_SHAPE picks the geometry (clip
// space; +y is down the image) and PSN1_COLOR the fragment's color.
constexpr const char *psn1VertexShader = R"glsl(#version 450
#ifndef PSN1_SHAPE
#define PSN1_SHAPE 0
#endif
#if PSN1_SHAPE == 0
// (a) one large triangle; its centroid is far from every edge.
const vec2 positions[3] = vec2[](vec2(-0.8, -0.8), vec2(0.8, -0.8), vec2(0.0, 0.8));
#elif PSN1_SHAPE == 1
// The whole target.
const vec2 positions[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                 vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
#elif PSN1_SHAPE == 2
// The middle half of the target (pixels 8..23 of 32).
const vec2 positions[6] = vec2[](vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(0.5, 0.5),
                                 vec2(-0.5, -0.5), vec2(0.5, 0.5), vec2(-0.5, 0.5));
#else
// A horizontal line through the centre of pixel row 16 of 32.
const vec2 positions[2] = vec2[](vec2(-0.75, 0.03125), vec2(0.75, 0.03125));
#endif
void main() {
    gl_Position = vec4(positions[gl_VertexIndex], 0.5, 1.0);
}
)glsl";

constexpr const char *psn1FragmentShader = R"glsl(#version 450
#ifndef PSN1_COLOR
#define PSN1_COLOR 0
#endif
const vec3 colors[4] = vec3[](vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0),
                              vec3(0.0, 0.0, 1.0), vec3(1.0, 1.0, 1.0));
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(colors[PSN1_COLOR], 1.0);
}
)glsl";

constexpr const char *psn1PresentVertexShader = R"glsl(#version 450
layout(location = 0) out vec2 outUV;
const vec2 positions[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                 vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
void main() {
    vec2 position = positions[gl_VertexIndex];
    outUV = position * 0.5 + 0.5;
    gl_Position = vec4(position, 0.0, 1.0);
}
)glsl";

constexpr const char *psn1PresentFragmentShader = R"glsl(#version 450
layout(set = 1, binding = 0) uniform sampler2D wire;
layout(set = 1, binding = 1) uniform sampler2D bias;
layout(set = 1, binding = 2) uniform sampler2D mask;
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = inUV.x < 1.0 / 3.0 ? texture(wire, inUV)
             : inUV.x < 2.0 / 3.0 ? texture(bias, inUV) : texture(mask, inUV);
}
)glsl";

Json colorTarget(const std::string &name) {
    return Json{{"name", name}, {"extent_scale", 1.0}, {"format", "R8G8B8A8_UNORM"}, {"format_class", "data"},
                {"usage", {"COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"}}};
}

Json depthTarget(const std::string &name, const std::string &format) {
    return Json{{"name", name}, {"extent_scale", 1.0}, {"format", format}, {"format_class", "data"},
                {"usage", Json::array({"DEPTH_STENCIL_ATTACHMENT"})}};
}

Json rasterPass(const std::string &name, int shape, int color, std::uint32_t vertex_count,
                const std::string &color_target, const Json &depth_target, Json raster_state) {
    return Json{{"name", name},
                {"type", "raster"},
                {"output", {{"color", color_target}, {"depth", depth_target}}},
                {"shader",
                 {{"vertex", "shaders/psn1"},
                  {"fragment", "shaders/psn1"},
                  {"defines", {"PSN1_SHAPE=" + std::to_string(shape), "PSN1_COLOR=" + std::to_string(color)}}}},
                {"draw", {{"vertex_count", vertex_count}}},
                {"raster_state", std::move(raster_state)},
                {"color_load_op", "clear"},
                {"clear_color", {0.0, 0.0, 0.0, 1.0}}};
}

Json stencilFaces(const Json &face) {
    return Json{{"front", face}, {"back", face}};
}

// The rendering section: (a)-(c), and (d) when `wide_line`.
Json psn1RenderingConfig(bool wide_line) {
    Json targets = Json::array({colorTarget("psn1_wire"), colorTarget("psn1_bias"),
                                depthTarget("psn1_bias_depth", "D32_SFLOAT"), colorTarget("psn1_mask"),
                                depthTarget("psn1_mask_depth", "D24_UNORM_S8_UINT")});
    Json passes = Json::array();
    // (a)
    passes.push_back(rasterPass("psn1_wire_pass", 0, 3, 3, "psn1_wire", nullptr, {{"polygon_mode", "line"}}));
    // (b) red, then green on the same plane: a strict less test keeps red
    // unless green's depth is biased toward the viewer.
    auto base = rasterPass("psn1_bias_base", 1, 0, 6, "psn1_bias", "psn1_bias_depth",
                           {{"depth_test", true}, {"depth_write", true}, {"depth_compare", "less"}});
    base["depth_load_op"] = "clear";
    base["depth_store_op"] = "store";
    passes.push_back(std::move(base));
    auto top = rasterPass("psn1_bias_top", 1, 1, 6, "psn1_bias", "psn1_bias_depth",
                          {{"depth_test", true}, {"depth_write", true}, {"depth_compare", "less"},
                           {"depth_bias", {{"constant", -1000.0}}}});
    top["after"] = Json::array({"psn1_bias_base"});
    top["color_load_op"] = "load";
    top["depth_load_op"] = "load";
    top["depth_store_op"] = "store";
    passes.push_back(std::move(top));
    // (c) A writes stencil 1 in the middle half (and no color); B draws blue
    // over the whole target where the stencil is 1.
    auto write = rasterPass("psn1_mask_write", 2, 0, 6, "psn1_mask", "psn1_mask_depth",
                            {{"stencil", stencilFaces({{"pass", "replace"}, {"compare", "always"}, {"reference", 1}})},
                             {"color_attachments", Json::array({{{"write_mask", "none"}}})}});
    write["depth_load_op"] = "clear";
    write["depth_store_op"] = "store";
    passes.push_back(std::move(write));
    auto read = rasterPass("psn1_mask_read", 1, 2, 6, "psn1_mask", "psn1_mask_depth",
                           {{"stencil", stencilFaces({{"compare", "equal"}, {"reference", 1}, {"write_mask", 0}})}});
    read["after"] = Json::array({"psn1_mask_write"});
    read["color_load_op"] = "load";
    read["depth_load_op"] = "load";
    passes.push_back(std::move(read));
    if (wide_line) {
        // (d)
        targets.push_back(colorTarget("psn1_line"));
        passes.push_back(rasterPass("psn1_line_pass", 3, 3, 2, "psn1_line", nullptr,
                                    {{"topology", "line_list"}, {"line_width", 3.0}}));
    }
    passes.push_back(Json{{"name", "psn1_present"},
                          {"type", "fullscreen"},
                          {"input", {"psn1_wire", "psn1_bias", "psn1_mask"}},
                          {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
                          {"shader", {{"vertex", "shaders/psn1_present"}, {"fragment", "shaders/psn1_present"}}}});
    return Json{{"render_targets", std::move(targets)},
                {"rendering_passes", Json::array({{{"name", "psn1_main"}, {"passes", std::move(passes)}}})}};
}

// Files, project and launch settings; the rendering config is written by
// the caller (it may depend on the device's facts).
void setUpPsn1Project(const std::filesystem::path &root) {
    writePsn1File(root / "scene.json",
                  R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
    writePsn1File(root / "assets.json", R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
    writePsn1File(root / "shaders" / "psn1.vert", psn1VertexShader);
    writePsn1File(root / "shaders" / "psn1.frag", psn1FragmentShader);
    writePsn1File(root / "shaders" / "psn1_present.vert", psn1PresentVertexShader);
    writePsn1File(root / "shaders" / "psn1_present.frag", psn1PresentFragmentShader);
    Json project{{"schema", "pelican.project"},
                 {"version", 1},
                 {"name", "psn1-pipeline-state"},
                 {"basic_config",
                  {{"window_size", {{"width", psn1Extent}, {"height", psn1Extent}}},
                   {"scene_data_json", "scene.json"},
                   {"asset_data_json", "assets.json"},
                   {"default_scene_id", "default_scene"},
                   {"rendering_config_json", "pipeline.json"},
                   {"default_rendering_pass", "psn1_main"}}}};
    GET_MODULE(ProjectSource).setProjectData(project.dump());
    GET_MODULE(PathResolver).setup(root, false, project.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{psn1Extent, psn1Extent};
    launch.headless_frames = 1;
    GET_MODULE(EngineTime).setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
}

void writePsn1RenderingConfig(const std::filesystem::path &root, const Json &section) {
    writePsn1File(root / "pipeline.json", TestSupport::renderGraphRoot(section).dump(2));
}

struct Psn1Readback {
    RenderTargetReadback pixels;

    Texel at(std::uint32_t column, std::uint32_t row) const {
        REQUIRE(pixels.format == vk::Format::eR8G8B8A8Unorm);
        REQUIRE(column < pixels.extent.width);
        REQUIRE(row < pixels.extent.height);
        const auto offset = (static_cast<std::size_t>(row) * pixels.extent.width + column) * 4u;
        return {pixels.bytes.at(offset), pixels.bytes.at(offset + 1), pixels.bytes.at(offset + 2),
                pixels.bytes.at(offset + 3)};
    }

    std::size_t count(const Texel &texel) const {
        std::size_t result = 0;
        for (std::uint32_t row = 0; row < pixels.extent.height; ++row)
            for (std::uint32_t column = 0; column < pixels.extent.width; ++column)
                result += at(column, row) == texel ? 1u : 0u;
        return result;
    }
};

constexpr Texel black{0, 0, 0, 255}, red{255, 0, 0, 255}, green{0, 255, 0, 255}, blue{0, 0, 255, 255},
    white{255, 255, 255, 255};

#endif

} // namespace

TEST_CASE("PSN-1 a raster pass draws wireframe, depth bias, stencil and wide lines from raster_state",
          "[headless][gpu][psn1]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    FastModuleContainer modules;
    const auto root = makePsn1ProjectDir();
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    } cleanup{root};
    try {
        setUpPsn1Project(root);
        auto &vkcore = TestSupport::requireVulkanDevice("Vulkan PSN-1 pipeline state unavailable");
        const bool wide_lines = vkcore.getRuntimeCapabilities().wide_lines;
        CAPTURE(wide_lines);
        writePsn1RenderingConfig(root, psn1RenderingConfig(wide_lines));

        auto &renderer = GET_MODULE(Renderer);
        // The descs carry what the documents wrote (the factory reads them).
        const auto rendering_pass = GET_MODULE(RenderingPassContainer).getRenderingPassIdByName("psn1_main");
        const auto program = GET_MODULE(FrameGraphRuntimeContainer).findProgram(rendering_pass);
        REQUIRE(program != nullptr);
        std::map<std::string, const CompiledPass *> passes;
        for (const auto &compiled : program->rendering_pass.passes) passes.emplace(compiled.definition.name, &compiled);
        auto &fullscreen = GET_MODULE(FullscreenPassContainer);
        const auto desc_of = [&](const std::string &name) {
            REQUIRE(passes.contains(name));
            return fullscreen.graphicsPipelineDescForTesting(passes.at(name)->pass_id);
        };
        CHECK(desc_of("psn1_wire_pass").polygon_mode == vk::PolygonMode::eLine);
        CHECK(desc_of("psn1_bias_top").depth_bias.has_value());
        CHECK_FALSE(desc_of("psn1_bias_base").depth_bias.has_value());
        CHECK(desc_of("psn1_mask_write").stencil.has_value());
        CHECK(desc_of("psn1_mask_read").stencil.has_value());
        CHECK(desc_of("psn1_mask_read").depth_format == std::optional{vk::Format::eD24UnormS8Uint});

        GET_MODULE(EngineTime).advance();
        renderer.render();
        vkcore.waitIdle();
        const auto read = [&](std::string_view name) {
            return Psn1Readback{RendererTestAccess::readRenderTarget(renderer, name, ImageSubresourceRange{})};
        };

        // (a) The outline is drawn and the inside is not: the centroid of
        // the triangle (16, 11) stays the clear color.
        const auto wire = read("psn1_wire");
        CHECK(wire.at(16, 11) == black);
        const auto outline = wire.count(white);
        CAPTURE(outline);
        CHECK(outline >= 40u);
        CHECK(outline <= 160u);

        // (b) The biased (green) quad, drawn second on the same plane, wins.
        const auto bias = read("psn1_bias");
        CHECK(bias.at(16, 16) == green);
        CHECK(bias.count(green) == psn1Extent * psn1Extent);

        // (c) Only inside the quad that wrote stencil 1 is blue.
        const auto mask = read("psn1_mask");
        CHECK(mask.at(16, 16) == blue);
        CHECK(mask.at(9, 9) == blue);
        CHECK(mask.at(2, 2) == black);
        CHECK(mask.at(29, 16) == black);
        CHECK(mask.count(blue) == 16u * 16u);

        // (d) Three pixel rows around the line's row 16, nothing beyond.
        if (wide_lines) {
            const auto line = read("psn1_line");
            CHECK(line.at(16, 15) == white);
            CHECK(line.at(16, 16) == white);
            CHECK(line.at(16, 17) == white);
            CHECK(line.at(16, 13) == black);
            CHECK(line.at(16, 19) == black);
        }
        vkcore.waitIdle();
    } catch (const std::exception &error) {
        TestSupport::skipIfVulkanDeviceUnavailable(error, "Vulkan PSN-1 pipeline state unavailable");
        throw;
    }
#else
    SUCCEED("PSN-1 GPU acceptance is compiler-ON only");
#endif
}

// PSN-1 A3 on the production path, and (e): with a device fact masked, the
// rendering-config registration refuses the document that needs it, by
// pipeline.state_unsupported@1 {pass, field, feature}.
TEST_CASE("PSN-1 registration refuses a document whose state the device lacks the feature for",
          "[headless][gpu][psn1]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    struct Expectation {
        const char *name;
        bool VulkanRuntimeCapabilities::*fact;
        Json raster_state;
        int shape;
        std::uint32_t vertex_count;
        const char *field;
        const char *feature;
    };
    const std::array expectations{
        Expectation{"fillModeNonSolid", &VulkanRuntimeCapabilities::fill_mode_non_solid,
                    Json{{"polygon_mode", "line"}}, 0, 3, "polygon_mode", "fillModeNonSolid"},
        // (e): the wide line of (d) on a device without wideLines.
        Expectation{"wideLines", &VulkanRuntimeCapabilities::wide_lines,
                    Json{{"topology", "line_list"}, {"line_width", 3.0}}, 3, 2, "line_width", "wideLines"},
    };
    for (const auto &expectation : expectations) {
        DYNAMIC_SECTION(expectation.name) {
            FastModuleContainer modules;
            const auto root = makePsn1ProjectDir();
            struct Cleanup {
                std::filesystem::path root;
                ~Cleanup() {
                    std::error_code error;
                    std::filesystem::remove_all(root, error);
                }
            } cleanup{root};
            try {
                setUpPsn1Project(root);
                auto &vkcore = TestSupport::requireVulkanDevice("Vulkan PSN-1 refusal unavailable");
                // Mutate the real runtime fact the registration reads; the
                // device keeps the feature enabled, only the fact says no.
                auto &capabilities = const_cast<VulkanRuntimeCapabilities &>(vkcore.getRuntimeCapabilities());
                struct Restore {
                    VulkanRuntimeCapabilities &capabilities;
                    bool VulkanRuntimeCapabilities::*fact;
                    bool value;
                    ~Restore() { capabilities.*fact = value; }
                } restore{capabilities, expectation.fact, capabilities.*expectation.fact};
                capabilities.*expectation.fact = false;
                // The document of the first case (without its wide line),
                // with one more pass that needs the masked feature.
                auto section = psn1RenderingConfig(false);
                section["render_targets"].push_back(colorTarget("psn1_extra"));
                auto &passes = section["rendering_passes"][0]["passes"];
                // Only the extra pass needs the feature: (a) fills here.
                for (auto &pass : passes)
                    if (pass.at("name") == "psn1_wire_pass") pass["raster_state"] = Json::object();
                passes.insert(passes.begin(),
                              rasterPass("psn1_unsupported", expectation.shape, 3, expectation.vertex_count,
                                         "psn1_extra", nullptr, expectation.raster_state));
                writePsn1RenderingConfig(root, section);
                try {
                    (void)GET_MODULE(Renderer);
                    FAIL("the document was registered without " << expectation.feature);
                } catch (const std::runtime_error &error) {
                    CAPTURE(error.what());
                    const auto coded = splitCodedRefusalMessage(error.what());
                    REQUIRE(coded.has_value());
                    CHECK(coded->code == "pipeline.state_unsupported@1");
                    CHECK(coded->fields == Json{{"pass", "psn1_unsupported"},
                                                {"field", expectation.field},
                                                {"feature", expectation.feature}});
                }
                vkcore.waitIdle();
            } catch (const std::exception &error) {
                TestSupport::skipIfVulkanDeviceUnavailable(error, "Vulkan PSN-1 refusal unavailable");
                throw;
            }
        }
    }
#else
    SUCCEED("PSN-1 GPU acceptance is compiler-ON only");
#endif
}

} // namespace Pelican
