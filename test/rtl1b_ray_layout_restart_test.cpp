// RTL-1b: whether a project has a ray group layout is decided once per engine
// start (the ray layout mode). A reload that adds one is committed with
// restart_required (restart_reason ray_layout_mode_changed) only after every
// other check of the candidate passed: a task still in the form without a
// layout, a hit group left out or a miss named wrong is refused by its own
// code and fields. A layout project whose task is broken starts the engine's
// fallback generation under --render-fallback instead of stopping the engine.
// Driven through the production path: a headless `pelican_player --rpc
// --render-fallback` child, line by line over its stdin/stdout.

#include "../src/project/projecttemplate.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>
// The decoder is this executable's own: it links no engine library that has it.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

std::string readFile(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void writeFile(const std::filesystem::path &path, const std::string &text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output << text;
}

constexpr const char *root_reference = "passes/main_rendering_config.json";
constexpr const char *feature_reference = "features/rtl1b.json";
const std::string feature_document = std::string{"project://"} + feature_reference;
constexpr const char *task_name = "rtl1b_trace";

// The ray generation shader of either form: one ray per pixel of ray_color,
// from the camera; orange where it hits, a dim blue where it does not.
// `layout`: the form with a ray group layout, which names its ray type and
// miss through the generated header.
std::string raygen(bool layout) {
    std::string text =
        "#version 460\n"
        "#extension GL_GOOGLE_include_directive : enable\n"
        "#extension GL_EXT_ray_tracing : require\n"
        "#include \"pelican_sets.glsl\"\n"
        "#include \"pelican_frame.glsl\"\n";
    if (layout) text += "#include \"__pelican_ray_group_layout.glsl\"\n";
    text +=
        "layout(rgba16f, set = PELICAN_SET_PASS_INPUT, binding = 0)\n"
        "    writeonly uniform image2D pelican_resource_ray_color;\n"
        "layout(set = PELICAN_SET_FRAME, binding = PELICAN_RAY_QUERY_TLAS_BINDING)\n"
        "    uniform accelerationStructureEXT pelicanRayTracingScene;\n"
        "layout(location = 0) rayPayloadEXT vec4 hit;\n"
        "void main() {\n"
        "    vec2 pixel = vec2(gl_LaunchIDEXT.xy) + vec2(0.5);\n"
        "    vec2 ndc = pixel / vec2(gl_LaunchSizeEXT.xy) * 2.0 - 1.0;\n"
        "    vec4 target = inverse(pelicanFrame.projection) * vec4(ndc, 0.5, 1.0);\n"
        "    vec3 direction = normalize(target.xyz / target.w);\n"
        "    direction = normalize((inverse(pelicanFrame.view) * vec4(direction, 0.0)).xyz);\n"
        "    vec3 origin = (inverse(pelicanFrame.view) * vec4(0.0, 0.0, 0.0, 1.0)).xyz;\n"
        "    hit = vec4(0.0);\n";
    text += layout ? "    traceRayEXT(pelicanRayTracingScene, gl_RayFlagsOpaqueEXT, 0xff, PELICAN_RAY_TYPE_primary,\n"
                     "                pelican_ray_type_count, PELICAN_MISS_sky, origin, 0.001, direction, 1000.0, 0);\n"
                   : "    traceRayEXT(pelicanRayTracingScene, gl_RayFlagsOpaqueEXT, 0xff, 0, 0, 0,\n"
                     "                origin, 0.001, direction, 1000.0, 0);\n";
    text +=
        // RTL-1c: green where a closest hit of a group of its own (marked)
        // answered.
        "    vec3 color = hit.w > 1.5 ? vec3(0.0, 1.0, 0.0)\n"
        "               : hit.w > 0.5 ? vec3(1.0, 0.5, 0.0) : vec3(0.05, 0.1, 0.4);\n"
        "    imageStore(pelican_resource_ray_color, ivec2(gl_LaunchIDEXT.xy), vec4(color, 1.0));\n"
        "}\n";
    return text;
}

constexpr const char *miss_shader =
    "#version 460\n"
    "#extension GL_EXT_ray_tracing : require\n"
    "layout(location = 0) rayPayloadInEXT vec4 hit;\n"
    "void main() { hit.w = 0.0; }\n";

constexpr const char *closest_hit_shader =
    "#version 460\n"
    "#extension GL_EXT_ray_tracing : require\n"
    "layout(location = 0) rayPayloadInEXT vec4 hit;\n"
    "void main() { hit = vec4(gl_WorldRayOriginEXT + gl_HitTEXT * gl_WorldRayDirectionEXT, 1.0); }\n";

// One ray type, one triangles group (every scene row goes to the default
// group), one miss, no record.
const Json layout_json{{"schema", "pelican.ray_group_layout"},
                       {"version", 3},
                       {"ray_types", Json::array({"primary"})},
                       {"default_group", "mesh"},
                       {"groups", Json::array({{{"name", "mesh"}, {"type", "triangles"}}})},
                       {"misses", Json::array({{{"name", "sky"}}})},
                       {"record", Json::array()}};

Json legacyRayTracing() {
    return {{"raygen", "project://shaders/legacy"},
            {"miss", "project://shaders/legacy"},
            {"closesthit", "project://shaders/legacy"}};
}

Json layoutRayTracing() {
    return {{"raygen", "project://shaders/layout"},
            {"traced_ray_types", Json::array({"primary"})},
            {"misses", {{"sky", "project://shaders/layout"}}},
            {"hit_shaders", {{"primary", {{"mesh", {{"closesthit", "project://shaders/layout"}}}}}}}};
}

// The project's one ray tracing task, in a feature document of its own (as
// the ray tracing demos author theirs), writing ray_color before present.
Json featureWith(const Json &ray_tracing) {
    return {{"schema", "pelican.render_feature"},
            {"version", 1},
            {"name", "rtl1b"},
            {"runtime_shader_compiler", "required"},
            {"required_capabilities", Json::array({"pelican.vulkan.ray_tracing_pipeline@1"})},
            {"render_targets",
             Json::array({{{"name", "ray_color"},
                           {"extent_scale", 1.0},
                           {"format", "R16G16B16A16_SFLOAT"},
                           {"format_class", "explicit(R16G16B16A16_SFLOAT)"},
                           {"usage", Json::array({"STORAGE", "SAMPLED"})}}})},
            {"compute_tasks",
             Json::array({{{"name", task_name},
                           {"before", Json::array({"present"})},
                           {"ray_tracing", ray_tracing},
                           {"writes", Json::array({"ray_color"})},
                           {"resource_ports", {{"ray_color", {{"resource", "ray_color"}, {"access", "storage"}}}}},
                           {"dispatch", {{"rays_from", {{"port", "ray_color"}}}}}}})}};
}

// A Blank project (under %TEMP%) with a cube for the rays to hit and the
// shaders of either form.
std::filesystem::path blankProjectWithCube(std::string_view name) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("pelican_rtl1b_" + std::string{name} + "_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    (void)Pelican::initializeProjectTemplate(root, Pelican::ProjectTemplate::Blank);
    auto scene = Json::parse(readFile(root / "scenes/main.scene.json"));
    scene.at("scenes").at("default_scene").at("objects").push_back(
        {{"name", "Cube"},
         {"components",
          Json::array({{{"name", "transform"},
                        {"pos", Json::array({0.0, 0.0, 0.0})},
                        {"rotation", Json::array({0.0, 0.0, 0.0, 1.0})},
                        {"scale", Json::array({1.0, 1.0, 1.0})}},
                       {{"name", "simplemodelview"}, {"model", "engine://primitives/cube"}}})}});
    writeFile(root / "scenes/main.scene.json", scene.dump(2) + "\n");
    for (const auto *form : {"legacy", "layout"}) {
        writeFile(root / "shaders" / (std::string{form} + ".rgen"), raygen(std::string_view{form} == "layout"));
        writeFile(root / "shaders" / (std::string{form} + ".rmiss"), miss_shader);
        writeFile(root / "shaders" / (std::string{form} + ".rchit"), closest_hit_shader);
    }
    return root;
}

// Blank + a cube for the rays to hit + the task's feature; present shows
// ray_color. `layout`: the root names the ray group layout.
struct RtProject {
    std::filesystem::path root;
    std::string blank_root;

    RtProject(std::string_view name, bool layout, const Json &ray_tracing) {
        root = blankProjectWithCube(name);
        blank_root = readFile(rootPath());
        writeRoot(layout);
        writeFeature(ray_tracing);
        if (layout) writeLayout();
    }
    ~RtProject() {
        // Kept for a look when the run asks for it.
        if (std::getenv("PELICAN_RTL1B_KEEP") != nullptr) return;
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    std::filesystem::path rootPath() const { return root / root_reference; }
    std::filesystem::path featurePath() const { return root / feature_reference; }
    std::filesystem::path stderrLog(std::string_view run) const {
        return root / ("rtl1b." + std::string{run} + ".stderr.log");
    }
    void writeRoot(bool layout) const {
        auto document = Json::parse(blank_root);
        auto &rendering = document.at("rendering");
        rendering["features"] = Json::array({feature_document});
        rendering.at("rendering_passes").at(0).at("passes").at(0).at("input") = Json::array({"ray_color"});
        if (layout) rendering["ray_group_layout"] = "project://ray_layout.json";
        writeFile(rootPath(), document.dump(2) + "\n");
    }
    void writeFeature(const Json &ray_tracing) const {
        writeFile(featurePath(), featureWith(ray_tracing).dump(2) + "\n");
    }
    void writeLayout() const { writeFile(root / "ray_layout.json", layout_json.dump(2) + "\n"); }
};

// RTL-1c: the project's ray tracing task as a node of Blank's root graph
// (render.compute, as Studio's palette writes it): its image a render.image
// node beside it, present showing that image.
constexpr const char *graph_task = "trace";
constexpr const char *graph_image = "trace__ray_color";

Json sceneColorType() {
    return {{"semantic", "pelican.render.color_signal@1"},
            {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}};
}

// The layout RTL-1c adds: the default group and a second one (marked) a
// material binding moves the cube to; its closest hit answers 2.
const Json graph_layout_json{{"schema", "pelican.ray_group_layout"},
                             {"version", 3},
                             {"ray_types", Json::array({"primary"})},
                             {"default_group", "mesh"},
                             {"groups", Json::array({{{"name", "mesh"}, {"type", "triangles"}},
                                                     {{"name", "marked"}, {"type", "triangles"}}})},
                             {"misses", Json::array({{{"name", "sky"}}})},
                             {"record", Json::array()}};

constexpr const char *marked_closest_hit_shader =
    "#version 460\n"
    "#extension GL_EXT_ray_tracing : require\n"
    "layout(location = 0) rayPayloadInEXT vec4 hit;\n"
    "void main() { hit = vec4(0.0, 0.0, 0.0, 2.0); }\n";

Json graphLayoutRayTracing() {
    auto ray_tracing = layoutRayTracing();
    ray_tracing["hit_shaders"]["primary"]["marked"] = {{"closesthit", "project://shaders/marked"}};
    return ray_tracing;
}

Json graphTaskParameters(const Json &ray_tracing) {
    return {{"ray_tracing", ray_tracing},
            {"dispatch", {{"rays_from", {{"port", "ray_color"}}}}},
            {"ports",
             {{"ray_color",
               {{"kind", "image"}, {"type", sceneColorType()}, {"access", "write"}, {"binding", "storage"}}}}}};
}

struct GraphRtProject {
    std::filesystem::path root;

    explicit GraphRtProject(std::string_view name) {
        root = blankProjectWithCube(name);
        writeFile(root / "shaders/marked.rchit", marked_closest_hit_shader);
        auto document = Json::parse(readFile(rootPath()));
        document["nodes"] = Json::array(
            {{{"id", graph_image},
              {"op", "render.image"},
              {"parameters",
               {{"type", sceneColorType()},
                {"format", "R16G16B16A16_SFLOAT"},
                {"extent_scale", 1.0},
                {"usage", Json::array({"STORAGE", "SAMPLED"})}}},
              {"outputs", Json::array({"resource"})}},
             {{"id", graph_task},
              {"op", "render.compute"},
              {"parameters", graphTaskParameters(legacyRayTracing())},
              {"inputs", {{"ray_color", {{"node", graph_image}, {"port", "resource"}}}}},
              {"outputs", Json::array({"ray_color"})}}});
        document.at("rendering").at("rendering_passes").at(0).at("passes").at(0).at("input") =
            Json::array({graph_image});
        writeFile(rootPath(), document.dump(2) + "\n");
    }
    ~GraphRtProject() {
        if (std::getenv("PELICAN_RTL1B_KEEP") != nullptr) return;
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    GraphRtProject(const GraphRtProject &) = delete;
    GraphRtProject &operator=(const GraphRtProject &) = delete;
    std::filesystem::path rootPath() const { return root / root_reference; }
    std::filesystem::path layoutPath() const { return root / "passes/ray_layout.json"; }
    std::filesystem::path stderrLog(std::string_view run) const {
        return root / ("rtl1c." + std::string{run} + ".stderr.log");
    }
};

#ifdef _WIN32
std::wstring playerCommand(const std::filesystem::path &project, const std::wstring &flags) {
    const std::filesystem::path player{PELICAN_RTL1B_PLAYER};
    return L"\"" + player.wstring() + L"\" " + flags + L" --size 160x90 --project \"" + project.wstring() + L"\"";
}

// One headless player with --rpc --render-fallback. Requests are written one
// line at a time and each answer is read before the next request goes out.
class PlayerRpc {
    HANDLE stdin_write_ = nullptr;
    HANDLE stdout_read_ = nullptr;
    PROCESS_INFORMATION process_{};
    std::thread reader_;
    std::mutex mutex_;
    std::condition_variable arrived_;
    std::deque<std::string> lines_;
    bool closed_ = false;
    int next_id_ = 1;
    std::filesystem::path stderr_log_;

    void readLoop() {
        std::string pending;
        char buffer[4096];
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(stdout_read_, buffer, sizeof buffer, &read, nullptr) || read == 0) break;
            pending.append(buffer, read);
            for (auto end = pending.find('\n'); end != std::string::npos; end = pending.find('\n')) {
                auto line = pending.substr(0, end);
                pending.erase(0, end + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::scoped_lock lock{mutex_};
                lines_.push_back(std::move(line));
                arrived_.notify_all();
            }
        }
        std::scoped_lock lock{mutex_};
        closed_ = true;
        arrived_.notify_all();
    }

  public:
    std::optional<DWORD> exit_code;

    PlayerRpc(const std::filesystem::path &project, const std::filesystem::path &stderr_log)
        : stderr_log_{stderr_log} {
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE stdin_read = nullptr;
        HANDLE stdout_write = nullptr;
        REQUIRE(CreatePipe(&stdin_read, &stdin_write_, &inherit, 0));
        REQUIRE(CreatePipe(&stdout_read_, &stdout_write, &inherit, 0));
        SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0);
        HANDLE error_log = CreateFileW(stderr_log.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(error_log != INVALID_HANDLE_VALUE);
        auto command = playerCommand(project, L"--rpc --headless --render-fallback");
        STARTUPINFOW startup{};
        startup.cb = sizeof startup;
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = stdin_read;
        startup.hStdOutput = stdout_write;
        startup.hStdError = error_log;
        const auto working = std::filesystem::path{PELICAN_RTL1B_PLAYER}.parent_path().wstring();
        const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                            nullptr, working.c_str(), &startup, &process_);
        CloseHandle(stdin_read);
        CloseHandle(stdout_write);
        CloseHandle(error_log);
        REQUIRE(started);
        reader_ = std::thread{[this] { readLoop(); }};
    }

    // Ends the input and waits for the player to leave.
    void close() {
        if (stdin_write_ != nullptr) {
            CloseHandle(stdin_write_);
            stdin_write_ = nullptr;
        }
        if (process_.hProcess != nullptr && !exit_code) {
            // The player leaves when its input ends; one that does not is
            // this test's own child, stopped so the run cannot hang.
            if (WaitForSingleObject(process_.hProcess, 120000) != WAIT_OBJECT_0) {
                TerminateProcess(process_.hProcess, 1);
                WaitForSingleObject(process_.hProcess, 10000);
            }
            DWORD code = 0;
            GetExitCodeProcess(process_.hProcess, &code);
            exit_code = code;
        }
    }

    ~PlayerRpc() {
        close();
        if (process_.hProcess != nullptr) {
            CloseHandle(process_.hThread);
            CloseHandle(process_.hProcess);
        }
        if (reader_.joinable()) reader_.join();
        if (stdout_read_ != nullptr) CloseHandle(stdout_read_);
    }
    PlayerRpc(const PlayerRpc &) = delete;
    PlayerRpc &operator=(const PlayerRpc &) = delete;

    Json call(const std::string &method, const Json &params = Json::object()) {
        const int id = next_id_++;
        const auto line = Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}.dump() + "\n";
        DWORD written = 0;
        REQUIRE(WriteFile(stdin_write_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr));
        REQUIRE(written == line.size());
        std::unique_lock lock{mutex_};
        const auto deadline = std::chrono::steady_clock::now() + 180s;
        for (;;) {
            while (!lines_.empty()) {
                auto response = Json::parse(lines_.front());
                lines_.pop_front();
                if (response.value("id", -1) == id) return response;
            }
            INFO("waiting for the answer to " << method);
            if (closed_) UNSCOPED_INFO("the player's log: " << readFile(stderr_log_));
            REQUIRE_FALSE(closed_);
            REQUIRE(arrived_.wait_until(lock, deadline) != std::cv_status::timeout);
        }
    }

    Json result(const std::string &method, const Json &params = Json::object()) {
        auto response = call(method, params);
        INFO(method << " -> " << response.dump());
        REQUIRE(response.contains("result"));
        return response.at("result");
    }

    // A ticket the editor accepted (or refused at once), until it settles.
    Json settle(const Json &accepted) {
        INFO(accepted.dump());
        if (accepted.value("status", std::string{}) == "rejected") return accepted;
        REQUIRE(accepted.at("status") == "accepted");
        for (int attempt = 0; attempt < 16; ++attempt) {
            (void)result("step_frame");
            auto settled = result("get_edit_result", {{"ticket", accepted.at("ticket")}});
            if (settled.value("status", std::string{}) != "pending") return settled;
        }
        FAIL("the ticket stayed pending");
        return Json{};
    }

    Json reload() { return settle(result("reload_render_documents")); }
};

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

