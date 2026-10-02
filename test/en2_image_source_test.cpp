#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/renderingpass/graphimagesource.hpp"
#include "../src/core/renderingpass/rendertargetcontainer.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/watch/reloadservice.hpp"
#include "../src/project/projecttemplate.hpp"
#include "render_graph_root_test_support.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stb_image.h>
#include <stb_image_write.h>
#include <string>
#include <vector>

// EN-2. A render.image reads a project image file (parameters.source): the
// file's sRGB values reach the shader linear and come back out unchanged, a
// premultiplied copy has its RGB times alpha in linear space, and a file that
// is not there is refused by name.

namespace Pelican {
namespace {

using Json = nlohmann::json;

void writeFile(const std::filesystem::path &path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file << contents;
}

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

TempDir makeTempDir(std::string_view name) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    return TempDir{std::filesystem::temp_directory_path() /
                   ("pelican_en2_" + std::string{name} + "_" + std::to_string(suffix))};
}

// The 2 x 2 file, sRGB and straight alpha: top-left, top-right, bottom-left,
// bottom-right. Only the bottom-right texel is not opaque.
constexpr std::array<std::array<unsigned char, 4>, 4> quad{{
    {200, 40, 10, 255},
    {30, 180, 90, 255},
    {90, 120, 240, 255},
    {255, 128, 0, 128},
}};

// `reversed`: the texels in the opposite order (bottom-right first).
void writeQuad(const std::filesystem::path &path, bool reversed = false) {
    std::filesystem::create_directories(path.parent_path());
    std::vector<unsigned char> rgba;
    for (std::size_t index = 0; index < quad.size(); ++index) {
        const auto &texel = quad[reversed ? quad.size() - 1 - index : index];
        rgba.insert(rgba.end(), texel.begin(), texel.end());
    }
    REQUIRE(stbi_write_png(path.string().c_str(), 2, 2, 4, rgba.data(), 2 * 4) != 0);
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
    TestSupport::requireVulkanDevice("EN-2 image sources");
    GET_MODULE(ECSPredefinedRegistration).reg();
    (void)GET_MODULE(watch::ReloadService);
}

struct Rpc {
    std::istringstream input;
    std::ostringstream output;
    EngineRpcEndpoint endpoint{input, output};
    std::uint64_t next_id = 9000;

