#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/fullscreenpass/fullscreenpasscontainer.hpp"
#include "../src/core/fullscreenpass/passinputlayout.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/renderingpass/passshaderdefines.hpp"
#include "../src/core/renderingpass/renderingpasscontainer.hpp"
#include "../src/core/shader/pelican_sets.hpp"
#include "../src/core/shader/shaderreflection.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/watch/reloadservice.hpp"
#include "../src/project/projecttemplate.hpp"
#include "render_graph_root_test_support.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stb_image.h>
#include <string>
#include <vector>

// VI-1. Variadic pass inputs (PELICAN_DECLARE_VARIADIC_INPUTS), the buffer
// binding rule that follows from them, per-pass shader.defines and
// engine://passthrough. The GPU cases edit the blank template's `present`
// through the render config editor and read the running renderer back, as
// Studio does.

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
                   ("pelican_vi1_" + std::string{name} + "_" + std::to_string(suffix))};
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
    TestSupport::requireVulkanDevice("VI-1 variadic pass inputs");
    GET_MODULE(ECSPredefinedRegistration).reg();
    // reload_shaders resolves the production reload owner when the endpoint
    // is built.
    (void)GET_MODULE(watch::ReloadService);
}

struct Rpc {
    std::istringstream input;
    std::ostringstream output;
    EngineRpcEndpoint endpoint{input, output};
    std::uint64_t next_id = 7000;

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

// Every pixel within 8 of `expected`.
bool every(const std::vector<unsigned char> &rgba, Rgb expected) {
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const auto got = pixel(rgba, x, y);
            if (std::abs(got.r - expected.r) > 8 || std::abs(got.g - expected.g) > 8 ||
                std::abs(got.b - expected.b) > 8)
                return false;
        }
    }
    return true;
}

// The blank template's UV gradient: red grows along x, green along y, blue
// is one constant middle value.
bool gradient(const std::vector<unsigned char> &rgba) {
    const auto left = pixel(rgba, 1, 32);
    const auto right = pixel(rgba, 62, 32);
    const auto top = pixel(rgba, 32, 1);
    const auto bottom = pixel(rgba, 32, 62);
    const auto blue = pixel(rgba, 32, 32).b;
    return right.r - left.r > 100 && std::abs(bottom.g - top.g) > 100 && blue > 60 &&
           blue < 220 && std::abs(pixel(rgba, 1, 1).b - blue) <= 4 &&
           std::abs(pixel(rgba, 62, 62).b - blue) <= 4;
}

constexpr Rgb red{255, 0, 0};
constexpr Rgb green{0, 255, 0};
constexpr Rgb blue{0, 0, 255};
constexpr Rgb black{0, 0, 0};
constexpr Rgb magenta{255, 0, 255};

std::string solidFrag(Rgb color) {
    return "#version 460\nlayout(location = 0) in vec2 inUV;\n"
           "layout(location = 0) out vec4 outColor;\n"
           "void main() { outColor = vec4(" +
           std::to_string(color.r / 255.0) + ", " + std::to_string(color.g / 255.0) + ", " +
           std::to_string(color.b / 255.0) + ", 1.0); }\n";
}

const Json present_output{{"color", {"display"}}, {"depth", nullptr}};

Json passthroughShader(std::optional<Json> defines = std::nullopt) {
    Json shader{{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}};
    if (defines) shader["defines"] = *defines;
    return shader;
}

Json projectShader(std::string_view stem, std::optional<Json> defines = std::nullopt) {
    Json shader{{"vertex", "engine://fullscreen"},
                {"fragment", "project://shaders/" + std::string{stem}}};
    if (defines) shader["defines"] = *defines;
    return shader;
}

struct TargetSpec {
    std::string name;
    Rgb color;
};

// The blank template with one render target per spec, each drawn in its
// solid colour by its own fullscreen pass before `present`. `buffers` go
// into a project feature, as a graph root requires.
void writeProject(const std::filesystem::path &root, const std::vector<TargetSpec> &targets,
                  const Json &present, const Json &buffers = Json::array()) {
    initializeProjectTemplate(root, ProjectTemplate::Blank);
    auto target_decls = Json::array();
    auto passes = Json::array();
    for (const auto &target : targets) {
        target_decls.push_back({{"name", target.name},
                                {"extent_scale", 1.0},
                                {"format", "R16G16B16A16_SFLOAT"},
                                {"format_class", "explicit(R16G16B16A16_SFLOAT)"},
                                {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}});
        passes.push_back({{"name", "draw_" + target.name},
                          {"type", "fullscreen"},
                          {"input", Json::array()},
                          {"output", {{"color", {target.name}}, {"depth", nullptr}}},
                          {"shader",
                           {{"vertex", "engine://fullscreen"},
                            {"fragment", "project://shaders/solid_" + target.name}}}});
        writeFile(root / ("shaders/solid_" + target.name + ".frag"), solidFrag(target.color));
    }
    auto present_pass = present;
    present_pass["name"] = "present";
    present_pass["type"] = "fullscreen";
    present_pass["output"] = present_output;
    passes.push_back(present_pass);
    Json rendering{{"render_targets", std::move(target_decls)},
                   {"rendering_passes",
                    Json::array({{{"name", "main_render"}, {"passes", std::move(passes)}}})}};
    if (!buffers.empty()) rendering["buffers"] = buffers;
    writeFile(root / "passes/main_rendering_config.json",
              TestSupport::renderGraphRootWithTaskFeature(rendering, root).dump(2));
}