std::ostream &operator<<(std::ostream &out, const Rgb &rgb) {
    return out << "rgb(" << rgb.r << ", " << rgb.g << ", " << rgb.b << ")";
}

// The next frame's centre and top-left corner through the capture RPC.
std::pair<Rgb, Rgb> captureCentreAndCorner(PlayerRpc &player, const std::filesystem::path &path) {
    (void)player.result("render_frame");
    std::filesystem::remove(path);
    (void)player.result("capture", {{"path", path.generic_string()}});
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc *pixels = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
    REQUIRE(pixels != nullptr);
    const auto at = [&](int x, int y) {
        const auto *pixel = pixels + (static_cast<std::size_t>(y) * width + x) * 4;
        return Rgb{pixel[0], pixel[1], pixel[2]};
    };
    const auto result = std::pair{at(width / 2, height / 2), at(4, 4)};
    stbi_image_free(pixels);
    return result;
}

// Frames run: the evaluation index moves on.
void requireFramesRun(PlayerRpc &player) {
    const auto before = player.result("get_status").at("frame").get<std::uint64_t>();
    (void)player.result("step_frame");
    (void)player.result("step_frame");
    CHECK(player.result("get_status").at("frame").get<std::uint64_t>() > before);
}

// A reload refused by the task parser's code, with the task and the feature
// document that declares it among its fields; the message is the parser's.
Json requireTaskRefusal(PlayerRpc &player, std::string_view code, std::string_view message) {
    const auto result = player.reload();
    INFO(result.dump(2));
    REQUIRE(result.at("status") == "rejected");
    CHECK_FALSE(result.contains("restart_required"));
    const auto &error = result.at("error");
    CHECK(error.at("code") == std::string{code});
    CHECK(error.at("task") == task_name);
    CHECK(error.at("document") == feature_document);
    CHECK(error.at("message") == std::string{message});
    return error;
}
#endif

} // namespace