    Json result(std::string_view method, Json params = Json::object()) {
        const auto response = Json::parse(endpoint.processLine(
            Json{{"jsonrpc", "2.0"}, {"id", ++next_id}, {"method", method}, {"params", std::move(params)}}
                .dump()));
        INFO(response.dump(2));
        REQUIRE(response.contains("result"));
        return response.at("result");
    }
    std::vector<unsigned char> frame(const std::filesystem::path &capture) {
        (void)result("render_frame");
        std::filesystem::remove(capture);
        (void)result("capture", {{"path", capture.generic_string()}});
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc *pixels = stbi_load(capture.string().c_str(), &width, &height, &channels, 4);
        REQUIRE(pixels != nullptr);
        REQUIRE(width == 64);
        REQUIRE(height == 64);
        std::vector<unsigned char> rgba(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
        stbi_image_free(pixels);
        return rgba;
    }
};

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

Rgb pixel(const std::vector<unsigned char> &rgba, int x, int y) {
    const auto at = (static_cast<std::size_t>(y) * 64 + x) * 4;
    return {rgba[at], rgba[at + 1], rgba[at + 2]};
}

bool near(Rgb got, Rgb expected, int tolerance) {
    return std::abs(got.r - expected.r) <= tolerance && std::abs(got.g - expected.g) <= tolerance &&
           std::abs(got.b - expected.b) <= tolerance;
}

const Json color_type{{"semantic", "pelican.render.color_signal@1"},
                      {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}};

// The left half shows the straight image, the right half the premultiplied
// one, each over the whole height.
constexpr std::string_view split_frag = R"glsl(#version 460
#extension GL_GOOGLE_include_directive : require
#include "pelican_sets.glsl"
#include "pelican_view.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
PELICAN_DECLARE_INPUT_0(in_straight);
PELICAN_DECLARE_INPUT_1(in_premultiplied);

void main() {
    vec2 uv = vec2(fract(inUV.x * 2.0), inUV.y);
    vec4 color = inUV.x < 0.5 ? PELICAN_TEXTURE_2D_0(in_straight, uv) : PELICAN_TEXTURE_2D_1(in_premultiplied, uv);
    outColor = vec4(color.rgb, 1.0);
}
)glsl";

// The blank template; `present` shows en2_shown, which the graph pass `split`
// draws from the two file images just before it.
void writeProject(const std::filesystem::path &root) {
    initializeProjectTemplate(root, ProjectTemplate::Blank);
    writeQuad(root / "assets/en2/quad.png");
    writeFile(root / "shaders/en2_split.frag", split_frag);
    Json rendering{
        {"render_targets", Json::array({{{"name", "en2_shown"},
                                         {"extent_scale", 1.0},
                                         {"format", "R16G16B16A16_SFLOAT"},
                                         {"format_class", "explicit(R16G16B16A16_SFLOAT)"},
                                         {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}}})},
        {"rendering_passes",
         Json::array({{{"name", "main_render"},
                       {"passes", Json::array({{{"name", "present"},
                                                {"type", "fullscreen"},
                                                {"input", Json::array({"en2_shown"})},
                                                {"output", {{"color", {"display"}}, {"depth", nullptr}}},
                                                {"shader", {{"vertex", "engine://fullscreen"},
                                                            {"fragment", "engine://passthrough"}}}}})}}})}};
    auto document = TestSupport::renderGraphRootWithTaskFeature(rendering, root);
    // The straight image also has its mips generated at upload; magnified,
    // it is read at level 0.
    const auto image = [](const std::string &id, const std::string &alpha) {
        return Json{{"id", id}, {"op", "render.image"},
                    {"parameters", {{"type", color_type},
                                    {"source", {{"asset", "project://assets/en2/quad.png"}, {"alpha", alpha},
                                                {"mips", alpha == "straight" ? "full" : "none"}}}}},
                    {"outputs", {"resource"}}};
    };
    const auto numbered = [](int input_index) {
        return Json{{"kind", "image"}, {"type", color_type}, {"access", "read"}, {"binding", "input"},
                    {"input_index", input_index}, {"sampling", {{"filter", "nearest"}}}};
    };
    document["nodes"] = Json::array({
        image("en2_straight", "straight"), image("en2_premultiplied", "premultiply"),
        {{"id", "shown_ref"}, {"op", "render.target_ref"},
         {"parameters", {{"name", "en2_shown"}, {"type", color_type}}}, {"outputs", {"resource"}}},
        {{"id", "split"}, {"op", "render.fullscreen"},
         {"parameters", {{"pass_set", "main_render"}, {"insert", {{"before", "present"}}},
                         {"shader", {{"vertex", "engine://fullscreen"},
                                     {"fragment", "project://shaders/en2_split"}}},
                         {"ports", {{"straight", numbered(0)}, {"premultiplied", numbered(1)},
                                    {"color", {{"kind", "image"}, {"type", color_type}, {"access", "write"},
                                               {"attachment", "color"}, {"location", 0}}}}}}},
         {"inputs", {{"straight", {{"node", "en2_straight"}, {"port", "resource"}}},
                     {"premultiplied", {{"node", "en2_premultiplied"}, {"port", "resource"}}},
                     {"color", {{"node", "shown_ref"}, {"port", "resource"}}}}},
         {"outputs", {"color"}}}});
    writeFile(root / "passes/main_rendering_config.json", document.dump(2));
}

// The bottom-right texel premultiplied in linear space and encoded back to
// sRGB: (255, 128, 0) at alpha 128/255 is (188, 93, 0); multiplying the sRGB
// bytes would give (128, 64, 0).
constexpr Rgb premultiplied_bottom_right{188, 93, 0};

Rgb texel(std::size_t index) {
    return {quad[index][0], quad[index][1], quad[index][2]};
}

} // namespace

TEST_CASE("EN-2 an image file that is not there, and the premultiplied pixels of one that is",
          "[en2][image-source]") {
    FastModuleContainer modules;
    const auto dir = makeTempDir("loader");
    std::filesystem::create_directories(dir.root);
    GET_MODULE(PathResolver).setup(dir.root, false);
    GraphImageSourceLoader loader{GET_MODULE(PathResolver)};
    const Json source{{"asset", "project://assets/en2/quad.png"}, {"alpha", "premultiply"}};
    try {
        (void)loader.load("logo", source);
        FAIL("a missing image file was read");
    } catch (const GraphRegistryError &error) {
        CHECK(error.code() == "graph.image_source_missing@1");
        CHECK(error.detail().at("node") == "logo");
        CHECK(error.detail().at("asset") == "project://assets/en2/quad.png");
        CHECK(error.detail().at("reason").is_string());
    }
    writeQuad(dir.root / "assets/en2/quad.png");
    const auto extent = loader.load("logo", source);
    CHECK(extent.width == 2);
    CHECK(extent.height == 2);
    CHECK(loader.files().size() == 1);
    const auto pixels = loader.pixels(RenderTargetSource{.asset = "project://assets/en2/quad.png",
                                                         .color_space = "srgb",
                                                         .alpha = "premultiply"});
    REQUIRE(pixels->rgba8.size() == 16);
    const auto byte = [&](std::size_t at) { return std::to_integer<int>(pixels->rgba8[at]); };
    // Opaque texels are as the file has them; the half-covered one is
    // multiplied in linear space.
    CHECK((byte(0) == 200 && byte(1) == 40 && byte(2) == 10 && byte(3) == 255));
    CHECK((byte(12) == 188 && byte(13) == 93 && byte(14) == 0 && byte(15) == 128));
    // In a linear file the bytes are multiplied as they are.
    const auto linear = graphImageSourcePixels(
        {std::byte{255}, std::byte{128}, std::byte{0}, std::byte{128}}, "linear", "premultiply");
    CHECK((std::to_integer<int>(linear[0]) == 128 && std::to_integer<int>(linear[1]) == 64));
}

