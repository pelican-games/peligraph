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
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stb_image.h>
#include <string>
#include <utility>
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

// `debug_utils` turns VK_EXT_debug_utils on (the gpu_labels launch option),
// so a test can count what the validation layer reports (ValidationCounter).
void configureHeadless(const std::filesystem::path &root, bool debug_utils = false) {
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setProjectData(readFile(root / "project.json"));
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.rpc = true;
    launch.headless_extent = vk::Extent2D{64, 64};
    launch.shader_hot_reload = false;
    launch.gpu_labels = debug_utils;
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

// VI-1b: what the validation layer reports as an error while this lives,
// through a VK_EXT_debug_utils messenger on the engine's instance (the
// engine installs none of its own). Inactive -- and its counts meaningless --
// when the layer or the extension is off, as in a build without the layer.
class ValidationCounter {
  public:
    ValidationCounter() {
        auto &vulkan = GET_MODULE(VulkanManageCore);
        instance_ = vulkan.getInstance();
        if (!vulkan.getVulkanValidationStatus().enabled || !vulkan.getDebugUtils().getStatus().enabled) return;
        const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        destroy_ = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (create == nullptr || destroy_ == nullptr) return;
        VkDebugUtilsMessengerCreateInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback = &ValidationCounter::received;
        info.pUserData = this;
        active_ = create(instance_, &info, nullptr, &messenger_) == VK_SUCCESS;
    }
    ~ValidationCounter() {
        if (active_) destroy_(instance_, messenger_, nullptr);
    }
    ValidationCounter(const ValidationCounter &) = delete;
    ValidationCounter &operator=(const ValidationCounter &) = delete;

    [[nodiscard]] bool active() const noexcept { return active_; }
    // The message ids reported since the last call.
    std::vector<std::string> take() {
        std::lock_guard lock{mutex_};
        return std::exchange(ids_, {});
    }

  private:
    static VKAPI_ATTR VkBool32 VKAPI_CALL received(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                   VkDebugUtilsMessageTypeFlagsEXT,
                                                   const VkDebugUtilsMessengerCallbackDataEXT *data,
                                                   void *user) {
        auto *self = static_cast<ValidationCounter *>(user);
        std::lock_guard lock{self->mutex_};
        self->ids_.push_back(data != nullptr && data->pMessageIdName != nullptr ? data->pMessageIdName
                                                                                : "(unnamed)");
        return VK_FALSE;
    }

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_ = nullptr;
    bool active_ = false;
    std::mutex mutex_;
    std::vector<std::string> ids_;
};

std::string joined(const std::vector<std::string> &ids) {
    std::string result;
    for (const auto &id : ids) result += (result.empty() ? "" : ", ") + id;
    return result;
}

// VI-1b shaders for `present` with inputs [vi_red, vi_green].
// Reads both: red from 0, green from 1 -> yellow.
constexpr std::string_view reads_two_frag = R"glsl(#version 460
#extension GL_GOOGLE_include_directive : require
#include "pelican_sets.glsl"
#include "pelican_view.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
PELICAN_DECLARE_INPUT_0(in_a);
PELICAN_DECLARE_INPUT_1(in_b);

void main() {
    outColor = vec4(PELICAN_TEXTURE_2D_0(in_a, inUV).r, PELICAN_TEXTURE_2D_1(in_b, inUV).g, 0.0, 1.0);
}
)glsl";

// Declares a third and reads it: the default (0, 0, 0, 0) makes blue 1 ->
// white; a bound render target (alpha 1) would make it yellow.
constexpr std::string_view reads_three_frag = R"glsl(#version 460
#extension GL_GOOGLE_include_directive : require
#include "pelican_sets.glsl"
#include "pelican_view.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
PELICAN_DECLARE_INPUT_0(in_a);
PELICAN_DECLARE_INPUT_1(in_b);
PELICAN_DECLARE_INPUT_2(in_c);

void main() {
    outColor = vec4(PELICAN_TEXTURE_2D_0(in_a, inUV).r, PELICAN_TEXTURE_2D_1(in_b, inUV).g,
                    1.0 - PELICAN_TEXTURE_2D_2(in_c, inUV).a, 1.0);
}
)glsl";