#ifdef _WIN32
TEST_CASE("RTL-1b a layout project whose ray tracing task is refused starts the fallback under --render-fallback, "
          "named by the task parser's code, and a reload of the fixed task brings the trace back",
          "[rtl1b][ray-layout][render-fallback][rpc]") {
    // The task leaves its one hit group out.
    auto broken = layoutRayTracing();
    broken["hit_shaders"]["primary"] = Json::object();
    RtProject project{"fallback", /*layout=*/true, broken};
    PlayerRpc player{project.root, project.stderrLog("fallback")};

    // The engine is up on the fallback, refused by the parser's name.
    {
        const auto status = player.result("get_status");
        INFO(status.dump(2));
        const auto &pipeline = status.at("render_pipeline");
        REQUIRE(pipeline.at("state") == "fallback");
        const auto &refusal = pipeline.at("refusal");
        CHECK(refusal.at("code") == "pelican.ray_tracing.hit_shader_missing@1");
        CHECK(refusal.at("task") == task_name);
        CHECK(refusal.at("ray_type") == "primary");
        CHECK(refusal.at("group") == "mesh");
        CHECK(refusal.at("message") ==
              "pelican.ray_tracing.hit_shader_missing@1: compute task 'rtl1b_trace' group='mesh' "
              "ray_type='primary' available=[]");
        // The process runs in the layout mode its root asked for.
        CHECK(status.at("ray_tracing").at("ray_layout_mode") == "layout");
    }
    requireFramesRun(player);

    // Fixed on disk and reloaded: the project's generation, in the same
    // process (the mode did not change).
    project.writeFeature(layoutRayTracing());
    {
        const auto result = player.reload();
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "committed");
        CHECK_FALSE(result.contains("restart_required"));
    }
    CHECK(player.result("get_status").at("render_pipeline").at("state") == "ready");
    requireFramesRun(player);
    const auto [centre, corner] = captureCentreAndCorner(player, project.root / "capture.png");
    INFO("centre " << centre << ", corner " << corner);
    CHECK(centre.r > centre.b + 60);
    CHECK(corner.b > corner.r + 30);
    player.close();
    CHECK(player.exit_code == 0u);
}

