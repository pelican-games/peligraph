#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/renderingpass/computetaskcontainer.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/renderingpass/rendertargetcontainer.hpp"
#include "../src/core/vkcore/rendertarget.hpp"
#include "../src/core/watch/problemlist.hpp"
#include "../src/project/renderconfigdocument.hpp"
#include "../src/core/watch/reloadservice.hpp"
#include "render_graph_root_test_support.hpp"
#include "renderer_testaccess.hpp"
#include "synthetic_stereo_target.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#ifdef _WIN32
#include <process.h>
#endif

namespace Pelican {
namespace {
std::filesystem::path makeTempProjectDir() {
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_cp1_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    return root;
}
void writeTextFile(const std::filesystem::path &path, const std::string &text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out << text;
    out.close();
    if (!out) throw std::runtime_error("cannot write fixture: " + path.string());
}
nlohmann::json makeProjectConfig(const std::string &scene, const std::string &assets) {
    return {{"basic_config", {{"window_size", {{"width", 8}, {"height", 8}}},
                              {"scene_data_json", scene}, {"asset_data_json", assets}}}};
}
const char *gpuArenaFullscreenVertexShader() {
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
const char *gpuArenaCopyFragmentShader() {
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

} // namespace

TEST_CASE("CP1 compute generations dispatch 80 stereo tasks and report apply failures", "[headless][gpu][compute][cp1]") {
#if PELICAN_RUNTIME_SHADER_COMPILER && PELICAN_WITH_OPENXR
    setupLogger();
    const auto root = makeTempProjectDir();
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    } cleanup{root};
    FastModuleContainer modules;
    writeTextFile(root / "scene.json", R"({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})");
    writeTextFile(root / "assets.json", R"({"schema":"pelican.asset_data","version":2,"models":[]})");
    writeTextFile(root / "shaders/fullscreen.vert", gpuArenaFullscreenVertexShader());
    writeTextFile(root / "shaders/present.frag", gpuArenaCopyFragmentShader());
    const std::string shader = R"(#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=1, local_size_y=1, local_size_z=1) in;
void main() { pelican_store_result(ivec2(0), vec4(0.25, 0.5, 0.75, 1)); }
)";
    writeTextFile(root / "shaders/cp1.comp", shader);
    auto config = nlohmann::json::parse(R"({
        "xr":{"view_execution":"sequential"},
        "render_targets":[{"name":"cp1_image","extent_scale":1.0,"format":"R8G8B8A8_UNORM","format_class":"data","usage":["STORAGE","SAMPLED"],"layers":2}],
        "rendering_passes":[{"name":"main","passes":[{"name":"present","type":"fullscreen","input":["cp1_image"],"output":{"color":"swapchain","depth":null},"shader":{"vertex":"shaders/fullscreen","fragment":"shaders/present"}}]}],
        "compute_tasks":[]
    })");
    for (int i = 0; i < 80; ++i) {
        auto task = nlohmann::json{
            {"name", "cp1_task_" + std::to_string(i)}, {"shader", "shaders/cp1"},
            {"writes", {"cp1_image"}}, {"before", {"present"}}, {"schedule", "per_view"},
            {"dispatch", {{"groups", {1, 1, 1}}}},
            {"resource_ports", {{"result", {{"resource", "cp1_image"}, {"access", "storage"}, {"view", "per_view"}}}}}};
        if (i) task["after"] = {"cp1_task_" + std::to_string(i - 1)};
        config["compute_tasks"].push_back(std::move(task));
    }
    const auto document = TestSupport::renderGraphRootWithTaskFeature(config, root);
    writeTextFile(root / "pipeline.json", document.dump());
    auto project = makeProjectConfig("scene.json", "assets.json");
    project["schema"] = "pelican.project";
    project["version"] = 1;
    project["name"] = "cp1-compute";
    project["basic_config"]["default_scene_id"] = "default_scene";
    project["basic_config"]["rendering_config_json"] = "pipeline.json";
    project["basic_config"]["default_rendering_pass"] = "main";
    writeTextFile(root / "project.json", project.dump());
    GET_MODULE(ProjectSource).setProjectData(project.dump());
    GET_MODULE(PathResolver).setup(root, false, project.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{8, 8};
    auto &vkcore = GET_MODULE(VulkanManageCore);
    launch.xr_active = true;
    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    auto &renderer = GET_MODULE(Renderer);
    renderer.selectGraphVariant(RenderGraphVariant::xr);
    RendererTestAccess::setExecutionTracing(renderer, true);
    RenderViewFamily family{.family_id = std::string{mainRenderViewFamilyId}};
    for (int eye = 0; eye < 2; ++eye) {
        RenderViewParameters view;
        view.view = glm::mat4{1};
        view.projection = glm::mat4{1};
        view.view_id = "$xr/" + std::to_string(eye);
        family.views.push_back(view);
    }
    Test::VulkanSyntheticStereoTarget target{launch.headless_extent,
        GET_MODULE(RenderTarget).getSwapchainFormat()};
    const auto draw = [&] {
        time.advance();
        renderer.renderLogicalFrame(target, family);
        vkcore.waitIdle();
        const auto &trace = RendererTestAccess::lastExecutionTrace(renderer);
        INFO(trace.dump());
        std::map<std::string, int> dispatched;
        const auto scan = [&](const nlohmann::json &nodes) {
            for (const auto &node : nodes)
                if (node.value("kind", std::string{}) == "compute") {
                    REQUIRE(node.value("recorded", false));
                    ++dispatched[node.at("name").get<std::string>()];
                }
        };
        if (trace.contains("view_family")) scan(trace.at("view_family").at("nodes"));
        else for (const auto &view : trace.at("views")) scan(view.at("nodes"));
        REQUIRE(dispatched.size() == 80);
        for (int i = 0; i < 80; ++i)
            REQUIRE(dispatched.at("cp1_task_" + std::to_string(i) + "#xr") == 2);
    };
    draw();
    auto &compute = GET_MODULE(ComputeTaskContainer);
    for (int i = 0; i < 80; ++i) {
        const auto id = compute.getComputeTaskIdByName("cp1_task_" + std::to_string(i) + "#xr");
        const auto left = compute.boundImageViewsForTesting(id, 0, 0);
        const auto right = compute.boundImageViewsForTesting(id, 0, 1);
        REQUIRE_FALSE(left.empty());
        REQUIRE(left != right);
    }
    const auto before = GET_MODULE(FrameGraphRuntimeContainer).snapshot();
    const auto limit = vkcore.getPhysDevice().getProperties().limits.maxDescriptorSetStorageBuffers;
    REQUIRE(limit < std::numeric_limits<std::uint32_t>::max());
    const std::string invalid_shader = "#version 450\nlayout(local_size_x=1) in;\n"
        "layout(set=1,binding=0) buffer B { uint value; } data[" + std::to_string(std::uint64_t{limit} + 1) +
        "];\nvoid main(){ data[0].value=1; }\n";
    writeTextFile(root / "shaders/refused.comp", invalid_shader);
    auto broken = config;
    broken["compute_tasks"][0]["shader"] = "shaders/refused";
    const auto replace = [&](const nlohmann::json &section) {
        const auto doc = TestSupport::renderGraphRootWithTaskFeature(section, root);
        writeTextFile(root / "pipeline.json", doc.dump());
    };
    const auto reload = [&] {
        return GET_MODULE(watch::ReloadService)
            .applyRuntimeNow(watch::renderPipelineReloadParticipantName).committed;
    };
    // A real vk::OutOfPoolMemoryError must refuse the entire candidate,
    // including allocations already made before the failing task.
    {
        ScopedComputeDescriptorAllocationFailureForTesting fail{5};
        REQUIRE_FALSE(reload());
        REQUIRE(GET_MODULE(FrameGraphRuntimeContainer).snapshot() == before);
        const auto rows = GET_MODULE(ProblemList).problemsOf(renderPipelineProblemDomain);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].at("code") == "render.config_apply_failed@1");
        CHECK(rows[0].at("fields").at("reason_code") == "compute.descriptor_pool_exhausted@1");
        CHECK(rows[0].at("fields").at("reason").at("sets").get<int>() >= 160);
    }
    draw();
    const auto probe = compute.getComputeTaskIdByName("cp1_task_0#xr");
    const auto revision = compute.bindingRevisionForTesting(probe);
    {
        ScopedComputeDescriptorAllocationFailureForTesting fail{5};
        CHECK_THROWS_AS(compute.rebindRenderTargets(GET_MODULE(RenderTargetContainer)), std::runtime_error);
    }
    CHECK(compute.bindingRevisionForTesting(probe) == revision);
    draw();
    replace(broken);
    REQUIRE_FALSE(reload());
    REQUIRE(GET_MODULE(FrameGraphRuntimeContainer).snapshot() == before);
    draw();
    auto rows = GET_MODULE(ProblemList).problemsOf(renderPipelineProblemDomain);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].at("code") == "render.config_apply_failed@1");
    CHECK(rows[0].at("fields").at("reason_code") == "compute.descriptor_budget@1");
    CHECK(GET_MODULE(watch::ReloadService).statusJson().at("last_error").get<std::string>().find("compute.descriptor_budget@1") != std::string::npos);
    REQUIRE_FALSE(reload());
    CHECK(GET_MODULE(ProblemList).problemsOf(renderPipelineProblemDomain).size() == 1);