// Studio's edit of `present` (edit_preset_pass on the direct root), and the
// ticket's terminal result.
Json editPresent(Rpc &rpc, const Json &inputs, const Json &shader) {
    const auto context = rpc.result("get_render_authoring_context");
    const auto sent = rpc.result("edit_preset_pass",
                                 {{"base_source_digest", context.at("source_digest").at("hex")},
                                  {"graph", "main_render"},
                                  {"pass_name", "present"},
                                  {"input", inputs},
                                  {"output", present_output},
                                  {"shader", shader}});
    if (sent.value("status", std::string{}) != "accepted") return sent;
    for (int attempt = 0; attempt < 8; ++attempt) {
        (void)rpc.result("step_frame");
        auto result = rpc.result("get_edit_result", {{"ticket", sent.at("ticket")}});
        if (result.value("status", std::string{}) != "pending") return result;
    }
    FAIL("the present edit ticket stayed pending");
    return Json{};
}

void requireCommitted(const Json &result) {
    INFO(result.dump(2));
    REQUIRE(result.value("status", std::string{}) == "committed");
}

Json presentNode(Rpc &rpc) {
    const auto plan = rpc.result("get_frame_plan");
    for (const auto &node : plan.at("nodes")) {
        if (node.value("name", std::string{}) == "present" && node.value("kind", std::string{}) != "compute")
            return node;
    }
    FAIL("get_frame_plan has no present node");
    return Json{};
}

PassId presentPassId() {
    const auto &passes = GET_MODULE(RenderingPassContainer);
    std::optional<PassId> present;
    for (const auto id : passes.getRegisteredPassIds()) {
        for (const auto &pass : passes.getCompiledRenderingPass(id).passes) {
            if (pass.definition.name == "present") present = pass.pass_id;
        }
    }
    REQUIRE(present.has_value());
    return *present;
}

Json presentRootDeclaration(Rpc &rpc) {
    const auto context = rpc.result("get_render_authoring_context");
    for (const auto &graph : context.at("graphs")) {
        for (const auto &pass : graph.at("passes")) {
            if (pass.at("name") == "present") {
                REQUIRE(pass.at("editable_root_pass") == true);
                return pass.at("root_declaration");
            }
        }
    }
    FAIL("get_render_authoring_context has no present");
    return Json{};
}

Json declaration(std::uint32_t binding, std::string type, std::string name, bool variadic) {
    return Json{{"binding", binding}, {"type", std::move(type)}, {"name", std::move(name)},
                {"variadic", variadic}};
}

Json variadicDeclarations(std::uint32_t first, std::uint32_t max) {
    auto result = Json::array();
    for (std::uint32_t k = 0; k < max; ++k) {
        result.push_back(declaration(first + k, "sampled_image",
                                     "pelican_variadic_input_" + std::to_string(k), true));
    }
    return result;
}

// Fixed inputs 0-2, a variadic group at 3-10 and a storage buffer at 11 that
// it writes out.
constexpr std::string_view fixed3_variadic_frag = R"glsl(#version 460
#extension GL_GOOGLE_include_directive : require
#include "pelican_sets.glsl"
#include "pelican_view.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
PELICAN_DECLARE_INPUT_0(in_a);
PELICAN_DECLARE_INPUT_1(in_b);
PELICAN_DECLARE_INPUT_2(in_c);
PELICAN_DECLARE_VARIADIC_INPUTS(3, 8);
layout(std430, set = PELICAN_SET_PASS_INPUT, binding = 11) readonly buffer Vi1Value { vec4 value; } vi1_value;

void main() {
    outColor = vec4(vi1_value.value.rgb, 1.0);
}
)glsl";

// The same through a resource port: the generated include places the
// buffer, so the shader does not name its binding.
constexpr std::string_view fixed3_variadic_port_frag = R"glsl(#version 460
#extension GL_GOOGLE_include_directive : require
#include "pelican_sets.glsl"
#include "pelican_view.glsl"
#include "pelican_resource_ports.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
PELICAN_DECLARE_INPUT_0(in_a);
PELICAN_DECLARE_INPUT_1(in_b);
PELICAN_DECLARE_INPUT_2(in_c);
PELICAN_DECLARE_VARIADIC_INPUTS(3, 8);