TEST_CASE("RTL-1b a reload that adds a ray group layout is checked first, then committed with restart_required; "
          "the engine started again runs the layout",
          "[rtl1b][ray-layout][restart][rpc]") {
    RtProject project{"restart", /*layout=*/false, legacyRayTracing()};
    {
        PlayerRpc player{project.root, project.stderrLog("legacy")};
        {
            const auto status = player.result("get_status");
            INFO(status.dump(2));
            REQUIRE(status.at("render_pipeline").at("state") == "ready");
            CHECK(status.at("ray_tracing").at("ray_layout_mode") == "none");
        }
        requireFramesRun(player);
        const auto legacy_feature = readFile(project.featurePath());

        // 1. The layout and the root naming it, the task left in the form
        // without a layout: refused by that name, not a restart. The reload
        // wrote nothing.
        project.writeLayout();
        project.writeRoot(/*layout=*/true);
        const auto layout_root = readFile(project.rootPath());
        {
            const auto error = requireTaskRefusal(
                player, "pelican.ray_tracing.legacy_syntax_with_layout@1",
                "pelican.ray_tracing.legacy_syntax_with_layout@1: compute task 'rtl1b_trace' must use "
                "traced_ray_types/misses/hit_shaders");
            CHECK(error.at("keys") == Json::array({"closesthit", "miss"}));
        }
        CHECK(readFile(project.featurePath()) == legacy_feature);
        CHECK(readFile(project.rootPath()) == layout_root);

        // 2. The layout's form, one group left out: hit_shader_missing with
        // the ray type and the group. PB-1 lists it as the feature's.
        auto missing_group = layoutRayTracing();
        missing_group["hit_shaders"]["primary"] = Json::object();
        project.writeFeature(missing_group);
        {
            const auto error = requireTaskRefusal(
                player, "pelican.ray_tracing.hit_shader_missing@1",
                "pelican.ray_tracing.hit_shader_missing@1: compute task 'rtl1b_trace' group='mesh' "
                "ray_type='primary' available=[]");
            CHECK(error.at("ray_type") == "primary");
            CHECK(error.at("group") == "mesh");
            const auto problems = player.result("get_problems").at("problems");
            INFO(problems.dump(2));
            REQUIRE(problems.size() == 1u);
            const auto &problem = problems.at(0);
            CHECK(problem.at("source") == "reload_rpc");
            CHECK(problem.at("code") == "pelican.ray_tracing.hit_shader_missing@1");
            CHECK(problem.at("document") == feature_document);
            CHECK(problem.at("document_kind") == "feature");
            CHECK(problem.at("fields").at("task") == task_name);
            CHECK(problem.at("fields").at("ray_type") == "primary");
            CHECK(problem.at("fields").at("group") == "mesh");
        }

        // 3. A miss the layout does not have: miss_shader_unknown with it.
        auto unknown_miss = layoutRayTracing();
        unknown_miss["misses"] = {{"sky", "project://shaders/layout"}, {"skyy", "project://shaders/layout"}};
        project.writeFeature(unknown_miss);
        {
            const auto error = requireTaskRefusal(
                player, "pelican.ray_tracing.miss_shader_unknown@1",
                "pelican.ray_tracing.miss_shader_unknown@1: compute task 'rtl1b_trace' miss='skyy'");
            CHECK(error.at("miss") == "skyy");
        }
        // The generation before goes on all the while.
        CHECK(player.result("get_status").at("render_pipeline").at("state") == "ready");
        requireFramesRun(player);

        // 4. The task in the layout's form: committed for the next start.
        project.writeFeature(layoutRayTracing());
        {
            const auto result = player.reload();
            INFO(result.dump(2));
            REQUIRE(result.at("status") == "committed");
            CHECK(result.at("committed") == true);
            REQUIRE(result.at("restart_required") == true);
            const auto &reason = result.at("restart_reason");
            CHECK(reason.at("code") == "ray_layout_mode_changed");
            CHECK(reason.at("from") == "none");
            CHECK(reason.at("to") == "layout");
            CHECK_THAT(reason.at("message").get<std::string>(),
                       Catch::Matchers::ContainsSubstring("ray_layout_mode_change_requires_restart"));
            CHECK(player.result("get_problems").at("problems").empty());
        }
        // Until the restart: the generation before draws, the process is
        // still without a layout, and the authoring context already names
        // the layout's ray types, groups and misses from the document.
        {
            const auto status = player.result("get_status");
            CHECK(status.at("render_pipeline").at("state") == "ready");
            CHECK(status.at("ray_tracing").at("ray_layout_mode") == "none");
        }
        requireFramesRun(player);
        {
            const auto context = player.result("get_render_authoring_context");
            const auto &ray_layout = context.at("ray_layout");
            INFO(ray_layout.dump(2));
            CHECK(ray_layout.at("mode") == "none");
            CHECK(ray_layout.at("ray_types") == Json::array({"primary"}));
            CHECK(ray_layout.at("groups") == Json::array({{{"name", "mesh"}, {"type", "triangles"}}}));
            CHECK(ray_layout.at("misses") == Json::array({"sky"}));
            CHECK(ray_layout.at("default_group") == "mesh");
            CHECK_FALSE(ray_layout.contains("error"));
        }
        // 5. The next change before a restart is committed for it again.
        auto recursion = layoutRayTracing();
        recursion["max_recursion"] = 1;
        project.writeFeature(recursion);
        {
            const auto result = player.reload();
            INFO(result.dump(2));
            REQUIRE(result.at("status") == "committed");
            CHECK(result.at("restart_required") == true);
            CHECK(result.at("restart_reason").at("code") == "ray_layout_mode_changed");
        }
        player.close();
        CHECK(player.exit_code == 0u);
    }

    // 6. Started again: the layout's mode, its generation, the trace.
    PlayerRpc player{project.root, project.stderrLog("layout")};
    {
        const auto status = player.result("get_status");
        INFO(status.dump(2));
        REQUIRE(status.at("render_pipeline").at("state") == "ready");
        CHECK(status.at("ray_tracing").at("ray_layout_mode") == "layout");
    }
    requireFramesRun(player);
    CHECK(player.result("get_frame_plan").dump().find(task_name) != std::string::npos);
    CHECK(player.result("get_render_authoring_context").at("ray_layout").at("mode") == "layout");
    const auto [centre, corner] = captureCentreAndCorner(player, project.root / "capture.png");
    INFO("centre " << centre << ", corner " << corner);
    CHECK(centre.r > centre.b + 60);
    CHECK(corner.b > corner.r + 30);
    player.close();
    CHECK(player.exit_code == 0u);
}