#ifdef _WIN32
    // The child starts with this same rejected document, without fallback.
    const std::string player_arg = "\"-DPLAYER=" + std::string{PELICAN_CP1_PLAYER} + "\"";
    const std::string root_arg = "\"-DPROJECT=" + root.string() + "\"";
    const std::string script = "\"" + std::string{PELICAN_TEST_SOURCE_DIR} + "/test/run_cp1_startup.cmake\"";
    const char *args[] = {"cmake", player_arg.c_str(), root_arg.c_str(), "-P", script.c_str(), nullptr};
    CHECK(_spawnv(_P_WAIT, PELICAN_CP1_CMAKE, args) == 0);
#endif
    // Editor candidates use a separate renderer entry point. Its refusal
    // must also reach Problems without committing any authored bytes.
    const auto key = GET_MODULE(PathResolver).normalizedReference("pipeline.json");
    bool source_committed = false;
    const auto authored = renderer.applyRenderPipelineAuthoringCandidate(
        RenderConfigCandidateDocumentSet{key,
            {{.reference = "pipeline.json", .normalized_reference = key,
              .path = root / "pipeline.json", .operation = RenderConfigDocumentOperation::replace,
              .expected = {RenderConfigDocumentExistence::present, renderConfigSourceDigest(document.dump())},
              .bytes = document.dump()}}},
        [&] { source_committed = true; });
    CHECK_FALSE(authored.committed);
    CHECK_FALSE(source_committed);
    CHECK(authored.error.find("compute.descriptor_budget@1") != std::string::npos);
    rows = GET_MODULE(ProblemList).problemsOf(renderPipelineProblemDomain);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].at("source").get<std::string>() == std::string{problemSourceEditRpc});
    CHECK(rows[0].at("code") == "render.config_apply_failed@1");
    REQUIRE(GET_MODULE(FrameGraphRuntimeContainer).snapshot() == before);
    draw();
    replace(config);
    REQUIRE(reload());
    CHECK(GET_MODULE(ProblemList).problemsOf(renderPipelineProblemDomain).empty());
    CHECK(GET_MODULE(watch::ReloadService).statusJson().at("last_error").is_null());
    draw();
    // Rebinding must use the same generation-sized budget as registration.
    GET_MODULE(ComputeTaskContainer).rebindRenderTargets(GET_MODULE(RenderTargetContainer));
    draw();
#else
    SKIP("CP1 stereo fixture requires runtime shader compiler and XR graph variant support");
#endif
}

} // namespace Pelican