TEST_CASE("EN-2 a graph fullscreen pass reads two images of one PNG file, straight and premultiplied",
          "[en2][image-source][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("fullscreen");
    const auto root = dir.root / "project";
    writeProject(root);
    const auto capture = dir.root / "capture.png";

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");

    // Twice: the image is uploaded once and keeps its pixels frame to frame.
    for (int frame = 0; frame < 2; ++frame) {
        INFO("frame " << frame);
        const auto rgba = rpc.frame(capture);
        // Left half: the straight file. Its sRGB bytes reach the shader
        // linear and the display encodes them back.
        CHECK(near(pixel(rgba, 8, 16), texel(0), 2));
        CHECK(near(pixel(rgba, 24, 16), texel(1), 2));
        CHECK(near(pixel(rgba, 8, 48), texel(2), 2));
        CHECK(near(pixel(rgba, 24, 48), texel(3), 2));
        // Right half: premultiplied. Opaque texels are unchanged.
        CHECK(near(pixel(rgba, 40, 16), texel(0), 2));
        CHECK(near(pixel(rgba, 56, 16), texel(1), 2));
        CHECK(near(pixel(rgba, 40, 48), texel(2), 2));
        CHECK(near(pixel(rgba, 56, 48), premultiplied_bottom_right, 2));
    }

    // Each image is the file's size, kept out of aliasing.
    auto &targets = GET_MODULE(RenderTargetContainer);
    for (const auto *name : {"en2_straight", "en2_premultiplied"}) {
        INFO(name);
        const auto id = targets.getRenderTargetIdByName(name);
        REQUIRE(isConcreteRenderTarget(id));
        const auto &image = targets.getImage(id);
        CHECK(image.extent.width == 2);
        CHECK(image.extent.height == 2);
        CHECK(image.format == vk::Format::eR8G8B8A8Srgb);
        CHECK(image.mip_levels == (std::string_view{name} == "en2_straight" ? 2u : 1u));
        CHECK_FALSE(targets.aliasGroup(id).has_value());
    }

    // The editor composes the document as written: the authoring context
    // takes each source's size from its file.
    (void)rpc.result("get_render_authoring_context");

    // The root rewritten on disk to read another file: the new generation
    // reads it and the straight half shows it.
    writeQuad(root / "assets/en2/reversed.png", true);
    const auto config_path = root / "passes/main_rendering_config.json";
    auto document = Json::parse(readFile(config_path));
    for (auto &node : document.at("nodes"))
        if (node.at("id") == "en2_straight")
            node["parameters"]["source"]["asset"] = "project://assets/en2/reversed.png";
    writeFile(config_path, document.dump(2));
    auto accepted = rpc.result("reload_render_documents");
    INFO(accepted.dump(2));
    REQUIRE(accepted.at("status") == "accepted");
    Json result;
    for (int attempt = 0; attempt < 8; ++attempt) {
        (void)rpc.result("step_frame");
        result = rpc.result("get_edit_result", {{"ticket", accepted.at("ticket")}});
        if (result.value("status", std::string{}) != "pending") break;
    }
    INFO(result.dump(2));
    REQUIRE(result.at("status") == "committed");
    const auto rgba = rpc.frame(capture);
    CHECK(near(pixel(rgba, 8, 16), texel(3), 2));
    CHECK(near(pixel(rgba, 24, 16), texel(2), 2));
    CHECK(near(pixel(rgba, 8, 48), texel(1), 2));
    CHECK(near(pixel(rgba, 24, 48), texel(0), 2));
    CHECK(near(pixel(rgba, 56, 48), premultiplied_bottom_right, 2));
    GET_MODULE(VulkanManageCore).waitIdle();
}

} // namespace Pelican