// The fixed inputs become a variadic group of `max`, read the same way:
// white while the group's unsupplied slots read the default.
std::string readsVariadicFrag(int max) {
    return R"glsl(#version 460
#extension GL_GOOGLE_include_directive : require
#include "pelican_sets.glsl"
#include "pelican_view.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
PELICAN_DECLARE_VARIADIC_INPUTS(0, )glsl" +
           std::to_string(max) + R"glsl();

void main() {
    outColor = vec4(PELICAN_VARIADIC_TEXTURE(0, inUV).r, PELICAN_VARIADIC_TEXTURE(1, inUV).g,
                    1.0 - PELICAN_VARIADIC_TEXTURE(2, inUV).a, 1.0);
}
)glsl";
}

// A second group that reflection sees (the macro once, and a declaration
// named like the group's at another binding): no compile error, the rule
// refuses it.
constexpr std::string_view reflected_two_groups_frag = R"glsl(#version 460
#extension GL_GOOGLE_include_directive : require
#include "pelican_sets.glsl"
#include "pelican_view.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
PELICAN_DECLARE_VARIADIC_INPUTS(0, 2);
layout(set = PELICAN_SET_PASS_INPUT, binding = 6) uniform sampler2D pelican_variadic_input_3;

void main() {
    outColor = PELICAN_VARIADIC_TEXTURE(0, inUV) + texture(pelican_variadic_input_3, inUV);
}
)glsl";

// For a `present` with no inputs: declares nothing, then binding 0 (read as
// the default: red).
constexpr std::string_view declares_none_frag = R"glsl(#version 460
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(0.0, 0.0, 1.0, 1.0); }
)glsl";

constexpr std::string_view declares_one_default_frag = R"glsl(#version 460
#extension GL_GOOGLE_include_directive : require
#include "pelican_sets.glsl"
#include "pelican_view.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
PELICAN_DECLARE_INPUT_0(in_a);

void main() { outColor = vec4(1.0 - PELICAN_TEXTURE_2D_0(in_a, inUV).a, 0.0, 0.0, 1.0); }
)glsl";

constexpr Rgb yellow{255, 255, 0};
constexpr Rgb white{255, 255, 255};

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

// VI-1b. A shader reload is checked against each fullscreen pass's inputs by
// the rule edit and load use. One the pass's inputs disagree with is refused
// for that pass, which keeps drawing with the pipeline and descriptor sets it
// had; one that passes has its descriptor sets rebuilt with the new set-1
// layout. The validation layer's errors are counted through a debug-utils
// messenger (ValidationCounter).

namespace {

// The project the VI-1b cases share: `present` reads `inputs` through
// project://shaders/present2, first `first`.
struct ReloadFixture {
    TempDir dir;
    std::filesystem::path root;
    std::filesystem::path shader;
    std::filesystem::path capture;

    explicit ReloadFixture(std::string_view name, const Json &inputs = Json::array({"vi_red", "vi_green"}),
                           std::string_view first = reads_two_frag)
        : dir{makeTempDir(name)}, root{dir.root / "project"}, shader{root / "shaders/present2.frag"},
          capture{dir.root / "capture.png"} {
        writeProject(root, {{"vi_red", red}, {"vi_green", green}},
                     Json{{"input", inputs}, {"shader", projectShader("present2")}});
        writeFile(shader, first);
    }
};

Json shaderReloadStatus(Rpc &rpc) {
    return rpc.result("get_status").at("reload").at("runtime").at(std::string{watch::shaderReloadParticipantName});
}

// Every one of `frames` step_frame calls succeeds.
void requireFramesRun(Rpc &rpc, int frames) {
    for (int frame = 0; frame < frames; ++frame) {
        const auto step = rpc.call("step_frame");
        INFO("frame " << frame << ": " << step.dump(2));
        REQUIRE(step.contains("result"));
    }
}

// No validation error since the last take (when the layer is on).
void requireNoValidationErrors(ValidationCounter &validation) {
    const auto ids = validation.take();
    INFO("validation errors: " << joined(ids));
    CHECK(ids.empty());
}

void warnWithoutValidation(const ValidationCounter &validation) {
    if (!validation.active()) {
        WARN("the Vulkan validation layer or VK_EXT_debug_utils is off: validation errors are not counted");
    }
}

} // namespace