void main() {
    outColor = vec4(pelican_load_vi1Value(0u), pelican_load_vi1Value(1u),
                    pelican_load_vi1Value(2u), 1.0);
}
)glsl";

constexpr std::string_view two_groups_frag = R"glsl(#version 460
#extension GL_GOOGLE_include_directive : require
#include "pelican_sets.glsl"
#include "pelican_view.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
PELICAN_DECLARE_VARIADIC_INPUTS(0, 2);
PELICAN_DECLARE_VARIADIC_INPUTS(4, 2);

void main() { outColor = vec4(1.0); }
)glsl";

constexpr std::string_view fixed_after_group_frag = R"glsl(#version 460
#extension GL_GOOGLE_include_directive : require
#include "pelican_sets.glsl"
#include "pelican_view.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
PELICAN_DECLARE_VARIADIC_INPUTS(0, 2);
PELICAN_DECLARE_INPUT_3(late);

void main() { outColor = PELICAN_VARIADIC_TEXTURE(0, inUV) + PELICAN_TEXTURE_2D_3(late, inUV); }
)glsl";

// Fixed only: declares bindings 0 and 1, reads 1.
constexpr std::string_view fixed_two_frag = R"glsl(#version 460
#extension GL_GOOGLE_include_directive : require
#include "pelican_sets.glsl"
#include "pelican_view.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
PELICAN_DECLARE_INPUT_0(in_a);
PELICAN_DECLARE_INPUT_1(in_b);

void main() { outColor = vec4(PELICAN_TEXTURE_2D_1(in_b, inUV).rgb, 1.0); }
)glsl";

// Reads only binding 0.
constexpr std::string_view fixed_one_frag = R"glsl(#version 460
#extension GL_GOOGLE_include_directive : require
#include "pelican_sets.glsl"
#include "pelican_view.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
PELICAN_DECLARE_INPUT_0(in_a);

void main() { outColor = vec4(PELICAN_TEXTURE_2D_0(in_a, inUV).rgb, 1.0); }
)glsl";

ReflectedBinding sampled(std::uint32_t binding, std::string name) {
    return ReflectedBinding{.set = PELICAN_SET_PASS_INPUT,
                            .binding = binding,
                            .type = vk::DescriptorType::eCombinedImageSampler,
                            .name = std::move(name),
                            .image_view_dimension = ReflectedImageViewDimension::two_d};
}

ReflectedBinding storage(std::uint32_t binding, std::string name) {
    return ReflectedBinding{.set = PELICAN_SET_PASS_INPUT,
                            .binding = binding,
                            .type = vk::DescriptorType::eStorageBuffer,
                            .name = std::move(name)};
}

ShaderReflection fixed3Variadic8() {
    ShaderReflection reflection;
    reflection.bindings = {sampled(0, "in_a"), sampled(1, "in_b"), sampled(2, "in_c")};
    for (std::uint32_t k = 0; k < 8; ++k)
        reflection.bindings.push_back(sampled(3 + k, "pelican_variadic_input_" + std::to_string(k)));
    reflection.bindings.push_back(storage(11, "vi1_value"));
    // Set 0 is not a pass input.
    reflection.bindings.push_back(
        ReflectedBinding{.set = 0, .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .name = "frame"});
    return reflection;
}

template <class Body>
FullscreenInputBindingMismatchInfo refusedLayout(Body &&body) {
    try {
        body();
    } catch (const FullscreenInputBindingMismatch &refused) {
        return refused.info();
    }
    FAIL("the layout was accepted");
    return {};
}

} // namespace