namespace {

// RTL-1c: the render document of a kind, as get_render_documents lists it.
Json renderDocumentOf(PlayerRpc &player, std::string_view kind) {
    const auto listed = player.result("get_render_documents");
    for (const auto &document : listed.at("documents"))
        if (document.at("kind") == std::string{kind}) return document;
    INFO(listed.dump(2));
    FAIL("no render document of kind " << kind);
    return Json{};
}

Json editBatch(PlayerRpc &player, const Json &edits) {
    const auto digest = player.result("get_ray_layout").at("source_digest").at("hex");
    return player.settle(player.result("edit_batch", {{"base_source_digest", digest}, {"edits", edits}}));
}

Json addLayoutEdit() {
    return {{"method", "edit_ray_layout"}, {"params", {{"layout", graph_layout_json}}}};
}

Json taskParametersEdit(const Json &ray_tracing) {
    return {{"method", "edit_graph_document"},
            {"params",
             {{"ops", Json::array({{{"op", "set_node_parameters"},
                                    {"node", graph_task},
                                    {"parameters", graphTaskParameters(ray_tracing)}}})}}}};
}

void requireRestart(const Json &result, std::string_view from, std::string_view to) {
    INFO(result.dump(2));
    REQUIRE(result.at("status") == "committed");
    REQUIRE(result.at("restart_required") == true);
    const auto &reason = result.at("restart_reason");
    CHECK(reason.at("code") == "ray_layout_mode_changed");
    CHECK(reason.at("from") == std::string{from});
    CHECK(reason.at("to") == std::string{to});
}

} // namespace