TEST_CASE("VI-1b a reload that drops a declaration the pass supplies is refused, and the pass keeps drawing "
          "the shader it had",
          "[vi1b][shader-reload][rpc][gpu]") {
    setupLogger(true);
    ReloadFixture fixture{"vi1b_drop"};
    FastModuleContainer modules;
    configureHeadless(fixture.root, true);
    ValidationCounter validation;
    warnWithoutValidation(validation);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");
    REQUIRE(every(rpc.frame(fixture.capture), yellow));
    requireNoValidationErrors(validation);
    const auto two = Json::array({declaration(0, "sampled_image", "in_a", false),
                                  declaration(1, "sampled_image", "in_b", false)});
    const auto one = Json::array({declaration(0, "sampled_image", "in_a", false)});

    // The shader now declares binding 0 only; the pass still gives two.
    writeFile(fixture.shader, fixed_one_frag);
    const auto reloaded = rpc.result("reload_shaders");
    INFO(reloaded.dump(2));
    CHECK(reloaded.at("committed") == true);
    CHECK(reloaded.at("changed") == Json::array({"project://shaders/present2.frag"}));
    REQUIRE(reloaded.at("refused_passes").size() == 1);
    const auto refused = reloaded.at("refused_passes").at(0);
    CHECK(refused.at("pass") == "present");
    CHECK(refused.at("fragment") == "project://shaders/present2");
    CHECK(refused.at("declarations") == one);
    const std::string reason =
        "Fullscreen pass input texture/local-read contract does not match shader reflection: pass 'present' "
        "input 1 ('vi_green') expects a sampled image at set 1 binding 1, but fragment "
        "'project://shaders/present2' declares no pass input at that binding (shader declares 1 pass input)";
    CHECK(refused.at("reason") == reason);
    const auto mismatch = refused.at("shader_input_mismatch");
    CHECK(mismatch.at("pass") == "present");
    CHECK(mismatch.at("direction") == "pass_input_undeclared");
    CHECK(mismatch.at("kind") == "texture");
    CHECK(mismatch.at("binding") == 1);
    CHECK(mismatch.at("resource") == "vi_green");
    CHECK(mismatch.at("shader_pass_inputs") == one);

    // Ten frames run, and draw the shader from before the reload.
    requireFramesRun(rpc, 10);
    CHECK(every(rpc.frame(fixture.capture), yellow));
    {
        const auto node = presentNode(rpc);
        INFO(node.dump(2));
        const auto &pass_inputs = node.at("pass_inputs");
        CHECK(pass_inputs.at("state") == "declared");
        CHECK(pass_inputs.at("declarations") == two);
        REQUIRE(pass_inputs.contains("reload_refused"));
        CHECK(pass_inputs.at("reload_refused") ==
              Json{{"reason", reason}, {"shader_input_mismatch", mismatch}, {"declarations", one}});
    }
    {
        // Reload status keeps it as a failure, and names the pass.
        const auto status = shaderReloadStatus(rpc);
        INFO(status.dump(2));
        CHECK(status.at("failed") == 1);
        CHECK(status.at("error_code") == "shader_reload_refused@1");
        CHECK(status.at("last_error") ==
              "Shader reload refused for pass 'present', which keeps drawing with its previous shader: " +
                  reason);
        CHECK(status.at("error_fields").at("refused_passes") == Json::array({refused}));
        CHECK(status.at("details").at("refused_passes") == Json::array({refused}));
    }
    auto &container = GET_MODULE(FullscreenPassContainer);
    CHECK(container.inputDescriptorSetLayoutForTesting(presentPassId()) ==
          container.pipelineInputSetLayoutForTesting(presentPassId()));
    GET_MODULE(VulkanManageCore).waitIdle();
    requireNoValidationErrors(validation);

    // The declaration back: taken, and the refusal is gone.
    writeFile(fixture.shader, reads_two_frag);
    const auto restored = rpc.result("reload_shaders");
    INFO(restored.dump(2));
    CHECK(restored.at("committed") == true);
    CHECK(restored.at("refused_passes") == Json::array());
    requireFramesRun(rpc, 3);
    CHECK(every(rpc.frame(fixture.capture), yellow));
    {
        const auto node = presentNode(rpc);
        INFO(node.dump(2));
        CHECK(node.at("pass_inputs").at("declarations") == two);
        CHECK_FALSE(node.at("pass_inputs").contains("reload_refused"));
        const auto status = shaderReloadStatus(rpc);
        CHECK(status.at("last_error").is_null());
        CHECK(status.at("details").at("refused_passes") == Json::array());
    }
    GET_MODULE(VulkanManageCore).waitIdle();
    requireNoValidationErrors(validation);
}