TEST_CASE("VI-1 a shader's pass-input layout is read from reflection and a broken variadic rule is refused",
          "[vi1]") {
    const auto layout = readPassInputLayout(fixed3Variadic8());
    REQUIRE(layout.variadic.has_value());
    CHECK(*layout.variadic == PassVariadicInputRange{.first = 3, .max = 8});
    REQUIRE(layout.declarations.size() == 12);
    CHECK(layout.declarations.front().name == "in_a");
    CHECK_FALSE(layout.declarations.front().variadic);
    CHECK(layout.declarations.at(3).variadic);
    CHECK(layout.declarations.at(10).name == "pelican_variadic_input_7");
    CHECK(layout.declarations.back().type == "storage_buffer");
    CHECK_FALSE(layout.declarations.back().variadic);

    // No group: nothing variadic.
    ShaderReflection fixed;
    fixed.bindings = {sampled(0, "in_a"), sampled(1, "in_b")};
    CHECK_FALSE(readPassInputLayout(fixed).variadic.has_value());

    SECTION("a fixed texture after the group") {
        auto reflection = fixed3Variadic8();
        reflection.bindings.push_back(sampled(12, "late"));
        const auto info = refusedLayout([&] { (void)readPassInputLayout(reflection); });
        CHECK(info.direction == fullscreenInputVariadicLayoutInvalid);
        CHECK(info.variadic_rule == passVariadicRuleDeclarationAfterGroup);
        CHECK(info.binding == 12);
        CHECK(info.kind == "texture");
        CHECK(info.variadic_range == PassVariadicInputRange{.first = 3, .max = 8});
        CHECK_THAT(FullscreenInputBindingMismatch{info}.what(),
                   Catch::Matchers::StartsWith(
                       "Fullscreen shader declares a pass input after its variadic inputs") &&
                       Catch::Matchers::ContainsSubstring("bindings 3-10") &&
                       Catch::Matchers::ContainsSubstring("buffers start at binding 11"));
    }
    SECTION("a buffer inside the group") {
        auto reflection = fixed3Variadic8();
        reflection.bindings.push_back(storage(5, "inside"));
        const auto info = refusedLayout([&] { (void)readPassInputLayout(reflection); });
        CHECK(info.variadic_rule == passVariadicRuleDeclarationAfterGroup);
        CHECK(info.binding == 5);
        CHECK(info.kind == "buffer");
    }
    SECTION("two groups (two stages that disagree)") {
        auto reflection = fixed3Variadic8();
        reflection.bindings.push_back(sampled(20, "pelican_variadic_input_0"));
        const auto info = refusedLayout([&] { (void)readPassInputLayout(reflection); });
        CHECK(info.variadic_rule == passVariadicRuleMultipleGroups);
        CHECK(info.binding == 20);
    }
    SECTION("a declaration named like the group that the macro did not make") {
        auto reflection = fixed3Variadic8();
        reflection.bindings.at(5).type = vk::DescriptorType::eStorageImage;
        const auto info = refusedLayout([&] { (void)readPassInputLayout(reflection); });
        CHECK(info.variadic_rule == passVariadicRuleMalformed);
        CHECK(info.binding == 5);
    }
}

TEST_CASE("VI-1 buffer inputs bind after the variadic group, or after the textures without one",
          "[vi1]") {
    const std::optional<PassVariadicInputRange> none;
    const std::optional<PassVariadicInputRange> group{PassVariadicInputRange{.first = 3, .max = 8}};
    CHECK(passBufferInputBinding(2, none, 0) == 2);
    CHECK(passBufferInputBinding(2, none, 1) == 3);
    for (const std::size_t textures : {std::size_t{0}, std::size_t{3}, std::size_t{7}, std::size_t{11}}) {
        CHECK(passBufferInputBinding(textures, group, 0) == 11);
        CHECK(passBufferInputBinding(textures, group, 2) == 13);
    }
    CHECK(compileLogShowsMultipleVariadicGroups(
        "ERROR: two.frag:8: 'pelican_variadic_first' : redefinition \n"));
    CHECK_FALSE(compileLogShowsMultipleVariadicGroups("ERROR: 'x' : redefinition"));
}

TEST_CASE("VI-1 shader.defines reads NAME and NAME=VALUE and refuses engine names by name", "[vi1]") {
    CHECK(parsePassShaderDefines(Json::object(), "p").empty());
    CHECK(parsePassShaderDefines(Json{{"defines", {"A", "PELICAN_PASSTHROUGH_INPUT=2", "B=vec4(1.0, 0.0)"}}},
                                 "p") ==
          std::vector<std::string>{"A", "PELICAN_PASSTHROUGH_INPUT=2", "B=vec4(1.0, 0.0)"});
    const auto refused = [](const Json &defines) {
        try {
            (void)parsePassShaderDefines(Json{{"defines", defines}}, "present");
        } catch (const PassShaderDefineError &error) {
            return error.info();
        }
        FAIL("accepted " + defines.dump());
        return PassShaderDefineErrorInfo{};
    };
    const auto reserved = refused({"A", "PELICAN_PASS_TEXTURE_INPUT_COUNT=3"});
    CHECK(reserved.reason == passShaderDefineReserved);
    CHECK(reserved.name == "PELICAN_PASS_TEXTURE_INPUT_COUNT");
    CHECK(reserved.index == 1);
    CHECK(PassShaderDefineError{reserved}.what() ==
          std::string{"Pass 'present' shader.defines entry 1 ('PELICAN_PASS_TEXTURE_INPUT_COUNT=3') "
                      "defines PELICAN_PASS_TEXTURE_INPUT_COUNT, which the engine defines itself "
                      "when it compiles the pass"});
    for (const auto *name : {"PELICAN_VARIADIC_INPUT_COUNT", "PELICAN_VIEW_COUNT", "PELICAN_MULTIVIEW",
                             "PELICAN_INPUT_0_LAYERED", "PELICAN_FEATURE_SHADOW",
                             "PELICAN_OUTPUT_UNORM_FALLBACK"}) {
        INFO(name);
        CHECK(refused({name}).reason == passShaderDefineReserved);
    }
    CHECK(refused({"1BAD"}).reason == passShaderDefineMalformed);
    CHECK(refused({"=3"}).reason == passShaderDefineMalformed);
    CHECK(refused({3}).reason == passShaderDefineMalformed);
    CHECK(refused({"A=1", "A=2"}).reason == passShaderDefineDuplicate);
    CHECK_THROWS(parsePassShaderDefines(Json{{"defines", "A"}}, "p"));
}