// RTL-1c: the paths only RTL-1a (the layout as a render document,
// edit_ray_layout) and RTL-1b (the mode decided per start, typed refusals)
// together open, from Blank with the ray tracing task a node of the root
// graph: the layout and the node's layout form in one edit_batch (committed
// for the next start), the node left as it was (refused by its code, with the
// node), what the editor reads before the restart, a material binding and the
// last object drawn in its material removed in the layout's process, and the
// commit taken back by restore_render_documents (the next start runs without
// a layout again).
TEST_CASE("RTL-1c a Blank graph node's ray tracing task takes a ray group layout in one edit_batch, binds a "
          "material after the restart, and restore_render_documents takes the layout back",
          "[rtl1c][ray-layout][restart][rpc]") {
    GraphRtProject project{"rtl1c"};
    const auto legacy_root = readFile(project.rootPath());
    const std::string layout_reference = "project://passes/ray_layout.json";
    {
        PlayerRpc player{project.root, project.stderrLog("legacy")};
        {
            const auto status = player.result("get_status");
            INFO(status.dump(2));
            REQUIRE(status.at("render_pipeline").at("state") == "ready");
            CHECK(status.at("ray_tracing").at("ray_layout_mode") == "none");
        }
        requireFramesRun(player);
        {
            // The graph node's task traces without a layout.
            const auto [centre, corner] = captureCentreAndCorner(player, project.root / "legacy.png");
            INFO("centre " << centre << ", corner " << corner);
            CHECK(centre.r > centre.b + 60);
            CHECK(corner.b > corner.r + 30);
        }
        {
            const auto read = player.result("get_ray_layout");
            INFO(read.dump(2));
            CHECK(read.at("mode") == "none");
            CHECK(read.at("layout").is_null());
        }

        // 2. The layout alone, the node left in the form without one: refused
        // by that name, with the node; nothing written.
        {
            const auto result = editBatch(player, Json::array({addLayoutEdit()}));
            INFO(result.dump(2));
            REQUIRE(result.at("status") == "rejected");
            CHECK_FALSE(result.contains("restart_required"));
            const auto &error = result.at("error");
            CHECK(error.at("code") == "pelican.ray_tracing.legacy_syntax_with_layout@1");
            CHECK(error.at("task") == graph_task);
            CHECK(error.at("node") == graph_task);
            CHECK(error.at("keys") == Json::array({"closesthit", "miss"}));
            CHECK(error.at("document") == root_reference);
            CHECK(readFile(project.rootPath()) == legacy_root);
            CHECK_FALSE(std::filesystem::exists(project.layoutPath()));
        }

        // 1. The layout made beside the root and the node's layout form, one
        // edit_batch: committed for the next start.
        {
            const auto result =
                editBatch(player, Json::array({addLayoutEdit(), taskParametersEdit(graphLayoutRayTracing())}));
            requireRestart(result, "none", "layout");
            const auto &documents = result.at("documents");
            INFO(documents.dump(2));
            CHECK(documents.contains(layout_reference));
            CHECK(documents.contains(root_reference));
            const auto root = Json::parse(readFile(project.rootPath()));
            CHECK(root.at("rendering").at("ray_group_layout") == layout_reference);
            CHECK(root.at("nodes").at(1).at("parameters").at("ray_tracing") == graphLayoutRayTracing());
            REQUIRE(std::filesystem::exists(project.layoutPath()));
            CHECK(Json::parse(readFile(project.layoutPath())) == graph_layout_json);
        }

        // 3. Before the restart: the process still runs without a layout,
        // the documents already name it, and the next edit is committed for
        // the restart again.
        {
            const auto read = player.result("get_ray_layout");
            INFO(read.dump(2));
            CHECK(read.at("mode") == "none");
            REQUIRE(read.at("layout").is_object());
            CHECK(read.at("layout").at("reference") == layout_reference);
            CHECK(read.at("layout").at("document") == graph_layout_json);
            CHECK_FALSE(read.at("layout").contains("error"));
            const auto &names = read.at("names");
            CHECK(names.at("groups") ==
                  Json::array({{{"name", "mesh"}, {"type", "triangles"}}, {{"name", "marked"}, {"type", "triangles"}}}));
            // The authoring context's names are the same reading.
            const auto context = player.result("get_render_authoring_context").at("ray_layout");
            INFO(context.dump(2));
            CHECK(context.at("mode") == "none");
            CHECK(context.at("reference") == layout_reference);
            for (const auto *key : {"ray_types", "groups", "misses", "default_group"}) {
                INFO(key);
                CHECK(context.at(key) == names.at(key));
            }
            CHECK_FALSE(context.contains("error"));
        }
        {
            auto recursion = graphLayoutRayTracing();
            recursion["max_recursion"] = 1;
            const auto result = editBatch(player, Json::array({taskParametersEdit(recursion)}));
            requireRestart(result, "none", "layout");
        }
        {
            const auto status = player.result("get_status");
            CHECK(status.at("render_pipeline").at("state") == "ready");
            CHECK(status.at("ray_tracing").at("ray_layout_mode") == "none");
        }
        requireFramesRun(player);
        player.close();
        CHECK(player.exit_code == 0u);
    }

    {
        // Started again: the layout's mode and the trace.
        PlayerRpc player{project.root, project.stderrLog("layout")};
        {
            const auto status = player.result("get_status");
            INFO(status.dump(2));
            REQUIRE(status.at("render_pipeline").at("state") == "ready");
            CHECK(status.at("ray_tracing").at("ray_layout_mode") == "layout");
        }
        requireFramesRun(player);
        CHECK(player.result("get_frame_plan").dump().find(graph_task) != std::string::npos);
        CHECK(player.result("get_ray_layout").at("mode") == "layout");
        {
            // Every row in the default group: its closest hit's orange.
            const auto [centre, corner] = captureCentreAndCorner(player, project.root / "layout.png");
            INFO("centre " << centre << ", corner " << corner);
            CHECK(centre.r > centre.b + 60);
            CHECK(corner.b > corner.r + 30);
        }

        const auto cubeGroups = [&] {
            const auto occurrences = player.result("get_ray_occurrences", {{"name", "Cube"}});
            INFO(occurrences.dump(2));
            std::vector<std::string> groups;
            for (const auto &entry : occurrences.at("entries")) {
                REQUIRE(entry.at("binding").is_object());
                CHECK(entry.at("binding").at("binding_index") == 0);
                groups.push_back(entry.at("binding").at("group_name").get<std::string>());
            }
            return groups;
        };

        // 5a. The primitive's own default material is offered by its asset
        // and index, and a binding by that selector matches the cube's rows.
        {
            const auto read = player.result("get_ray_layout");
            INFO(read.dump(2));
            CHECK(read.at("bindings").is_null());
            const Json primitive_default{{"asset", "engine://primitives/cube"}, {"source_material_index", 0}};
            const auto &materials = read.at("selectable_materials");
            CHECK(std::any_of(materials.begin(), materials.end(),
                              [&](const Json &material) { return material.at("selector") == primitive_default; }));
            const Json by_asset{{"schema", "pelican.ray_bindings"},
                                {"version", 2},
                                {"bindings", Json::array({{{"material", primitive_default},
                                                           {"group", "marked"},
                                                           {"record", Json::object()}}})}};
            const auto result = player.settle(player.result(
                "edit_ray_layout", {{"base_source_digest", read.at("source_digest").at("hex")}, {"bindings", by_asset}}));
            INFO(result.dump(2));
            REQUIRE(result.at("status") == "committed");
            requireFramesRun(player);
            CHECK(player.result("get_ray_layout").at("unresolved_bindings") == Json::array());
            const auto groups = cubeGroups();
            REQUIRE_FALSE(groups.empty());
            CHECK(std::ranges::all_of(groups, [](const std::string &group) { return group == "marked"; }));
            const auto [centre, corner] = captureCentreAndCorner(player, project.root / "primitive_default.png");
            INFO("centre " << centre << ", corner " << corner);
            CHECK(centre.g > centre.r + 60);
        }

        // 5. A material made and bound to the cube is one get_ray_layout
        // offers; a binding by its selector moves the cube's rows to marked.
        const std::string material_path = "materials/rtl1c.material.json";
        {
            const auto created = player.result(
                "create_material", {{"path", material_path},
                                    {"surface", "engine://surfaces/openpbr/opaque_single.surface"},
                                    {"name", "rtl1c_red"}});
            INFO(created.dump(2));
            const auto bound =
                player.result("bind_material", {{"model", "engine://primitives/cube"}, {"material", "rtl1c_red"}});
            INFO(bound.dump(2));
        }
        requireFramesRun(player);
        Json selector;
        {
            const auto read = player.result("get_ray_layout");
            INFO(read.dump(2));
            const auto &materials = read.at("selectable_materials");
            for (const auto &material : materials)
                if (material.at("name") == "rtl1c_red") selector = material.at("selector");
            REQUIRE(selector == Json{{"document", material_path}});
            // The cube no longer draws in its default: the binding by it
            // matches no row now, and is listed.
            CHECK(read.at("unresolved_bindings") ==
                  Json::array({{{"index", 0},
                                {"selector", {{"asset", "engine://primitives/cube"}, {"source_material_index", 0}}},
                                {"group", "marked"}}}));
        }
        const Json bindings{{"schema", "pelican.ray_bindings"},
                            {"version", 2},
                            {"bindings", Json::array({{{"material", selector}, {"group", "marked"}, {"record", Json::object()}}})}};
        {
            const auto read = player.result("get_ray_layout");
            const auto result = player.settle(player.result(
                "edit_ray_layout", {{"base_source_digest", read.at("source_digest").at("hex")},
                                    {"bindings", bindings},
                                    {"base_bindings_digest", read.at("bindings").at("digest")}}));
            INFO(result.dump(2));
            REQUIRE(result.at("status") == "committed");
            CHECK_FALSE(result.contains("restart_required"));
        }
        requireFramesRun(player);
        {
            const auto read = player.result("get_ray_layout");
            INFO(read.dump(2));
            CHECK(read.at("unresolved_bindings") == Json::array());
            const auto groups = cubeGroups();
            REQUIRE_FALSE(groups.empty());
            CHECK(std::ranges::all_of(groups, [](const std::string &group) { return group == "marked"; }));
            const auto [centre, corner] = captureCentreAndCorner(player, project.root / "marked.png");
            INFO("centre " << centre << ", corner " << corner);
            CHECK(centre.g > centre.r + 60);
            CHECK(corner.b > corner.r + 30);
        }

        // 6. The last object drawn in that material removed: the scene edit
        // goes through, and the binding no row matches is listed.
        {
            const auto session = player.result("open_editor_session", {{"display_name", "RTL-1c"}});
            const auto tree = player.result("scene_tree", Json::object());
            std::uint64_t cube = 0;
            for (const auto &object : tree.at("objects"))
                if (object.value("name", std::string{}) == "Cube")
                    cube = object.at("authoring_object_id").get<std::uint64_t>();
            REQUIRE(cube != 0);
            const auto result =
                player.settle(player.result("edit", {{"actor_id", session.at("actor_id")},
                                                     {"base_revision", tree.at("scene_revision")},
                                                     {"operations", Json::array({{{"op", "destroy"}, {"object_id", cube}}})}}));
            INFO(result.dump(2));
            REQUIRE(result.at("status") == "committed");
        }
        requireFramesRun(player);
        {
            const auto read = player.result("get_ray_layout");
            INFO(read.dump(2));
            CHECK(read.at("unresolved_bindings") ==
                  Json::array({{{"index", 0}, {"selector", selector}, {"group", "marked"}}}));
            CHECK(player.result("get_status").at("render_pipeline").at("state") == "ready");
            // No cube: the miss everywhere.
            const auto [centre, corner] = captureCentreAndCorner(player, project.root / "removed.png");
            INFO("centre " << centre << ", corner " << corner);
            CHECK(centre.b > centre.r + 30);
        }

        // 4. The layout taken back by one restore (the layout and the
        // bindings deleted, the root as it was): committed for the next
        // start, which runs without a layout.
        {
            const auto root = renderDocumentOf(player, "root");
            const auto layout = renderDocumentOf(player, "ray_group_layout");
            const auto bound = renderDocumentOf(player, "ray_bindings");
            const auto result = player.settle(player.result(
                "restore_render_documents",
                {{"documents", Json::array({{{"reference", layout.at("reference")},
                                             {"base_digest", layout.at("digest")},
                                             {"content", nullptr}},
                                            {{"reference", bound.at("reference")},
                                             {"base_digest", bound.at("digest")},
                                             {"content", nullptr}},
                                            {{"reference", root.at("reference")},
                                             {"base_digest", root.at("digest")},
                                             {"content", legacy_root}}})}}));
            requireRestart(result, "layout", "none");
            CHECK(readFile(project.rootPath()) == legacy_root);
            CHECK_FALSE(std::filesystem::exists(project.layoutPath()));
            CHECK(player.result("get_ray_layout").at("mode") == "layout");
        }
        player.close();
        CHECK(player.exit_code == 0u);
    }

    // Started again: without a layout, the graph node's task traces.
    PlayerRpc player{project.root, project.stderrLog("legacy_again")};
    {
        const auto status = player.result("get_status");
        INFO(status.dump(2));
        REQUIRE(status.at("render_pipeline").at("state") == "ready");
        CHECK(status.at("ray_tracing").at("ray_layout_mode") == "none");
    }
    requireFramesRun(player);
    const auto [centre, corner] = captureCentreAndCorner(player, project.root / "legacy_again.png");
    INFO("centre " << centre << ", corner " << corner);
    CHECK(centre.r > centre.b + 60);
    CHECK(corner.b > corner.r + 30);
    player.close();
    CHECK(player.exit_code == 0u);
}
#endif