TEST_CASE("VI-1b one reload that a pass refuses still publishes the other passes' shaders",
          "[vi1b][shader-reload][rpc][gpu]") {
    setupLogger(true);
    ReloadFixture fixture{"vi1b_partial"};
    FastModuleContainer modules;
    configureHeadless(fixture.root, true);
    ValidationCounter validation;
    warnWithoutValidation(validation);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");
    REQUIRE(every(rpc.frame(fixture.capture), yellow));

    // One reload: vi_red's pass now draws blue, and present's shader drops
    // binding 1. present keeps its shader (red from input 0, green from
    // input 1): (0, 255, 0). Had it taken the new one (input 0 as it is),
    // it would draw blue; had the whole reload been rolled back, yellow.
    writeFile(fixture.root / "shaders/solid_vi_red.frag", solidFrag(blue));
    writeFile(fixture.shader, fixed_one_frag);
    const auto reloaded = rpc.result("reload_shaders");
    INFO(reloaded.dump(2));
    CHECK(reloaded.at("committed") == true);
    CHECK(reloaded.at("changed").size() == 2);
    REQUIRE(reloaded.at("refused_passes").size() == 1);
    CHECK(reloaded.at("refused_passes").at(0).at("pass") == "present");
    requireFramesRun(rpc, 3);
    CHECK(every(rpc.frame(fixture.capture), green));
    GET_MODULE(VulkanManageCore).waitIdle();
    requireNoValidationErrors(validation);
}