TEST_CASE("VI-1 engine://passthrough writes the input PELICAN_PASSTHROUGH_INPUT names, and the gradient "
          "with none, through the running renderer",
          "[vi1][render-config-editor][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("passthrough");
    const auto root = dir.root / "project";
    writeProject(root, {{"vi_red", red}, {"vi_green", green}, {"vi_blue", blue}},
                 Json{{"input", Json::array()}, {"shader", passthroughShader()}});
    const auto capture = dir.root / "capture.png";

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");

    // No inputs: the gradient, and the declarations the pass would take.
    CHECK(gradient(rpc.frame(capture)));
    {
        const auto node = presentNode(rpc);
        INFO(node.dump(2));
        REQUIRE(node.contains("pass_inputs"));
        CHECK(node.at("pass_inputs") == Json{{"state", "declared"},
                                             {"fragment", "engine://passthrough"},
                                             {"declarations", variadicDeclarations(0, 8)},
                                             {"variadic_range", {{"first", 0}, {"max", 8}}},
                                             {"pass_texture_inputs", 0},
                                             {"pass_buffer_inputs", 0},
                                             {"buffer_binding_base", 8}});
    }

    const auto show = [&](const Json &inputs, std::optional<int> selected) {
        const auto defines =
            selected ? std::optional<Json>{Json::array(
                           {"PELICAN_PASSTHROUGH_INPUT=" + std::to_string(*selected)})}
                     : std::nullopt;
        const auto result = editPresent(rpc, inputs, passthroughShader(defines));
        requireCommitted(result);
        return rpc.frame(capture);
    };

    // One input: it is shown. Every other variadic binding is filled with
    // the default, and that is not warned about.
    CHECK(every(show(Json::array({"vi_red"}), std::nullopt), red));
    CHECK(GET_MODULE(FullscreenPassContainer).filledPassInputs(presentPassId()).size() == 7);
    CHECK_FALSE(GET_MODULE(FullscreenPassContainer).filledPassInputReport(presentPassId()).has_value());
    CHECK(presentNode(rpc).at("pass_inputs").at("pass_texture_inputs") == 1);

    // Three inputs: 0 by default and by define, 2 by define.
    CHECK(every(show(Json::array({"vi_red", "vi_green", "vi_blue"}), std::nullopt), red));
    CHECK(every(show(Json::array({"vi_red", "vi_green", "vi_blue"}), 0), red));
    CHECK(every(show(Json::array({"vi_red", "vi_green", "vi_blue"}), 2), blue));
    CHECK(every(show(Json::array({"vi_green", "vi_blue", "vi_red"}), 2), red));

    // A number past the inputs the pass gives reads the default: black.
    CHECK(every(show(Json::array({"vi_red"}), 2), black));

    // shader.defines went through the editor and came back as written.
    const auto declared = presentRootDeclaration(rpc);
    CHECK(declared.at("shader") ==
          passthroughShader(Json::array({"PELICAN_PASSTHROUGH_INPUT=2"})));

    // Back to none: the gradient again.
    CHECK(gradient(show(Json::array(), std::nullopt)));
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("VI-1 a pass define the engine sets itself is refused by name through the editor",
          "[vi1][render-config-editor][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("reserved_define");
    const auto root = dir.root / "project";
    writeProject(root, {{"vi_red", red}}, Json{{"input", Json::array()}, {"shader", passthroughShader()}});
    const auto rendering_path = root / "passes/main_rendering_config.json";

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");
    const auto before = readFile(rendering_path);

    const auto refused = editPresent(rpc, Json::array({"vi_red"}),
                                     passthroughShader(Json::array({"PELICAN_PASS_TEXTURE_INPUT_COUNT=5"})));
    INFO(refused.dump(2));
    REQUIRE(refused.at("status") == "rejected");
    const auto &error = refused.at("error");
    CHECK(error.at("code") == "render_pipeline_preflight_failed");
    CHECK_THAT(error.at("message").get<std::string>(),
               Catch::Matchers::ContainsSubstring(
                   "defines PELICAN_PASS_TEXTURE_INPUT_COUNT, which the engine defines itself"));
    REQUIRE(error.contains("pass_shader_define"));
    CHECK(error.at("pass_shader_define") == Json{{"pass", "present"},
                                                 {"define", "PELICAN_PASS_TEXTURE_INPUT_COUNT=5"},
                                                 {"name", "PELICAN_PASS_TEXTURE_INPUT_COUNT"},
                                                 {"index", 0},
                                                 {"reason", "reserved"}});
    CHECK(readFile(rendering_path) == before);

    // An ordinary define is taken, and read back as written.
    const auto shader = passthroughShader(Json::array({"PELICAN_PASSTHROUGH_INPUT=0", "VI1_UNUSED"}));
    requireCommitted(editPresent(rpc, Json::array({"vi_red"}), shader));
    CHECK(presentRootDeclaration(rpc).at("shader") == shader);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("VI-1 a fixed-3 + variadic(3, 8) shader takes 3 to 11 textures and reads its buffer at binding 11, "
          "through the running renderer",
          "[vi1][render-config-editor][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("fixed3");
    const auto root = dir.root / "project";
    std::vector<TargetSpec> targets;
    for (int index = 0; index < 12; ++index) targets.push_back({"vi_t" + std::to_string(index), green});
    const Json buffers = Json::array({{{"name", "vi1_value"},
                                       {"size", 16},
                                       {"lifetime", "persistent"},
                                       {"host_source", "runtime_f32"},
                                       {"initial_f32", {1.0, 0.0, 1.0, 1.0}}}});
    writeProject(root, targets,
                 Json{{"input", Json::array({"vi_t0", "vi_t1", "vi_t2", "vi1_value"})},
                      {"shader", projectShader("fixed3_variadic")}},
                 buffers);
    writeFile(root / "shaders/fixed3_variadic.frag", fixed3_variadic_frag);
    const auto capture = dir.root / "capture.png";
    const auto rendering_path = root / "passes/main_rendering_config.json";

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");

    const auto inputs = [](int textures) {
        auto result = Json::array();
        for (int index = 0; index < textures; ++index) result.push_back("vi_t" + std::to_string(index));
        result.push_back("vi1_value");
        return result;
    };
    // The buffer is (1, 0, 1): magenta. A buffer at the wrong binding would
    // read the default (zeros): black.
    CHECK(every(rpc.frame(capture), magenta));
    {
        const auto node = presentNode(rpc);
        INFO(node.dump(2));
        auto expected = Json::array({declaration(0, "sampled_image", "in_a", false),
                                     declaration(1, "sampled_image", "in_b", false),
                                     declaration(2, "sampled_image", "in_c", false)});
        for (const auto &entry : variadicDeclarations(3, 8)) expected.push_back(entry);
        expected.push_back(declaration(11, "storage_buffer", "vi1_value", false));
        CHECK(node.at("pass_inputs") ==
              Json{{"state", "declared"},
                   {"fragment", "project://shaders/fixed3_variadic"},
                   {"declarations", expected},
                   {"variadic_range", {{"first", 3}, {"max", 8}}},
                   {"pass_texture_inputs", 3},
                   {"pass_buffer_inputs", 1},
                   {"buffer_binding_base", 11}});
    }
    // Nothing fixed is filled, so nothing is warned about.
    CHECK_FALSE(GET_MODULE(FullscreenPassContainer).filledPassInputReport(presentPassId()).has_value());

    for (const int textures : {7, 11}) {
        INFO(textures << " textures");
        requireCommitted(editPresent(rpc, inputs(textures), projectShader("fixed3_variadic")));
        CHECK(every(rpc.frame(capture), magenta));
        CHECK(presentNode(rpc).at("pass_inputs").at("pass_texture_inputs") == textures);
        CHECK(presentNode(rpc).at("pass_inputs").at("buffer_binding_base") == 11);
    }

    // Twelve do not fit: the twelfth would sit on the buffer's binding.
    const auto before = readFile(rendering_path);
    const auto refused = editPresent(rpc, inputs(12), projectShader("fixed3_variadic"));
    INFO(refused.dump(2));
    REQUIRE(refused.at("status") == "rejected");
    const auto &error = refused.at("error");
    CHECK(error.at("code") == "render_pipeline_preflight_failed");
    CHECK(error.at("message") ==
          "Fullscreen pass supplies more texture inputs than its shader takes: pass 'present' supplies 12 "
          "texture inputs, but fragment 'project://shaders/fixed3_variadic' takes at most 11 (variadic "
          "inputs at set 1 bindings 3-10); input 11 ('vi_t11') has no binding");
    const auto &mismatch = error.at("shader_input_mismatch");
    CHECK(mismatch.at("direction") == "variadic_capacity_exceeded");
    CHECK(mismatch.at("kind") == "texture");
    CHECK(mismatch.at("binding") == 11);
    CHECK(mismatch.at("resource") == "vi_t11");
    CHECK(mismatch.at("pass_texture_inputs") == 12);
    CHECK(mismatch.at("variadic_range") == Json{{"first", 3}, {"max", 8}});
    CHECK(mismatch.at("shader_pass_inputs").size() == 12);
    CHECK(readFile(rendering_path) == before);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("VI-1 a resource-port buffer of a variadic shader is generated at the group's end, "
          "through the running renderer",
          "[vi1][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("port");
    const auto root = dir.root / "project";
    std::vector<TargetSpec> targets;
    for (int index = 0; index < 3; ++index) targets.push_back({"vi_t" + std::to_string(index), green});
    const Json buffers = Json::array({{{"name", "vi1_value"},
                                       {"size", 16},
                                       {"lifetime", "persistent"},
                                       {"host_source", "runtime_f32"},
                                       {"initial_f32", {1.0, 0.0, 1.0, 1.0}}}});
    writeProject(root, targets,
                 Json{{"input", Json::array({"vi_t0", "vi_t1", "vi_t2", "vi1_value"})},
                      {"resource_ports",
                       {{"vi1Value",
                         {{"resource", "vi1_value"}, {"kind", "buffer"}, {"element", "float"},
                          {"access", "storage"}}}}},
                      {"shader", projectShader("fixed3_variadic_port")}},
                 buffers);
    writeFile(root / "shaders/fixed3_variadic_port.frag", fixed3_variadic_port_frag);
    const auto capture = dir.root / "capture.png";

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");
    CHECK(every(rpc.frame(capture), magenta));
    const auto node = presentNode(rpc);
    INFO(node.dump(2));
    const auto &declarations = node.at("pass_inputs").at("declarations");
    REQUIRE(declarations.size() == 12);
    CHECK(declarations.back().at("binding") == 11);
    CHECK(declarations.back().at("type") == "storage_buffer");
    CHECK(GET_MODULE(FullscreenPassContainer).resourceInterfaceForTesting(presentPassId()).back().binding ==
          11);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("VI-1 two variadic groups, and a fixed input after the group, are refused by name through the editor",
          "[vi1][render-config-editor][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("layout");
    const auto root = dir.root / "project";
    writeProject(root, {{"vi_red", red}, {"vi_green", green}},
                 Json{{"input", Json::array({"vi_red"})}, {"shader", passthroughShader()}});
    writeFile(root / "shaders/two_groups.frag", two_groups_frag);
    writeFile(root / "shaders/fixed_after_group.frag", fixed_after_group_frag);
    writeFile(root / "shaders/fixed_two.frag", fixed_two_frag);
    writeFile(root / "shaders/fixed_one.frag", fixed_one_frag);
    const auto rendering_path = root / "passes/main_rendering_config.json";

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");
    const auto before = readFile(rendering_path);

    {
        const auto refused = editPresent(rpc, Json::array({"vi_red"}), projectShader("two_groups"));
        INFO(refused.dump(2));
        REQUIRE(refused.at("status") == "rejected");
        const auto &error = refused.at("error");
        CHECK(error.at("code") == "render_pipeline_preflight_failed");
        CHECK(error.at("message") ==
              "Fullscreen shader declares more than one group of variadic pass inputs: fragment "
              "'project://shaders/two_groups' uses PELICAN_DECLARE_VARIADIC_INPUTS more than once; a "
              "shader declares one group, after its fixed inputs");
        const auto &mismatch = error.at("shader_input_mismatch");
        CHECK(mismatch.at("pass") == "present");
        CHECK(mismatch.at("direction") == "variadic_layout_invalid");
        CHECK(mismatch.at("variadic_rule") == "multiple_variadic_groups");
    }
    {
        const auto refused = editPresent(rpc, Json::array({"vi_red"}), projectShader("fixed_after_group"));
        INFO(refused.dump(2));
        REQUIRE(refused.at("status") == "rejected");
        const auto &mismatch = refused.at("error").at("shader_input_mismatch");
        CHECK(mismatch.at("direction") == "variadic_layout_invalid");
        CHECK(mismatch.at("variadic_rule") == "declaration_after_variadic_group");
        CHECK(mismatch.at("binding") == 3);
        CHECK(mismatch.at("variadic_range") == Json{{"first", 0}, {"max", 2}});
        CHECK(mismatch.at("shader_pass_inputs") ==
              Json::array({declaration(0, "sampled_image", "pelican_variadic_input_0", true),
                           declaration(1, "sampled_image", "pelican_variadic_input_1", true),
                           declaration(3, "sampled_image", "late", false)}));
        CHECK_THAT(refused.at("error").at("message").get<std::string>(),
                   Catch::Matchers::StartsWith(
                       "Fullscreen shader declares a pass input after its variadic inputs: fragment "
                       "'project://shaders/fixed_after_group' declares a sampled image at set 1 binding 3"));
    }
    CHECK(readFile(rendering_path) == before);

    // Without a group: fixed declarations, no range, and a filled fixed
    // binding is warned about as before.
    requireCommitted(editPresent(rpc, Json::array({"vi_red"}), projectShader("fixed_two")));
    const auto node = presentNode(rpc);
    INFO(node.dump(2));
    CHECK(node.at("pass_inputs") == Json{{"state", "declared"},
                                         {"fragment", "project://shaders/fixed_two"},
                                         {"declarations",
                                          Json::array({declaration(0, "sampled_image", "in_a", false),
                                                       declaration(1, "sampled_image", "in_b", false)})},
                                         {"variadic_range", nullptr},
                                         {"pass_texture_inputs", 1},
                                         {"pass_buffer_inputs", 0},
                                         {"buffer_binding_base", 1}});
    const auto report = GET_MODULE(FullscreenPassContainer).filledPassInputReport(presentPassId());
    REQUIRE(report.has_value());
    CHECK(report->binding == 1);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("VI-1 measurement: reloading a shader that drops a declaration its pass supplies",
          "[vi1][shader-reload][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("reload");
    const auto root = dir.root / "project";
    writeProject(root, {{"vi_red", red}, {"vi_green", green}},
                 Json{{"input", Json::array({"vi_red", "vi_green"})}, {"shader", projectShader("present2")}});
    // Declares both and shows the second.
    writeFile(root / "shaders/present2.frag", fixed_two_frag);
    const auto capture = dir.root / "capture.png";

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");
    REQUIRE(every(rpc.frame(capture), green));
    const auto generation_before = rpc.result("get_frame_plan").at("runtime_generation");

    // The shader now declares binding 0 only; the pass still gives two.
    writeFile(root / "shaders/present2.frag", fixed_one_frag);
    const auto reload = rpc.call("reload_shaders");
    std::cout << "VI-1 reload measurement: reload_shaders -> " << reload.dump() << "\n";
    // Frame by frame: what each step answers, what the frame plan says the
    // pipeline declares, and what a capture shows.
    std::vector<Json> steps;
    for (int frame = 0; frame < 3; ++frame) {
        steps.push_back(rpc.call("step_frame"));
        std::cout << "VI-1 reload measurement: step_frame " << frame << " -> " << steps.back().dump()
                  << "\n";
    }
    const auto plan = rpc.call("get_frame_plan");
    Json pass_inputs;
    if (plan.contains("result")) {
        for (const auto &node : plan.at("result").at("nodes"))
            if (node.value("name", std::string{}) == "present") pass_inputs = node.value("pass_inputs", Json{});
    }
    std::cout << "VI-1 reload measurement: pass_inputs after = " << pass_inputs.dump()
              << ", runtime_generation " << generation_before << " -> "
              << (plan.contains("result") ? plan.at("result").at("runtime_generation") : plan) << "\n";
    const auto rendered = rpc.call("render_frame");
    std::cout << "VI-1 reload measurement: render_frame -> " << rendered.dump() << "\n";
    std::filesystem::remove(capture);
    const auto captured = rpc.call("capture", {{"path", capture.generic_string()}});
    std::cout << "VI-1 reload measurement: capture -> " << captured.dump() << "\n";
    std::optional<Rgb> center;
    if (captured.contains("result") && std::filesystem::exists(capture)) {
        int width = 0, height = 0, channels = 0;
        if (stbi_uc *pixels = stbi_load(capture.string().c_str(), &width, &height, &channels, 4)) {
            std::vector<unsigned char> rgba(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
            stbi_image_free(pixels);
            center = pixel(rgba, 32, 32);
            std::cout << "VI-1 reload measurement: centre pixel = (" << center->r << "," << center->g << ","
                      << center->b << ")\n";
        }
    }
    const auto status = rpc.call("get_status");
    std::cout << "VI-1 reload measurement: get_status -> "
              << (status.contains("result") ? status.at("result").value("shader_reload", status.at("result")).dump()
                                            : status.dump())
              << "\n";

    // What is recorded (the VI-1 report), measured 2026-09-25:
    // - reload_shaders commits (the module compiles) and the pipeline is
    //   replaced; the reload status records no failure;
    // - the first frame after it fails at the pass's descriptor rebind with
    //   the Q-14e refusal naming the input the shader no longer declares, and
    //   step_frame returns that as its error;
    // - later frames run: the new pipeline is bound with the descriptor sets
    //   made for the old set-1 layout (the validation layer reports
    //   VUID-vkCmdBindDescriptorSets-pDescriptorSets-00358 every frame), and
    //   the frame plan shows the new declaration. What is drawn is not
    //   defined, so it is not checked here.
    INFO(reload.dump(2));
    REQUIRE(reload.contains("result"));
    CHECK(reload.at("result").at("committed") == true);
    REQUIRE(steps.size() == 3);
    {
        INFO(steps.front().dump(2));
        REQUIRE(steps.front().contains("error"));
        CHECK_THAT(steps.front().at("error").at("message").get<std::string>(),
                   Catch::Matchers::StartsWith(
                       "Fullscreen pass input texture/local-read contract does not match shader reflection: "
                       "input 1 ('vi_green') expects a sampled image at set 1 binding 1"));
    }
    for (std::size_t index = 1; index < steps.size(); ++index) {
        INFO(steps[index].dump(2));
        CHECK(steps[index].contains("result"));
    }
    CHECK(center.has_value());
    CHECK(pass_inputs.at("declarations").size() == 1);
    GET_MODULE(VulkanManageCore).waitIdle();
}

} // namespace Pelican