TEST_CASE("VI-1b a reload that adds, widens or turns declarations variadic rebuilds the pass's descriptor "
          "sets with the new layout",
          "[vi1b][shader-reload][rpc][gpu]") {
    setupLogger(true);
    ReloadFixture fixture{"vi1b_add"};
    FastModuleContainer modules;
    configureHeadless(fixture.root, true);
    ValidationCounter validation;
    warnWithoutValidation(validation);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");
    REQUIRE(every(rpc.frame(fixture.capture), yellow));
    auto &container = GET_MODULE(FullscreenPassContainer);
    const auto layout_before = container.inputDescriptorSetLayoutForTesting(presentPassId());
    REQUIRE(layout_before);
    CHECK(container.filledDescriptorWritesForTesting(presentPassId()) == 0);

    const auto reload = [&](std::string_view source) {
        writeFile(fixture.shader, source);
        const auto reloaded = rpc.result("reload_shaders");
        INFO(reloaded.dump(2));
        CHECK(reloaded.at("committed") == true);
        CHECK(reloaded.at("refused_passes") == Json::array());
        requireFramesRun(rpc, 3);
    };
    const auto filledBindings = [&] {
        std::vector<std::uint32_t> bindings;
        for (const auto &entry : container.filledPassInputs(presentPassId())) bindings.push_back(entry.binding);
        return bindings;
    };

    // Binding 2 added and read: the default (alpha 0) makes blue 1 -> white.
    reload(reads_three_frag);
    CHECK(every(rpc.frame(fixture.capture), white));
    CHECK(filledBindings() == std::vector<std::uint32_t>{2});
    // One default write per descriptor set (one variant, two parities), into
    // sets allocated with the pipeline's new set-1 layout.
    CHECK(container.filledDescriptorWritesForTesting(presentPassId()) == 2);
    const auto layout_three = container.pipelineInputSetLayoutForTesting(presentPassId());
    CHECK(layout_three != layout_before);
    CHECK(container.inputDescriptorSetLayoutForTesting(presentPassId()) == layout_three);
    CHECK(presentNode(rpc).at("pass_inputs").at("declarations").size() == 3);
    GET_MODULE(VulkanManageCore).waitIdle();
    requireNoValidationErrors(validation);

    // The fixed inputs become a variadic group of four, then eight.
    for (const int max : {4, 8}) {
        INFO("variadic group of " << max);
        reload(readsVariadicFrag(max));
        CHECK(every(rpc.frame(fixture.capture), white));
        std::vector<std::uint32_t> expected;
        for (int binding = 2; binding < max; ++binding) expected.push_back(static_cast<std::uint32_t>(binding));
        CHECK(filledBindings() == expected);
        CHECK(container.filledDescriptorWritesForTesting(presentPassId()) == 2 * expected.size());
        CHECK(container.inputDescriptorSetLayoutForTesting(presentPassId()) ==
              container.pipelineInputSetLayoutForTesting(presentPassId()));
        CHECK_FALSE(container.filledPassInputReport(presentPassId()).has_value());
        const auto node = presentNode(rpc);
        INFO(node.dump(2));
        CHECK(node.at("pass_inputs").at("variadic_range") == Json{{"first", 0}, {"max", max}});
        CHECK_FALSE(node.at("pass_inputs").contains("reload_refused"));
        GET_MODULE(VulkanManageCore).waitIdle();
        requireNoValidationErrors(validation);
    }
}

TEST_CASE("VI-1b a reload that gives a pass without inputs a declaration binds the default for it",
          "[vi1b][shader-reload][rpc][gpu]") {
    setupLogger(true);
    ReloadFixture fixture{"vi1b_none", Json::array(), declares_none_frag};
    FastModuleContainer modules;
    configureHeadless(fixture.root, true);
    ValidationCounter validation;
    warnWithoutValidation(validation);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");
    REQUIRE(every(rpc.frame(fixture.capture), blue));
    auto &container = GET_MODULE(FullscreenPassContainer);
    CHECK_FALSE(container.inputDescriptorSetLayoutForTesting(presentPassId()));

    writeFile(fixture.shader, declares_one_default_frag);
    const auto reloaded = rpc.result("reload_shaders");
    INFO(reloaded.dump(2));
    CHECK(reloaded.at("refused_passes") == Json::array());
    requireFramesRun(rpc, 3);
    // Binding 0 reads the default (alpha 0): red. Unbound, it read black.
    CHECK(every(rpc.frame(fixture.capture), red));
    CHECK(container.filledPassInputs(presentPassId()).size() == 1);
    CHECK(container.filledDescriptorWritesForTesting(presentPassId()) == 2);
    CHECK(container.inputDescriptorSetLayoutForTesting(presentPassId()) ==
          container.pipelineInputSetLayoutForTesting(presentPassId()));
    GET_MODULE(VulkanManageCore).waitIdle();
    requireNoValidationErrors(validation);
}

TEST_CASE("VI-1b a reload to two variadic groups is refused, and the pass keeps drawing the shader it had",
          "[vi1b][shader-reload][rpc][gpu]") {
    setupLogger(true);
    ReloadFixture fixture{"vi1b_groups"};
    FastModuleContainer modules;
    configureHeadless(fixture.root, true);
    ValidationCounter validation;
    warnWithoutValidation(validation);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");
    REQUIRE(every(rpc.frame(fixture.capture), yellow));

    // The macro twice does not compile: the whole reload fails as a compile
    // does, and says what the pass compiler would have.
    writeFile(fixture.shader, two_groups_frag);
    {
        const auto failed = rpc.call("reload_shaders");
        INFO(failed.dump(2));
        REQUIRE(failed.contains("error"));
        const auto &data = failed.at("error").at("data");
        CHECK(data.at("error_code") == "shader_reload_failed@1");
        CHECK(data.at("error_fields").at("shader_input_mismatch").at("direction") == "variadic_layout_invalid");
        CHECK(data.at("error_fields").at("shader_input_mismatch").at("variadic_rule") ==
              "multiple_variadic_groups");
    }
    requireFramesRun(rpc, 10);
    CHECK(every(rpc.frame(fixture.capture), yellow));
    GET_MODULE(VulkanManageCore).waitIdle();
    requireNoValidationErrors(validation);

    // A second group reflection sees: it compiles, and the rule refuses it
    // for the pass.
    writeFile(fixture.shader, reflected_two_groups_frag);
    const auto reloaded = rpc.result("reload_shaders");
    INFO(reloaded.dump(2));
    CHECK(reloaded.at("committed") == true);
    REQUIRE(reloaded.at("refused_passes").size() == 1);
    const auto mismatch = reloaded.at("refused_passes").at(0).at("shader_input_mismatch");
    CHECK(mismatch.at("pass") == "present");
    CHECK(mismatch.at("direction") == "variadic_layout_invalid");
    CHECK(mismatch.at("variadic_rule") == "multiple_variadic_groups");
    CHECK(mismatch.at("binding") == 6);
    requireFramesRun(rpc, 10);
    CHECK(every(rpc.frame(fixture.capture), yellow));
    const auto node = presentNode(rpc);
    INFO(node.dump(2));
    CHECK(node.at("pass_inputs").at("declarations").size() == 2);
    CHECK(node.at("pass_inputs").at("reload_refused").at("declarations") ==
          Json::array({declaration(0, "sampled_image", "pelican_variadic_input_0", true),
                       declaration(1, "sampled_image", "pelican_variadic_input_1", true),
                       declaration(6, "sampled_image", "pelican_variadic_input_3", true)}));
    GET_MODULE(VulkanManageCore).waitIdle();
    requireNoValidationErrors(validation);
}

TEST_CASE("VI-1b the file watch's shader reload refuses a pass the same way, and reload status keeps it",
          "[vi1b][shader-reload][rpc][gpu]") {
    setupLogger(true);
    ReloadFixture fixture{"vi1b_watch"};
    FastModuleContainer modules;
    configureHeadless(fixture.root, true);
    ValidationCounter validation;
    warnWithoutValidation(validation);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");
    REQUIRE(every(rpc.frame(fixture.capture), yellow));

    // What the file watch hands the reload service for the write: the
    // shader participant's batch, billed at the next render boundary.
    writeFile(fixture.shader, fixed_one_frag);
    const std::array requests{
        watch::ReloadRequest{watch::makeAssetKey("shaders/present2.frag"), watch::ReloadKind::modified, {}, 1}};
    CHECK(GET_MODULE(watch::ReloadService).applyRequestsForTesting(requests) == std::vector<bool>{true});
    requireFramesRun(rpc, 10);
    CHECK(every(rpc.frame(fixture.capture), yellow));
    {
        const auto status = shaderReloadStatus(rpc);
        INFO(status.dump(2));
        CHECK(status.at("failed") == 1);
        CHECK(status.at("error_code") == "shader_reload_refused@1");
        CHECK_THAT(status.at("last_error").get<std::string>(),
                   Catch::Matchers::StartsWith("Shader reload refused for pass 'present'"));
        REQUIRE(status.at("error_fields").at("refused_passes").size() == 1);
        CHECK(status.at("error_fields").at("refused_passes").at(0).at("shader_input_mismatch").at("binding") == 1);
        CHECK(status.at("details").at("refused_passes").size() == 1);
    }
    const auto node = presentNode(rpc);
    INFO(node.dump(2));
    CHECK(node.at("pass_inputs").at("declarations").size() == 2);
    CHECK(node.at("pass_inputs").at("reload_refused").at("declarations") ==
          Json::array({declaration(0, "sampled_image", "in_a", false)}));
    GET_MODULE(VulkanManageCore).waitIdle();
    requireNoValidationErrors(validation);
}

} // namespace Pelican
