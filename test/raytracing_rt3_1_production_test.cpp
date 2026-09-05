#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <components/predefined.hpp>

#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/appflow/framephase.hpp"
#include "../src/core/appflow/teardown.hpp"
#include "../src/core/audio/audio.hpp"
#include "../src/core/asset/model.hpp"
#include "../src/core/communication/editorcommandservice.hpp"
#if PELICAN_WITH_RPC
#include "../src/core/communication/editorrpchandlers.hpp"
#endif
#include "../src/core/communication/editorruntimefactory.hpp"
#if PELICAN_WITH_RPC
#include "../src/core/communication/rpcserver.hpp"
#endif
#include "../src/core/container.hpp"
#include "../src/core/ecs/componentinfo.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/gamelogic/behaviorarena.hpp"
#include "../src/core/gamelogic/gamelogicreload.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/authoringsceneauthority.hpp"
#include "../src/core/loader/componentcodec.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/resolvedscene.hpp"
#include "../src/core/loader/scene.hpp"
#include "scene_load_test_support.hpp"
#include "scene_load_fault_test_support.hpp"
#include "../src/core/log.hpp"
#include "../src/core/light/lightcontainer.hpp"
#include "../src/core/material/materialcontainer.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/vertbufcontainer.hpp"
#include "../src/core/phys/physworld.hpp"
#include "../src/core/registrationownerhost.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/renderingpass/renderingpassconfigloader.hpp"
#include "../src/core/renderer/camera.hpp"
#include "../src/core/renderer/polygoninstancecontainer.hpp"
#include "../src/core/renderer/proceduralgeometrycontainer.hpp"
#include "../src/core/shader/raygrouplayout.hpp"
#include "../src/core/shader/pipelinefactory.hpp"
#include "../src/core/terminalshutdown.hpp"
#include "../src/core/vkcore/accelerationstructure.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/core/watch/reloadservice.hpp"
#include "../src/core/userpublic/details/component/registerer.hpp"
#include "../src/core/userpublic/details/ecs/componentdeclare.hpp"
#include "../src/core/userpublic/deterministicrng.hpp"
#include "../src/core/userpublic/pelican_core.hpp"
#include "gltf_fragment_fixture.hpp"
#include "morph_fixture.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <picosha2.h>
#include <sstream>
#include <string>
#include <vector>
#include "authoringscenetestsupport.hpp"
#include "fixtures/ray_namespace_component/ray_instance_component.hpp"
#include "raytracing_rt3_1_fixture.hpp"
#include "vulkan_test_support.hpp"
#include "fixtures/wp380_scene_transaction/api.hpp"
#include "fixtures/wp380b_effect/api.hpp"

#ifdef _WIN32
#include <Windows.h>
#endif

namespace Pelican {
namespace {

using Catch::Matchers::ContainsSubstring;
using Json = nlohmann::json;
using RayTracingRt31Fixture::accelerationDeclaration;
using RayTracingRt31Fixture::layoutRoot;

std::string_view wp380MutationId(std::string_view owner,
                                 std::string_view field);
void requireWp380CommittedPhaseTrace(const SceneLoadCommitted &committed);

class Wp374aProjectRoot {
  public:
    std::filesystem::path path;

    explicit Wp374aProjectRoot(std::string_view label) {
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path = std::filesystem::temp_directory_path() /
               ("pelican_wp374a_" + std::string{label} + "_" +
                std::to_string(stamp));
        std::filesystem::create_directories(path);
    }

    ~Wp374aProjectRoot() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void writeWp374aText(const std::filesystem::path &path,
                     std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) {
        throw std::runtime_error("failed to create WP374a fixture: " +
                                 path.string());
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("failed to write WP374a fixture: " +
                                 path.string());
    }
}

Json wp375NoLayoutCompilerOffRenderingConfig() {
    // The hybrid_v1 preset deliberately injects a global shader define. A
    // compiler-OFF build cannot apply that define to its embedded SPIR-V.
    // These WP375 edits do not render a frame, so use the smallest production
    // graph that resolves directly to embedded shaders and generates no
    // shader include document.
    return Json{
        {"rendering_passes",
         Json::array(
             {{{"name", "main_render"},
               {"passes",
                Json::array(
                    {{{"name", "present"},
                      {"type", "fullscreen"},
                      {"output",
                       {{"color", Json::array({"swapchain"})},
                        {"depth", nullptr}}},
                      {"shader",
                       {{"vertex", "engine://fullscreen"},
                        {"fragment", "engine://shader_lab_hello"}}}}})}}})},
    };
}

std::string readWp374aText(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error("failed to read WP374a fixture: " +
                                 path.string());
    }
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

Json wp374aLayout(std::uint32_t version, bool with_as) {
    auto layout = layoutRoot(version);
    if (with_as) {
        auto declaration = accelerationDeclaration();
        declaration["rules"] = Json::array();
        layout["acceleration_structure"] = std::move(declaration);
    }
    return layout;
}

void writeWp374aProject(const std::filesystem::path &root,
                        std::optional<std::uint32_t> layout_version,
                        bool with_as, bool with_ray_instance,
                        bool renderable,
                        std::optional<Json> authored_ray_instance =
                            std::nullopt,
                        bool material_alpha_profile = false) {
    Json components = Json::array();
    if (renderable) {
        components.push_back({{"name", "transform"},
                              {"pos", {0.0, 0.0, 0.0}},
                              {"rotation", {0.0, 0.0, 0.0, 1.0}},
                              {"scale", {1.0, 1.0, 1.0}}});
        components.push_back(
            {{"name", "simplemodelview"}, {"model", "chair"}});
    }
    if (with_ray_instance) {
        components.push_back(authored_ray_instance.value_or(
            Json{{"name", "pelican_ray_instance"}, {"mask", 9}}));
    }
    Json objects = Json::array();
    if (!components.empty()) {
        objects.push_back(
            {{"name", "Chair"}, {"components", std::move(components)}});
    }
    writeWp374aText(
        root / "scene.json",
        Json{{"schema", "pelican.scene"},
             {"version", 1},
             {"scenes", {{"main", {{"objects", std::move(objects)}}}}}}
            .dump(2));

    Json models = Json::array();
    if (renderable) {
        std::filesystem::create_directories(root / "models");
        TestGltfFragmentFixture::writeGlb(root / "models" / "chair.glb");
        models.push_back({{"name", "chair"},
                          {"path", "models/chair.glb"}});
    }
    writeWp374aText(
        root / "assets.json",
        Json{{"schema", "pelican.asset_data"},
             {"version", 1},
             {"models", std::move(models)},
             {"textures", Json::array()}}
            .dump(2));

    Json rendering;
#if PELICAN_RUNTIME_SHADER_COMPILER
    rendering = Json{
        {"pipeline",
         {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
    };
#else
    rendering = layout_version
                    ? Json{{"pipeline",
                            {{"preset",
                              "engine://render_pipelines/hybrid_v1.json"}}}}
                    : wp375NoLayoutCompilerOffRenderingConfig();
#endif
    if (layout_version) {
        rendering["ray_group_layout"] = "ray_layout.json";
        auto layout = wp374aLayout(*layout_version, with_as);
        if (material_alpha_profile) {
            layout["portable_profile"] = "material_alpha_v1";
            layout["engine_record"] = {
                "material_index", "geometry_index"};
            RayTracingRt31Fixture::writeWp366RaygenAndMissShaders(
                root, writeWp374aText, false, true);
            RayTracingRt31Fixture::writeWp366ClosestHitShaders(
                root, writeWp374aText, false, true, false);
            const Json feature{
                {"schema", "pelican.render_feature"},
                {"version", 1},
                {"name", "wp377_profile_owner"},
                {"runtime_shader_compiler", "required"},
                {"required_capabilities",
                 Json::array({
                     "pelican.vulkan.ray_tracing_pipeline@1"})},
                {"render_targets",
                 Json::array({
                     {{"name", "wp366_a"},
                      {"extent_scale", 1.0},
                      {"format", "R32G32B32A32_UINT"},
                      {"format_class", "data"},
                      {"role", "data"},
                      {"usage", {"STORAGE"}}},
                 })},
                {"compute_tasks",
                 Json::array({
                     RayTracingRt31Fixture::wp366TypedRayTracingTask(
                         "wp377_profile_owner", "wp366_a",
                         "project://shaders/wp366_a", false),
                 })},
            };
            writeWp374aText(
                root / "features" / "wp377_profile_owner.json",
                feature.dump(2));
            rendering["features"] = Json::array({
                "project://features/wp377_profile_owner.json"});
        }
        writeWp374aText(root / "ray_layout.json", layout.dump(2));
    }
    if (renderable) {
        rendering["target_planning"] = {
            {"graphs",
             {{"main_render",
               {{"required_capabilities",
                 Json::array({"pelican.vulkan.ray_query@1"})}}}}},
        };
    }
    writeWp374aText(root / "hybrid.json", rendering.dump(2));
}

void writeWp386aProceduralProject(const std::filesystem::path &root) {
    writeWp374aProject(root, 3, true, false, false);
    writeWp374aText(root / "scene.json", Json{
        {"schema", "pelican.scene"}, {"version", 1},
        {"scenes", {{"main", {{"objects", Json::array({{
            {"name", "Sphere"},
            {"components", Json::array({
                Json{{"name", "transform"}, {"pos", {0, 0, 0}},
                     {"rotation", {0, 0, 0, 1}}, {"scale", {1, 1, 1}}},
                Json{{"name", "pelican_procedural_geometry"},
                     {"asset", "sphere_asset"}}
            })}
        }})}}}}}
    }.dump(2));
    writeWp374aText(root / "assets.json", Json{
        {"schema", "pelican.asset_data"}, {"version", 2},
        {"models", Json::array()}, {"textures", Json::array()},
        {"materials", Json::array()},
        {"procedural_geometry", Json::array({{
            {"name", "sphere_asset"}, {"kind", "aabb_v1"},
            {"aabb", {{"min", {-16.0, -16.0, 1.0}},
                      {"max", {16.0, 16.0, 2.0}}}},
            {"record", {{"radius", 1.0}}}
        }})}
    }.dump(2));
    writeWp374aText(root / "ray_layout.json", Json{
        {"schema", "pelican.ray_group_layout"}, {"version", 3},
        {"ray_types", {"primary"}}, {"default_group", "sphere"},
        {"groups", Json::array({{{"name", "sphere"},
                                  {"type", "procedural"}}})},
        {"misses", Json::array({{{"name", "sky"}}})},
        {"engine_record", {"material_index", "geometry_index",
                            "geometry_kind"}},
        {"record", Json::array({{{"name", "radius"}, {"type", "f32"}}})},
        {"acceleration_structure", {
            {"defaults", {{"mask", 255},
                           {"instance_flags", Json::array()},
                           {"geometry_flags", Json::array()}}},
            {"procedural_defaults", {{"geometry_flags", {"opaque"}}}},
            {"rules", Json::array()}}}
    }.dump(2));
    writeWp374aText(root / "ray_bindings.json", R"json({
      "schema":"pelican.ray_bindings","version":2,
      "bindings":[{"procedural":"sphere_asset","group":"sphere"}]
    })json");
    writeWp374aText(root / "shaders" / "wp386.rgen",
        RayTracingRt31Fixture::wp366RaygenSource(
            "0u", "0u", "pelican_resource_wp386_out"));
    writeWp374aText(root / "shaders" / "wp386.rmiss",
        RayTracingRt31Fixture::wp366MissSource("0x386au", "0u"));
    writeWp374aText(root / "shaders" / "wp386.rchit", R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
struct Payload { uint marker; uint q; uint flags; uint tint; };
layout(location=0) rayPayloadInEXT Payload payload;
hitAttributeEXT vec2 hit_attribute;
layout(shaderRecordEXT, std430) buffer RayRecord {
    uint material_index;
    uint geometry_index;
    uint geometry_kind;
    float radius;
} ray_record;
void main() { payload.marker = 0xdead386au; }
)glsl");
    const Json feature{
        {"schema", "pelican.render_feature"}, {"version", 1},
        {"name", "wp386_procedural"},
        {"runtime_shader_compiler", "required"},
        {"required_capabilities", Json::array({
             "pelican.vulkan.ray_tracing_pipeline@1"})},
        {"render_targets", Json::array({{
             {"name", "wp386_out"}, {"extent_scale", 1.0},
             {"format", "R32G32B32A32_UINT"}, {"format_class", "data"},
             {"role", "data"}, {"usage", {"STORAGE", "TRANSFER_SRC"}}
        }})},
        {"compute_tasks", Json::array({{
             {"name", "wp386_trace"},
             {"ray_tracing", {
                 {"raygen", "project://shaders/wp386"},
                 {"traced_ray_types", {"primary"}},
                 {"misses", {{"sky", "project://shaders/wp386"}}},
                 {"hit_shaders", {{"primary", {{"sphere", {
                     {"closesthit", "project://shaders/wp386"},
                     {"intersection", "engine://procedural_no_report"}
                 }}}}}}
             }},
             {"writes", {"wp386_out"}},
             {"resource_ports", {{"wp386_out", {{"resource", "wp386_out"},
                                                   {"access", "storage"}}}}},
             {"dispatch", {{"rays_from", {{"port", "wp386_out"}}}}}
        }})}
    };
    writeWp374aText(root / "features" / "wp386_procedural.json",
                    feature.dump(2));
    writeWp374aText(root / "hybrid.json", Json{
        {"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
        {"features", {"project://features/wp386_procedural.json"}},
        {"ray_group_layout", "ray_layout.json"},
        {"ray_bindings", "ray_bindings.json"},
        {"target_planning", {{"graphs", {{"main_render", {
            {"required_capabilities", Json::array({
                 "pelican.vulkan.ray_query@1"})}}}}}}}
    }.dump(2));
}

std::shared_ptr<const RayTracingProjectConfig>
configureWp374aRuntime(
    const std::filesystem::path &root,
    std::optional<std::uint64_t> deterministic_seed = std::nullopt,
    std::optional<std::string> project_json = std::nullopt,
    std::optional<std::filesystem::path> user_dir = std::nullopt) {
    if (project_json) {
        GET_MODULE(PathResolver).setup(root, false, *project_json,
                                      std::move(user_dir));
    } else {
        GET_MODULE(PathResolver).setup(root, false);
    }
    Json basic_config{
        {"window_size", {{"width", 32}, {"height", 32}}},
        {"scene_data_json", "scene.json"},
        {"asset_data_json", "assets.json"},
        {"default_scene_id", "main"},
        {"rendering_config_json", "hybrid.json"},
        {"default_rendering_pass", "main_render"},
    };
    if (deterministic_seed) {
        basic_config["seed"] = *deterministic_seed;
    }
    const auto prefab_path = root / "prefabs" / "wp375a_unit.json";
    if (std::filesystem::exists(prefab_path)) {
        GET_MODULE(ProjectSource).setProjectData(
            Json{{"schema", "pelican.project"},
                 {"version", 1},
                 {"name", "wp375a-production"},
                 {"engine_min_version", "0.1.0"},
                 {"prefabs",
                  Json::array({{{"name", "wp375a_unit"},
                                {"path", "prefabs/wp375a_unit.json"}}})},
                 {"basic_config", basic_config}}
                .dump());
    } else {
        GET_MODULE(ProjectSource).setSourceByData(
            Json{{"basic_config", basic_config}}.dump());
    }
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{32, 32};
    launch.headless_frames = 1;
    GET_MODULE(EngineTime).setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);

    auto &config = GET_MODULE(ProjectBasicConfig);
    auto &paths = GET_MODULE(PathResolver);
    const auto rendering = loadRenderingPassConfigJsonFromString(
        config.renderingConfigJson(), "WP374a ProjectBasicConfig preflight");
    const auto project = loadRayTracingProjectConfig(
        rendering,
        [&paths](std::string_view reference) {
            return paths.loadText(reference);
        },
        PELICAN_RUNTIME_SHADER_COMPILER != 0);
    GET_MODULE(RayLayoutRuntimeState)
        .configure(rayLayoutSemanticMode(project.get()));
    return project;
}

void loadWp374aScene(
    const std::filesystem::path &root,
    std::optional<std::filesystem::path> game_logic_dll = std::nullopt,
    std::string_view scene_id = "main",
    std::optional<std::uint64_t> deterministic_seed = std::nullopt,
    std::optional<std::string> project_json = std::nullopt,
    std::optional<std::filesystem::path> user_dir = std::nullopt) {
    (void)configureWp374aRuntime(root, deterministic_seed,
                                std::move(project_json),
                                std::move(user_dir));
    const bool has_game_logic_dll = game_logic_dll.has_value();
    if (has_game_logic_dll) {
        GET_MODULE(EngineLaunchConfig).game_logic_dll =
            std::move(game_logic_dll);
    }
    // Acquire the device only after the fixture project root is configured;
    // acquiring it first makes the renderer read the default project layout.
    TestSupport::requireVulkanDevice("WP374a production acceptance");
    GET_MODULE(ECSPredefinedRegistration).reg();
    if (has_game_logic_dll && !initializeConfiguredGameLogic()) {
        throw std::runtime_error(
            "WP375 production behavior fixture failed to load: " +
            configuredGameLogicStatus().last_error);
    }
    test_support::loadScene(GET_MODULE(SceneLoader), std::string{scene_id});
}

void loadWp375aShippingExample(
    const std::filesystem::path &game_logic_dll) {
#ifdef PELICAN_TEST_SOURCE_DIR
    const auto root =
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" /
        "example";
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setProjectData(
        readWp374aText(root / "project.json"));
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{32, 32};
    launch.headless_frames = 1;
    launch.game_logic_dll = game_logic_dll;
    GET_MODULE(EngineTime).setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);

    auto &config = GET_MODULE(ProjectBasicConfig);
    auto &paths = GET_MODULE(PathResolver);
    const auto rendering = loadRenderingPassConfigJsonFromString(
        config.renderingConfigJson(), "WP375a shipping example");
    const auto project = loadRayTracingProjectConfig(
        rendering,
        [&paths](std::string_view reference) {
            return paths.loadText(reference);
        },
        PELICAN_RUNTIME_SHADER_COMPILER != 0);
    GET_MODULE(RayLayoutRuntimeState)
        .configure(rayLayoutSemanticMode(project.get()));
    TestSupport::requireVulkanDevice("WP375a shipping example");
    GET_MODULE(ECSPredefinedRegistration).reg();
    if (!initializeConfiguredGameLogic()) {
        throw std::runtime_error(
            "WP375a shipping example behavior fixture failed to load: " +
            configuredGameLogicStatus().last_error);
    }
    test_support::loadScene(GET_MODULE(SceneLoader), "default_scene");
#else
    (void)game_logic_dll;
    throw std::runtime_error("PELICAN_TEST_SOURCE_DIR is unavailable");
#endif
}

#if PELICAN_WITH_RPC
Json runWp374aRpcResponse(EditorCommandService &service, std::uint64_t id,
                          std::string_view method, Json params) {
    const auto request =
        Json{{"jsonrpc", "2.0"},
             {"id", id},
             {"method", method},
             {"params", std::move(params)}}
            .dump() +
        "\n";
    std::istringstream input{request};
    std::ostringstream output;
    EditorCommandRpcAdapter protocol_adapter{service};
    RpcServer server{input, output};
    configureEditorRpcHandlers(
        server, protocol_adapter,
        EditorRpcHandlerHooks{.snapshot_imported = [] {},
                              .save_busy = [] { return false; }});
    server.run();
    const auto response_bytes = output.str();
    if (response_bytes.empty() || response_bytes.back() != '\n') {
        throw std::runtime_error(
            "WP374a RpcServer::run did not emit one newline-terminated response");
    }
    const auto response = Json::parse(response_bytes);
    if (response.value("id", std::uint64_t{}) != id) {
        throw std::runtime_error("WP374a RPC failure: " + response.dump());
    }
    return response;
}

Json runWp374aRpcBytes(EditorCommandService &service, std::uint64_t id,
                       std::string_view method, Json params) {
    const auto response = runWp374aRpcResponse(
        service, id, method, std::move(params));
    if (!response.contains("result")) {
        throw std::runtime_error("WP374a RPC failure: " + response.dump());
    }
    return response.at("result");
}

Json finishWp374aEdit(EditorCommandService &service, std::uint64_t &rpc_id,
                      std::uint64_t actor, std::uint64_t base_revision,
                      Json operations) {
    const auto accepted = runWp374aRpcBytes(
        service, rpc_id++, "edit",
        {{"actor_id", actor},
         {"base_revision", base_revision},
         {"operations", std::move(operations)},
         {"coalesce_key", "wp374a-production-route"}});
    REQUIRE(accepted.at("status") == "accepted");
    invokeEditorCommitQueueHook();
    const auto result = runWp374aRpcBytes(
        service, rpc_id++, "get_edit_result",
        {{"ticket", accepted.at("ticket")}});
    INFO("WP374a edit result: " << result.dump());
    return result;
}

Json commitWp374aEdit(EditorCommandService &service, std::uint64_t &rpc_id,
                      std::uint64_t actor, std::uint64_t base_revision,
                      Json operations) {
    const auto result = finishWp374aEdit(service, rpc_id, actor, base_revision,
                                         std::move(operations));
    REQUIRE(result.at("status") == "committed");
    return result;
}
#endif

RaySceneRowSnapshot renderWp374aProductionRows() {
    GET_MODULE(ECSCore).update();
    GET_MODULE(ECSCore).update();
    GET_MODULE(Renderer).render();
    GET_MODULE(VulkanManageCore).waitIdle();
    const auto runtime = GET_MODULE(FrameGraphRuntimeContainer).snapshot();
    if (runtime == nullptr || runtime->gpu_arena == nullptr) {
        throw std::runtime_error("WP374a production GPU owner is absent");
    }
    const auto owner = std::ranges::find_if(
        runtime->gpu_arena->scopes,
        [](const auto &scope) { return scope.ray_tracing_project != nullptr; });
    if (owner == runtime->gpu_arena->scopes.end() ||
        owner->acceleration_structures == nullptr) {
        throw std::runtime_error("WP374a production AS owner is absent");
    }
    const auto *rows =
        owner->acceleration_structures->activeRaySceneRowsForTesting();
    if (rows == nullptr) {
        throw std::runtime_error("WP374a committed TLAS row plan is absent");
    }
    return *rows;
}

TEST_CASE(
    "WP386a production render carries one procedural generation through SBT and TLAS while no-report stays invisible",
    "[WP386a][ray-tracing][rt3-2a][production][gpu]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    Wp374aProjectRoot project{"wp386a_procedural"};
    writeWp386aProceduralProject(project.path);
    auto scene = Json::parse(readWp374aText(project.path / "scene.json"));
    auto &objects = scene["scenes"]["main"]["objects"];
    auto second = objects.front();
    second["name"] = "SphereSecond";
    objects.push_back(std::move(second));
    writeWp374aText(project.path / "scene.json", scene.dump());
    FastModuleContainer modules;
    try {
        loadWp374aScene(project.path);
        auto &vkcore = GET_MODULE(VulkanManageCore);
        if (!vkcore.getRuntimeCapabilities().ray_tracing_pipeline) {
            SKIP("pelican.vulkan.ray_tracing_pipeline@1 is unavailable");
        }
        std::vector<RayTracingSbtWriteObservation> writes;
        const ScopedRayTracingSbtWriteObserver observer{
            [&](const auto &write) { writes.push_back(write); }};
        const auto rows = renderWp374aProductionRows();
        REQUIRE(rows.rows.size() == 2);
        CHECK(rows.rows[0].scene_identity != rows.rows[1].scene_identity);
        CHECK(rows.rows[0].geometry_kind == RayGeometryKind::aabbs);
        CHECK(rows.rows[0].material_index ==
              rayMissingTextureIndex);
        REQUIRE_FALSE(writes.empty());
        CHECK(writes.front().record_count == 2);

        const auto runtime = GET_MODULE(FrameGraphRuntimeContainer).snapshot();
        REQUIRE(runtime != nullptr);
        REQUIRE(runtime->gpu_arena != nullptr);
        const auto owner = std::ranges::find_if(
            runtime->gpu_arena->scopes,
            [](const auto &scope) {
                return scope.ray_tracing_project != nullptr;
            });
        REQUIRE(owner != runtime->gpu_arena->scopes.end());
        REQUIRE(owner->acceleration_structures != nullptr);
        const auto *root = owner->acceleration_structures
                               ->activeGenerationRootForTesting();
        REQUIRE(root != nullptr);
        CHECK(root->generation == rows.generation);
        REQUIRE(root->sbt != nullptr);
        CHECK(root->sbt->generation == root->generation);
        CHECK(root->tlas_plan_generation == root->generation);
        REQUIRE(root->tlas_plan.size() == 2);
        REQUIRE(root->inventory != nullptr);
        // Feed the scene loader's two real occurrences back through the AS
        // row resolver with reversed publication order and equivalent zeros.
        // Object identity must preserve both rows; float spelling must not
        // change their matching keys or canonical order.
        auto reordered = root->inventory->instances;
        REQUIRE(reordered.size() == 2);
        REQUIRE(reordered[0].authoring_object_id !=
                reordered[1].authoring_object_id);
        const auto original_rows = resolveRayTracingSceneRows(
            reordered, *owner->ray_tracing_project);
        std::reverse(reordered.begin(), reordered.end());
        for (auto &instance : reordered) {
            for (glm::length_t column = 0; column < 4; ++column) {
                for (glm::length_t row = 0; row < 4; ++row) {
                    if (instance.world_transform[column][row] == 0.0F)
                        instance.world_transform[column][row] = -0.0F;
                }
            }
        }
        const auto reordered_rows = resolveRayTracingSceneRows(
            reordered, *owner->ray_tracing_project);
        REQUIRE(reordered_rows.rows.size() == 2);
        CHECK(reordered_rows.identity_digest_sha256 ==
              original_rows.identity_digest_sha256);
        for (std::size_t index = 0; index < 2; ++index) {
            CHECK(reordered_rows.rows[index].geometry_match_identity ==
                  original_rows.rows[index].geometry_match_identity);
            CHECK(reordered_rows.rows[index].scene_identity ==
                  original_rows.rows[index].scene_identity);
        }

        const auto output = GET_MODULE(Renderer).readRenderTargetForTesting(
            "wp386_out", ImageSubresourceRange{});
        REQUIRE(output.bytes.size() >= sizeof(std::uint32_t));
        std::uint32_t marker = 0;
        std::memcpy(&marker, output.bytes.data(), sizeof(marker));
        CHECK(marker == 0x386au);
        CHECK(marker != 0xdead386au);
    } catch (const std::exception &error) {
        TestSupport::skipIfVulkanDeviceUnavailable(
            error, "Vulkan WP386a procedural fixture unavailable");
        throw;
    }
#else
    SUCCEED("WP386a GPU acceptance is compiler-ON only");
#endif
}

TEST_CASE(
    "WP386a production loader rejects each independently downgraded procedural document",
    "[WP386a][ray-tracing][rt3-2a][production][version][gpu]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    struct Downgrade {
        std::string_view file;
        std::string_view diagnostic;
    };
    constexpr std::array cases{
        Downgrade{"assets.json",
                  "pelican.asset_data_procedural_requires_v2@1"},
        Downgrade{"ray_bindings.json",
                  "pelican.ray_binding_selector_exactly_one@1"},
        Downgrade{"ray_layout.json",
                  "pelican.ray_group_type_requires_layout_v3@1"},
    };
    for (const auto &entry : cases) {
        DYNAMIC_SECTION(entry.file) {
            setupLogger();
            Wp374aProjectRoot project{"wp386a_version_downgrade"};
            writeWp386aProceduralProject(project.path);
            const auto path = project.path / entry.file;
            auto document = Json::parse(readWp374aText(path));
            document["version"] = entry.file == "ray_layout.json" ? 2 : 1;
            writeWp374aText(path, document.dump(2));
            FastModuleContainer modules;
            CHECK_THROWS_WITH(
                loadWp374aScene(project.path),
                ContainsSubstring(std::string{entry.diagnostic}));
        }
    }
    {
        setupLogger();
        Wp374aProjectRoot project{"wp386a_missing_layout_reject"};
        writeWp386aProceduralProject(project.path);
        auto rendering = Json::parse(readWp374aText(project.path / "hybrid.json"));
        rendering.erase("ray_group_layout");
        rendering.erase("ray_bindings");
        writeWp374aText(project.path / "hybrid.json", rendering.dump());
        FastModuleContainer modules;
        CHECK_THROWS_WITH(loadWp374aScene(project.path),
            ContainsSubstring("pelican.ray_group_type_requires_layout_v3@1"));
    }
    {
        setupLogger();
        Wp374aProjectRoot project{"wp386a_task_type_reject"};
        writeWp386aProceduralProject(project.path);
        const auto path =
            project.path / "features" / "wp386_procedural.json";
        auto feature = Json::parse(readWp374aText(path));
        feature["compute_tasks"][0]["ray_tracing"]["hit_shaders"]
               ["primary"]["sphere"]["type"] = "procedural";
        writeWp374aText(path, feature.dump(2));
        FastModuleContainer modules;
        CHECK_THROWS_WITH(
            (loadWp374aScene(project.path), renderWp374aProductionRows()),
            ContainsSubstring("pelican.ray_group_type_declared_in_task@1"));
    }
#else
    SUCCEED("WP386a production version acceptance is compiler-ON only");
#endif
}

TEST_CASE(
    "WP386a procedural participant exposes four independent manifest mutations and exact success trace",
    "[WP386a][transaction][manifest][mutation][gpu]") {
    enum class Field {
        aabb_buffer,
        allocation_generation,
        instance_snapshot,
        retire_queue,
    };
    struct MutationCase {
        std::string_view field;
        SceneLoadFaultPhase phase;
        Field value;
    };
    constexpr std::array cases{
        MutationCase{"aabb_buffer", SceneLoadFaultPhase::publish,
                     Field::aabb_buffer},
        MutationCase{"allocation_generation", SceneLoadFaultPhase::publish,
                     Field::allocation_generation},
        MutationCase{"instance_snapshot", SceneLoadFaultPhase::publish,
                     Field::instance_snapshot},
        MutationCase{"retire_queue", SceneLoadFaultPhase::retire,
                     Field::retire_queue},
    };
    for (const auto &entry : cases) {
        DYNAMIC_SECTION(entry.field) {
            setupLogger();
            Wp374aProjectRoot project{"wp386a_participant_mutation"};
            writeWp386aProceduralProject(project.path);
            FastModuleContainer modules;
            (void)configureWp374aRuntime(project.path);
            TestSupport::requireVulkanDevice(
                "WP386a procedural participant acceptance");
            GET_MODULE(ECSPredefinedRegistration).reg();
            auto &loader = GET_MODULE(SceneLoader);
            test_support::ManifestSceneLoadFault injector{
                wp380MutationId("procedural", entry.field)};
            injector.setEnabled(true);
            test_support::setSceneLoadFaultInjectorForTesting(loader,
                                                               &injector);
            const auto result = loader.load(SceneLoadRequest::borrowed(
                "main", GET_MODULE(ProjectBasicConfig).sceneProjectionState(),
                SceneLoadRoot::startup));
            test_support::setSceneLoadFaultInjectorForTesting(loader,
                                                               nullptr);
            const auto *committed = std::get_if<SceneLoadCommitted>(&result);
            REQUIRE(committed != nullptr);
            requireWp380CommittedPhaseTrace(*committed);
            auto &procedural = GET_MODULE(ProceduralGeometryContainer);
            if (entry.value == Field::aabb_buffer) {
                const auto instances = procedural.proceduralRayInstances();
                REQUIRE(instances.size() == 1);
                CHECK(instances[0].aabb_buffer_address == 0);
                CHECK_FALSE(instances[0].geometry_lease);
            } else if (entry.value == Field::allocation_generation) {
                CHECK(procedural.allocationGeneration() == 0);
            } else if (entry.value == Field::instance_snapshot) {
                CHECK(procedural.instanceCount() == 0);
            } else {
                CHECK(procedural.retirementCount() == 0);
            }
            REQUIRE(injector.saw(entry.phase, "procedural"));
            GET_MODULE(VulkanManageCore).waitIdle();
        }
    }
}

TEST_CASE(
    "WP383 material-less glTF keeps source index absent through the production AS route",
    "[WP383][ray-tracing][rt3-1][scene-loader][production][mutation][gpu]") {
    setupLogger();
#if PELICAN_RUNTIME_SHADER_COMPILER
    // This fixture has geometry but no glTF materials.  It must therefore
    // retain no source material index all the way from GltfLoader through
    // PolygonInstanceContainer to the AS resolver; an AS declaration with a
    // rule makes the resulting Unavailable identity observable.
    Wp374aProjectRoot project{"wp383_material_less"};
    writeWp374aProject(project.path, 2, true, true, true);
    TestGltfFragmentFixture::writeGlb(project.path / "models" / "chair.glb",
                                      false, true, false, true);
    auto layout = wp374aLayout(2, true);
    layout["acceleration_structure"]["rules"] =
        accelerationDeclaration().at("rules");
    writeWp374aText(project.path / "ray_layout.json", layout.dump(2));

    FastModuleContainer modules;
    loadWp374aScene(project.path);
    GET_MODULE(ECSCore).update();
    const auto snapshots =
        GET_MODULE(PolygonInstanceContainer).rayQueryGeometryInstances();
    REQUIRE_FALSE(snapshots.empty());
    for (const auto &snapshot : snapshots) {
        CHECK(snapshot.material_document_reference.empty());
        CHECK_FALSE(snapshot.source_material_index.has_value());
    }
    const auto classification = classifyRayQueryStaticGeometry(snapshots);
    REQUIRE_FALSE(classification.static_instance_indices.empty());

    // Drive the same production AS snapshot lowering used by the renderer.
    // It performs the static classification and constructs RaySceneRowInput
    // from the PIC snapshot before calling resolveRaySceneRows.
    auto &config = GET_MODULE(ProjectBasicConfig);
    auto &paths = GET_MODULE(PathResolver);
    const auto rendering = loadRenderingPassConfigJsonFromString(
        config.renderingConfigJson(), "WP383 material-less AS snapshot");
    const auto ray_project = loadRayTracingProjectConfig(
        rendering,
        [&paths](std::string_view reference) {
            return paths.loadText(reference);
        },
        PELICAN_RUNTIME_SHADER_COMPILER != 0);
    CHECK_THROWS_WITH(
        resolveRayTracingSceneRows(snapshots, *ray_project),
        ContainsSubstring(
            "pelican.ray_as_row_material_identity_unavailable@1"));
    GET_MODULE(VulkanManageCore).waitIdle();
#else
    SUCCEED("WP383 production AS route requires the runtime shader compiler");
#endif
}

void requireWp374aTlasValues(const RaySceneRowSnapshot &rows,
                             std::uint64_t authoring_object_id,
                             std::uint8_t mask, std::uint32_t flags,
                             std::uint32_t custom_index) {
    const auto matching = std::ranges::count(
        rows.rows, authoring_object_id,
        &RaySceneRow::authoring_object_id);
    REQUIRE(matching > 0);
    for (const auto &row : rows.rows) {
        if (row.authoring_object_id != authoring_object_id) continue;
        CHECK(row.instance_mask == mask);
        CHECK(row.instance_flags == flags);
        CHECK(row.instance_custom_index == custom_index);
    }
}

std::vector<std::array<std::uint32_t, 4>> wp379ResolvedTlasRowValues(
    const RaySceneRowSnapshot &rows, std::uint64_t authoring_object_id) {
    std::vector<std::array<std::uint32_t, 4>> values;
    for (const auto &row : rows.rows) {
        if (row.authoring_object_id != authoring_object_id) continue;
        values.push_back({row.instance_mask, row.instance_flags,
                          row.instance_custom_index, row.geometry_flags});
    }
    return values;
}

const Json &wp375Component(const Json &response, std::string_view name) {
    const auto &components = response.at("components");
    const auto found = std::find_if(
        components.begin(), components.end(), [&](const auto &component) {
            return component.at("name").template get<std::string>() == name;
        });
    if (found == components.end()) {
        throw std::runtime_error("WP375 component query is missing '" +
                                 std::string{name} + "'");
    }
    return *found;
}

#if PELICAN_WITH_RPC
Json wp375Components(EditorCommandService &service, std::uint64_t &rpc_id,
                     std::string_view name,
                     std::string_view scene_id = "main") {
    return runWp374aRpcBytes(
        service, rpc_id++, "get_components",
        {{"scene_id", std::string{scene_id}}, {"name", std::string{name}}});
}

std::uint64_t wp375OpenActor(EditorCommandService &service,
                             std::uint64_t &rpc_id,
                             std::string_view display_name) {
    return runWp374aRpcBytes(
               service, rpc_id++, "open_editor_session",
               {{"display_name", std::string{display_name}}})
        .at("actor_id")
        .get<std::uint64_t>();
}

std::uint64_t wp375SceneRevision(EditorCommandService &service,
                                 std::uint64_t &rpc_id) {
    return runWp374aRpcBytes(service, rpc_id++, "scene_tree", Json::object())
        .at("scene_revision")
        .get<std::uint64_t>();
}

void wp375Save(EditorCommandService &service, std::uint64_t &rpc_id) {
    const auto result =
        runWp374aRpcBytes(service, rpc_id++, "save_scene", Json::object());
    REQUIRE(result.at("byte_count").get<std::uint64_t>() != 0);
    REQUIRE_FALSE(result.at("digest").at("hex").get<std::string>().empty());
}
#endif

void writeWp375BehaviorProject(const std::filesystem::path &root) {
    writeWp374aProject(root, std::nullopt, false, false, false);
    writeWp374aText(
        root / "scene.json",
        Json{{"schema", "pelican.scene"},
             {"version", 1},
             {"scenes",
              {{"main",
                {{"objects",
                  Json::array(
                      {{{"name", "BehaviorHost"},
                        {"components",
                         Json::array({{{"name", "transform"},
                                       {"pos", {0.0, 0.0, 0.0}},
                                       {"rotation", {0.0, 0.0, 0.0, 1.0}},
                                       {"scale", {1.0, 1.0, 1.0}}}})}}})}}}}}}
            .dump(2));
}

void writeWp375aInactivePrefabProject(const std::filesystem::path &root) {
    writeWp374aProject(root, std::nullopt, false, false, false);
    writeWp374aText(
        root / "prefabs" / "wp375a_unit.json",
        Json{{"schema", "pelican.prefab"},
             {"version", 1},
             {"name", "wp375a_unit"},
             {"parameters",
              Json::array({{{"name", "radius"},
                            {"type", "f32"},
                            {"default", 0.4}}})},
             {"components",
              Json::array({{{"name", "collider"},
                            {"id", "hit"},
                            {"shape", "sphere"},
                            {"radius", {{"$param", "radius"}}}}})}}
            .dump(2));
    const Json transform{{"name", "transform"},
                         {"pos", {0.0, 0.0, 0.0}},
                         {"rotation", {0.0, 0.0, 0.0, 1.0}},
                         {"scale", {1.0, 1.0, 1.0}}};
    writeWp374aText(
        root / "scene.json",
        Json{{"schema", "pelican.scene"},
             {"version", 2},
             {"scenes",
              {{"main",
                {{"objects",
                  Json::array(
                      {{{"name", "BehaviorHost"},
                        {"components",
                         Json::array(
                             {transform,
                              {{"name", "behavior"},
                               {"type", "wp155_project_behavior"},
                               {"params", {{"label", "main-active"}}}}})}}})}}},
               {"secondary",
                {{"objects",
                  Json::array(
                      {{{"name", "P"},
                        {"components", Json::array({transform})},
                        {"prefab",
                         {{"ref", "wp375a_unit"},
                          {"instance_id", "wp375a_secondary"}}}}})}}}}}}
            .dump(2));
}

void writeWp375aParticipantProject(const std::filesystem::path &root) {
    writeWp374aProject(root, std::nullopt, false, false, false);
    const Json transform{{"name", "transform"},
                         {"pos", {0.0, 0.0, 0.0}},
                         {"rotation", {0.0, 0.0, 0.0, 1.0}},
                         {"scale", {1.0, 1.0, 1.0}}};
    writeWp374aText(
        root / "scene.json",
        Json{{"schema", "pelican.scene"},
             {"version", 1},
             {"scenes",
              {{"main",
                {{"objects",
                  Json::array(
                      {{{"name", "CameraHost"},
                        {"components",
                         Json::array(
                             {transform,
                              {{"name", "camera"},
                               {"type", "perspective"},
                               {"yfov", 0.7},
                               {"znear", 0.1},
                               {"zfar", 100.0}}})}},
                       {{"name", "LightHost"},
                        {"components",
                         Json::array(
                             {{{"name", "light"},
                               {"type", "point"},
                               {"position", {1.0, 2.0, 3.0}},
                               {"intensity", 1.5},
                               {"color", {1.0, 1.0, 1.0}}}})}},
                       {{"name", "ColliderHost"},
                        {"components",
                         Json::array(
                             {transform,
                              {{"name", "collider"},
                               {"shape", "sphere"},
                               {"radius", 0.5}}})}},
                       {{"name", "BehaviorHost"},
                        {"components",
                         Json::array(
                             {transform,
                              {{"name", "behavior"},
                               {"type", "wp155_project_behavior"},
                               {"params", {{"label", "before"}}}}})}}})}}}}}}
            .dump(2));
}

#if PELICAN_WITH_RPC
Json commitWp375PrefabParameter(EditorCommandService &service,
                                std::uint64_t &rpc_id,
                                std::uint64_t actor,
                                const Json &object,
                                double value) {
    const auto &instance = object.at("prefab_instance");
    const auto accepted = runWp374aRpcBytes(
        service, rpc_id++, "set_prefab_parameter",
        {{"actor_id", actor},
         {"scene_revision", wp375SceneRevision(service, rpc_id)},
         {"closure_generation", instance.at("closure_generation")},
         {"provider_generation", instance.at("provider_generation")},
         {"object", object.at("authoring_object_id")},
         {"parameter", "radius"},
         {"value", value}});
    REQUIRE(accepted.at("status") == "accepted");
    invokeEditorCommitQueueHook();
    const auto result = runWp374aRpcBytes(
        service, rpc_id++, "get_edit_result",
        {{"ticket", accepted.at("ticket")}});
    INFO("WP375a prefab edit result: " << result.dump());
    REQUIRE(result.at("status") == "committed");
    return result;
}
#endif

void wp375FinishRuntime(std::unique_ptr<EditorCommandService> &service,
                        bool behavior = false) {
    service.reset();
    if (behavior) {
        GET_MODULE(BehaviorAttachmentArena).deactivateAll();
        shutdownConfiguredGameLogic();
    }
    GET_MODULE(VulkanManageCore).waitIdle();
}

class Wp375RuntimeScopeGuard final {
    std::unique_ptr<EditorCommandService> &service_;
    bool behavior_ = false;

  public:
    explicit Wp375RuntimeScopeGuard(
        std::unique_ptr<EditorCommandService> &service,
        bool behavior = false) noexcept
        : service_{service}, behavior_{behavior} {}
    Wp375RuntimeScopeGuard(const Wp375RuntimeScopeGuard &) = delete;
    Wp375RuntimeScopeGuard &operator=(const Wp375RuntimeScopeGuard &) = delete;
    ~Wp375RuntimeScopeGuard() { wp375FinishRuntime(service_, behavior_); }
};

void requireWp375MissingRendererParticipant() {
    FastModuleContainer modules;
    GET_MODULE(RayLayoutRuntimeState)
        .configure(RayLayoutSemanticMode::LegacyNoLayout);
    test_support::SceneProjectionTarget target{
        AuthoringSceneDocument::load(
            nlohmann::ordered_json{
                {"schema", "pelican.scene"},
                {"version", 2},
                {"scenes",
                 {{"main",
                   {{"objects",
                     nlohmann::ordered_json::array(
                         {{{"name", "Root"},
                           {"components",
                            nlohmann::ordered_json::array(
                                {{{"name", "transform"},
                                  {"pos", {0.0, 0.0, 0.0}},
                                  {"rotation", {0.0, 0.0, 0.0, 1.0}},
                                  {"scale", {1.0, 1.0, 1.0}}},
                                 {{"name", "simplemodelview"},
                                  {"model", "old"}}})}}})}}}}},
            }
                .dump(),
            SceneRevision{13})};
    const auto &scene = target.projectionState().resolved().scenes().front();
    REQUIRE(scene.objects.size() == 1);
    const auto object_id = scene.objects.front().authoring_object_id;
    const nlohmann::ordered_json authored{
        {"name", "simplemodelview"}, {"model", "next"}};
    const std::array operations{nlohmann::ordered_json{
        {"op", "set_component_value"},
        {"object_id", object_id.value},
        {"component_slot", "simplemodelview"},
        {"authored_component", authored},
    }};
    const auto canonical = freezeEditorCanonicalOperations(
        operations, target.projectionState());
    const std::array commands{makeSetComponentValueCommand(
        "main", "Root", "simplemodelview", authored)};
    const auto context = makeEditorProjectionContext(
        "main", EditorProjectionMode::Committed);
    auto plan_result = planEditorProjection(
        EditorProjectionPlanningRequest{
            .base_revision = target.projectionState().revision(),
            .commands = commands,
            .operations = canonical,
        },
        target.projectionState(), context);
    REQUIRE(plan_result.hasValue());
    auto plan = std::move(plan_result).value();
    ECSCoreTemplatePublic ecs;
    EditorRuntimeBindingState bindings{
        .epoch = 9,
        .codec_generation = context.layoutGeneration(),
        .behavior_generation = context.behaviorGeneration(),
    };
    const auto result = materializeEditorProjection(
        plan,
        EditorProjectionModules{
            .ecs = &ecs,
            .component_info = &GET_MODULE(ComponentInfoManager),
        },
        target, bindings, bindings.epoch, context.layoutGeneration(),
        context.behaviorGeneration());
    REQUIRE(result.status == EditorProjectionStatus::Failed);
    REQUIRE(result.error.has_value());
    INFO("WP375 missing renderer participant: " << result.error->message);
    REQUIRE_THAT(
        result.error->message,
        ContainsSubstring("editor_projection_participant_missing@1"));
    REQUIRE_THAT(result.error->message, ContainsSubstring("renderer"));
    REQUIRE(target.projectionState().revision() == SceneRevision{13});
}

} // namespace

#if PELICAN_WITH_RPC
TEST_CASE(
    "WP375a inactive prefab RPC is document-only and cannot reconcile the active behavior arena",
    "[WP375a][rpc][production][inactive-scene][prefab][behavior][regression][gpu]") {
    setupLogger();
#ifdef PELICAN_WP375_BEHAVIOR_FIXTURE
    Wp374aProjectRoot project{"wp375a_inactive_prefab"};
    writeWp375aInactivePrefabProject(project.path);
    const std::filesystem::path behavior_dll{
        PELICAN_WP375_BEHAVIOR_FIXTURE};
    std::uint64_t rpc_id = 375500;

    // The negative side is the reported regression: main owns the only live
    // behavior, while the edited behavior-free prefab belongs to secondary.
    {
        FastModuleContainer modules;
        loadWp374aScene(project.path, behavior_dll, "main");
        auto service = makeEditorRuntimeService();
        const auto actor = wp375OpenActor(
            *service, rpc_id, "WP375a inactive-scene regression");
        const auto secondary = runWp374aRpcBytes(
            *service, rpc_id++, "get_components",
            {{"scene_id", "secondary"}, {"name", "P"}});
        const auto before_document = Json::parse(
            AuthoringSceneAuthority::encodeSemantic(
                GET_MODULE(ProjectBasicConfig).sceneDocument()));
        const auto before_attachments =
            GET_MODULE(BehaviorAttachmentArena).snapshot();
        REQUIRE_FALSE(before_attachments.empty());
        REQUIRE(std::ranges::all_of(
            before_attachments,
            [](const BehaviorAttachmentInfo &attachment) {
                return attachment.active && !attachment.pending;
            }));

        (void)commitWp375PrefabParameter(
            *service, rpc_id, actor, secondary, 1.25);

        const auto after_attachments =
            GET_MODULE(BehaviorAttachmentArena).snapshot();
        REQUIRE(after_attachments.size() == before_attachments.size());
        for (std::size_t index = 0; index < before_attachments.size(); ++index) {
            CHECK(after_attachments[index].handle ==
                  before_attachments[index].handle);
            CHECK(after_attachments[index].entity ==
                  before_attachments[index].entity);
            CHECK(after_attachments[index].stable_name ==
                  before_attachments[index].stable_name);
            CHECK(after_attachments[index].active);
            CHECK_FALSE(after_attachments[index].pending);
        }

        const auto after_document = Json::parse(
            AuthoringSceneAuthority::encodeSemantic(
                GET_MODULE(ProjectBasicConfig).sceneDocument()));
        REQUIRE(after_document.at("scenes").at("main") ==
                before_document.at("scenes").at("main"));
        REQUIRE(after_document.at("scenes")
                    .at("secondary")
                    .at("objects")
                    .at(0)
                    .at("prefab")
                    .at("parameters")
                    .at("radius") == Catch::Approx(1.25));
        wp375FinishRuntime(service, true);
    }

    // The positive side makes secondary active. The same RPC must now reach
    // its real physics participant, proving that the edit itself was not
    // globally disabled to fix the negative side.
    {
        FastModuleContainer modules;
        loadWp374aScene(project.path, behavior_dll, "secondary");
        auto service = makeEditorRuntimeService();
        const auto actor = wp375OpenActor(
            *service, rpc_id, "WP375a active-secondary contrast");
        const auto secondary = runWp374aRpcBytes(
            *service, rpc_id++, "get_components",
            {{"scene_id", "secondary"}, {"name", "P"}});
        const auto before_physics = GET_MODULE(PhysWorld).snapshotPrepared();
        REQUIRE(before_physics.bindings.size() == 1);
        REQUIRE(before_physics.bindings.front().collider.radius ==
                Catch::Approx(0.4));

        (void)commitWp375PrefabParameter(
            *service, rpc_id, actor, secondary, 1.75);

        const auto after_physics = GET_MODULE(PhysWorld).snapshotPrepared();
        REQUIRE(after_physics.bindings.size() == 1);
        REQUIRE(after_physics.bindings.front().collider.radius ==
                Catch::Approx(1.75));
        wp375FinishRuntime(service, true);
    }
#else
    FAIL("WP375a production behavior fixture was not composed");
#endif
}

TEST_CASE(
    "WP375a pending behavior add rolls back after publication and succeeds on retry in one process",
    "[WP375a][rpc][production][behavior][rollback][mutation][gpu]") {
    setupLogger();
    Wp374aProjectRoot project{"wp375a_pending_rollback"};
    writeWp375BehaviorProject(project.path);
    std::uint64_t rpc_id = 375600;

    struct OneShotAfterPublication final : EditorProjectionFaultInjector {
        bool armed = true;
        bool fired = false;

        bool shouldFail(EditorProjectionFaultPoint point,
                        std::string_view adapter_name) noexcept override {
            if (!armed || fired ||
                point != EditorProjectionFaultPoint::AfterPublication ||
                adapter_name != "scene_pair") {
                return false;
            }
            fired = true;
            armed = false;
            return true;
        }
    } fault;

    FastModuleContainer modules;
    loadWp374aScene(project.path);
    auto service = makeEditorRuntimeService(
        EditorRuntimeServiceOptions{.projection_fault_injector = &fault});
    const auto actor = wp375OpenActor(
        *service, rpc_id, "WP375a pending rollback");
    const auto before = wp375Components(*service, rpc_id, "BehaviorHost");
    const auto object_id =
        before.at("authoring_object_id").get<std::uint64_t>();
    const auto before_revision = wp375SceneRevision(*service, rpc_id);
    const auto add = Json::array(
        {{{"op", "add_component"},
          {"object_id", object_id},
          {"component_index", 1},
          {"component",
           {{"name", "behavior"},
            {"type", "wp375a_dll_absent_behavior"},
            {"params", Json::object()}}}}});

    const auto failed = finishWp374aEdit(
        *service, rpc_id, actor, before_revision, add);
    REQUIRE(failed.at("status") == "failed");
    REQUIRE(fault.fired);
    REQUIRE(GET_MODULE(BehaviorAttachmentArena).snapshot().empty());
    REQUIRE(wp375SceneRevision(*service, rpc_id) == before_revision);
    REQUIRE(wp375Components(*service, rpc_id, "BehaviorHost")
                .at("components") == before.at("components"));

    (void)commitWp374aEdit(
        *service, rpc_id, actor, before_revision, add);
    const auto attachments = GET_MODULE(BehaviorAttachmentArena).snapshot();
    REQUIRE(attachments.size() == 1);
    REQUIRE(attachments.front().pending);
    REQUIRE_FALSE(attachments.front().active);
    REQUIRE(wp375Component(
                wp375Components(*service, rpc_id, "BehaviorHost"),
                "behavior")
                .at("pending") == true);
    wp375FinishRuntime(service);
}

TEST_CASE(
    "WP375b pending behavior journal resolves its attachment again after game-logic reload",
    "[WP375b][rpc][production][behavior][reload][journal][undo][regression][mutation][gpu]") {
    setupLogger();
#ifdef PELICAN_WP375_BEHAVIOR_FIXTURE
    Wp374aProjectRoot project{"wp375b_pending_reload_undo"};
    writeWp375BehaviorProject(project.path);
    const auto appearing_dll = project.path / "logic" / "appearing.dll";
    std::filesystem::create_directories(appearing_dll.parent_path());
    REQUIRE_FALSE(std::filesystem::exists(appearing_dll));

    (void)configureWp374aRuntime(project.path);
    GET_MODULE(EngineLaunchConfig).game_logic_dll = appearing_dll;
    REQUIRE_FALSE(initializeConfiguredGameLogic());
    REQUIRE(configuredGameLogicStatus().configured);
    REQUIRE_FALSE(configuredGameLogicStatus().loaded);
    TestSupport::requireVulkanDevice("WP375b pending behavior reload");
    GET_MODULE(ECSPredefinedRegistration).reg();
    test_support::loadScene(GET_MODULE(SceneLoader), "main");
    // Ensure EngineRpcEndpoint observes the production runtime-reload owner.
    (void)GET_MODULE(watch::ReloadService);

    std::istringstream rpc_input;
    std::ostringstream rpc_output;
    {
        EngineRpcEndpoint endpoint{rpc_input, rpc_output};
        std::uint64_t rpc_id = 375650;
        const auto rpc = [&](std::string_view method, Json params) {
            const auto id = rpc_id++;
            const auto response = Json::parse(endpoint.processLine(
                Json{{"jsonrpc", "2.0"},
                     {"id", id},
                     {"method", method},
                     {"params", std::move(params)}}
                    .dump()));
            INFO("WP375b engine RPC response: " << response.dump());
            if (response.contains("error")) {
                throw std::runtime_error("WP375b engine RPC failure: " +
                                         response.dump());
            }
            REQUIRE(response.at("id") == id);
            return response.at("result");
        };

        const auto actor =
            rpc("open_editor_session",
                {{"display_name", "WP375b pending reload undo"}})
                .at("actor_id")
                .get<std::uint64_t>();
        const auto host = rpc("get_components",
                              {{"scene_id", "main"},
                               {"name", "BehaviorHost"}});
        const auto object_id =
            host.at("authoring_object_id").get<std::uint64_t>();
        const auto revision = rpc("scene_tree", Json::object())
                                  .at("scene_revision")
                                  .get<std::uint64_t>();
        const auto accepted = rpc(
            "edit",
            {{"actor_id", actor},
             {"base_revision", revision},
             {"operations",
              Json::array(
                  {{{"op", "add_component"},
                    {"object_id", object_id},
                    {"component",
                     {{"name", "behavior"},
                      {"type", "wp155_project_behavior"},
                      {"params", {{"label", "pending"}}}}}}})}});
        REQUIRE(accepted.at("status") == "accepted");
        invokeEditorCommitQueueHook();
        const auto added =
            rpc("get_edit_result", {{"ticket", accepted.at("ticket")}});
        REQUIRE(added.at("status") == "committed");
        const auto pending = GET_MODULE(BehaviorAttachmentArena).snapshot();
        REQUIRE(pending.size() == 1);
        REQUIRE(pending.front().pending);
        REQUIRE_FALSE(pending.front().active);
        const auto pending_handle = pending.front().handle;
        const auto saved = rpc("save_scene", Json::object());
        REQUIRE(saved.at("byte_count").get<std::uint64_t>() != 0);

        std::filesystem::copy_file(
            std::filesystem::path{PELICAN_WP375_BEHAVIOR_FIXTURE},
            appearing_dll, std::filesystem::copy_options::overwrite_existing);
        const auto reloaded = rpc("reload_game_logic", Json::object());
        REQUIRE(reloaded.at("generation").get<std::uint64_t>() == 1);
        const auto active = GET_MODULE(BehaviorAttachmentArena).snapshot();
        REQUIRE(active.size() == 1);
        REQUIRE(active.front().active);
        REQUIRE_FALSE(active.front().pending);
        REQUIRE(active.front().handle != pending_handle);

        const auto undo = rpc(
            "undo",
            {{"actor_id", actor},
             {"base_revision",
              rpc("scene_tree", Json::object())
                  .at("scene_revision")},
             {"transaction_id", added.at("transaction_id")}});
        REQUIRE(undo.at("status") == "accepted");
        invokeEditorCommitQueueHook();
        const auto undone =
            rpc("get_edit_result", {{"ticket", undo.at("ticket")}});
        INFO("WP375b post-reload undo result: " << undone.dump());
        REQUIRE(undone.at("status") == "committed");
        REQUIRE(GET_MODULE(BehaviorAttachmentArena).snapshot().empty());
        REQUIRE(rpc("get_components",
                    {{"scene_id", "main"}, {"name", "BehaviorHost"}})
                    .at("components") == host.at("components"));
    }
    GET_MODULE(BehaviorAttachmentArena).deactivateAll();
    shutdownConfiguredGameLogic();
    GET_MODULE(VulkanManageCore).waitIdle();
#else
    FAIL("WP375b production behavior fixture was not composed");
#endif
}

TEST_CASE(
    "WP375a shipping BlueSpot validates behavior type before its missing runtime binding",
    "[WP375a][rpc][production][example][behavior][validation-order][gpu]") {
    setupLogger();
#if !PELICAN_RUNTIME_SHADER_COMPILER
    // The shipping example resolves B-layer surfaces from GLSL source, which a
    // compiler-OFF build cannot provide (same gate as the project smoke).
    SKIP("shipping example requires the runtime shader compiler");
#endif
#ifdef PELICAN_WP375_BEHAVIOR_FIXTURE
    const std::filesystem::path behavior_dll{
        PELICAN_WP375_BEHAVIOR_FIXTURE};
    std::uint64_t rpc_id = 375700;
    FastModuleContainer modules;
    loadWp375aShippingExample(behavior_dll);
    auto service = makeEditorRuntimeService();
    const auto actor = wp375OpenActor(
        *service, rpc_id, "WP375a BlueSpot validation order");
    const auto blue_spot = wp375Components(
        *service, rpc_id, "BlueSpot", "default_scene");
    REQUIRE_FALSE(blue_spot.contains("entity_id"));
    const auto object_id =
        blue_spot.at("authoring_object_id").get<std::uint64_t>();
    const auto revision = wp375SceneRevision(*service, rpc_id);

    const auto add_behavior = [&](std::string type) {
        return finishWp374aEdit(
            *service, rpc_id, actor, revision,
            Json::array(
                {{{"op", "add_component"},
                  {"object_id", object_id},
                  {"component",
                   {{"name", "behavior"},
                    {"type", std::move(type)},
                    {"params", {{"label", "blue"}}}}}}}));
    };

    const auto unknown = add_behavior("wp375a_no_such_behavior");
    REQUIRE(unknown.at("status") == "rejected");
    REQUIRE(unknown.at("error").at("code") ==
            "behavior_type_unknown@1");
    REQUIRE(unknown.at("error").at("payload").at("type") ==
            "wp375a_no_such_behavior");
    REQUIRE(unknown.at("error").at("payload").at("object_id") ==
            object_id);

    const auto known = add_behavior("wp155_project_behavior");
    REQUIRE(known.at("status") == "rejected");
    REQUIRE(known.at("error").at("code") ==
            "runtime_binding_missing@1");
    REQUIRE(known.at("error").at("payload").at("object_id") == object_id);
    REQUIRE(wp375SceneRevision(*service, rpc_id) == revision);
    REQUIRE(wp375Components(*service, rpc_id, "BlueSpot", "default_scene")
                .at("components") == blue_spot.at("components"));
    REQUIRE(GET_MODULE(BehaviorAttachmentArena).snapshot().empty());
    wp375FinishRuntime(service, true);
#else
    FAIL("WP375a production behavior fixture was not composed");
#endif
}

TEST_CASE(
    "WP375b inactive shipping-scene light preview cannot replace the active light runtime",
    "[WP375b][rpc][production][example][preview][inactive-scene][light][regression][mutation][gpu]") {
    setupLogger();
#if !PELICAN_RUNTIME_SHADER_COMPILER
    SKIP("shipping example requires the runtime shader compiler");
#endif
#ifdef PELICAN_WP375_BEHAVIOR_FIXTURE
    const std::filesystem::path behavior_dll{
        PELICAN_WP375_BEHAVIOR_FIXTURE};
    std::uint64_t rpc_id = 375750;
    FastModuleContainer modules;
    loadWp375aShippingExample(behavior_dll);
    auto service = makeEditorRuntimeService();
    Wp375RuntimeScopeGuard runtime_scope{service, true};
    const auto actor = wp375OpenActor(
        *service, rpc_id, "WP375b inactive light preview");

    const auto light_count = [] {
        const auto state = GET_MODULE(LightContainer).snapshotPrepared();
        return state.directional_lights.size() + state.point_lights.size() +
               state.spot_lights.size();
    };
    const auto preview_set = [&](const Json &object, double intensity) {
        const auto operation = Json{{"op", "set_component_value"},
                                    {"object_id", object.at("authoring_object_id")},
                                    {"component_slot", "light"},
                                    {"field_path", "/intensity"},
                                    {"value", intensity}};
        const auto opened = runWp374aRpcBytes(
            *service, rpc_id++, "open_preview",
            {{"actor_id", actor},
             {"operations", Json::array({operation})}});
        REQUIRE(opened.at("status") == "accepted");
        invokeEditorCommitQueueHook();
        const auto open_result = runWp374aRpcBytes(
            *service, rpc_id++, "get_preview_result",
            {{"request_id", opened.at("request_id")}});
        INFO("WP375b open preview result: " << open_result.dump());
        REQUIRE(open_result.at("status") == "open");
        return opened;
    };
    const auto commit_preview = [&](const Json &opened) {
        const auto accepted = runWp374aRpcBytes(
            *service, rpc_id++, "commit_preview",
            {{"actor_id", actor}, {"ticket", opened.at("ticket")}});
        REQUIRE(accepted.at("status") == "accepted");
        invokeEditorCommitQueueHook();
        const auto result = runWp374aRpcBytes(
            *service, rpc_id++, "get_preview_result",
            {{"request_id", accepted.at("request_id")}});
        INFO("WP375b commit preview result: " << result.dump());
        REQUIRE(result.at("status") == "committed");
    };
    const auto abort_preview = [&](const Json &opened) {
        const auto accepted = runWp374aRpcBytes(
            *service, rpc_id++, "abort_preview",
            {{"actor_id", actor}, {"ticket", opened.at("ticket")}});
        REQUIRE(accepted.at("status") == "accepted");
        invokeEditorCommitQueueHook();
        const auto result = runWp374aRpcBytes(
            *service, rpc_id++, "get_preview_result",
            {{"request_id", accepted.at("request_id")}});
        INFO("WP375b abort preview result: " << result.dump());
        REQUIRE(result.at("status") == "succeeded");
    };

    const auto before_document = Json::parse(
        AuthoringSceneAuthority::encodeSemantic(
            GET_MODULE(ProjectBasicConfig).sceneDocument()));
    REQUIRE(light_count() == 14);
    const auto inactive = wp375Components(
        *service, rpc_id, "SceneFlowKeyLight", "scene_flow_second");
    const auto inactive_preview = preview_set(inactive, 6.25);

    // The historical fault is already visible here: the inactive scene's
    // single light replaced all fourteen lights in the active scene.
    REQUIRE(light_count() == 14);
    commit_preview(inactive_preview);
    REQUIRE(light_count() == 14);

    const auto after_inactive = Json::parse(
        AuthoringSceneAuthority::encodeSemantic(
            GET_MODULE(ProjectBasicConfig).sceneDocument()));
    REQUIRE(after_inactive.at("scenes").at("default_scene") ==
            before_document.at("scenes").at("default_scene"));
    REQUIRE(wp375Component(
                wp375Components(*service, rpc_id, "SceneFlowKeyLight",
                                "scene_flow_second"),
                "light")
                .at("authored_json")
                .at("intensity") == Catch::Approx(6.25));

    // The abort path restores the committed document through a second
    // Preview plan. It must retain the inactive operation and select the same
    // DocumentOnly route instead of projecting the secondary light set.
    const auto inactive_abort = preview_set(inactive, 8.5);
    REQUIRE(light_count() == 14);
    abort_preview(inactive_abort);
    REQUIRE(light_count() == 14);
    REQUIRE(wp375Component(
                wp375Components(*service, rpc_id, "SceneFlowKeyLight",
                                "scene_flow_second"),
                "light")
                .at("authored_json")
                .at("intensity") == Catch::Approx(6.25));

    // Positive contrast: previewing a light in the active scene still reaches
    // the real LightContainer before the durable commit.
    const auto active = wp375Components(
        *service, rpc_id, "BlueSpot", "default_scene");
    const auto active_preview = preview_set(active, 3.75);
    const auto active_runtime = GET_MODULE(LightContainer).snapshotPrepared();
    REQUIRE(light_count() == 14);
    const auto active_index = active_runtime.spot_names.at("BlueSpot");
    REQUIRE(active_runtime.spot_lights.at(active_index).intensity ==
            Catch::Approx(3.75));
    commit_preview(active_preview);
    REQUIRE(wp375Component(
                wp375Components(*service, rpc_id, "BlueSpot", "default_scene"),
                "light")
                .at("authored_json")
                .at("intensity") == Catch::Approx(3.75));
#else
    FAIL("WP375b shipping production behavior fixture was not composed");
#endif
}

TEST_CASE(
    "WP375a production set routes publish camera light collider and behavior participant values",
    "[WP375a][rpc][production][route-participants][mutation][gpu]") {
    setupLogger();
#ifdef PELICAN_WP375_BEHAVIOR_FIXTURE
    Wp374aProjectRoot project{"wp375a_route_participants"};
    writeWp375aParticipantProject(project.path);
    const std::filesystem::path behavior_dll{
        PELICAN_WP375_BEHAVIOR_FIXTURE};
    std::uint64_t rpc_id = 375800;

    FastModuleContainer modules;
    loadWp374aScene(project.path, behavior_dll);
    auto service = makeEditorRuntimeService();
    const auto actor = wp375OpenActor(
        *service, rpc_id, "WP375a route participant values");
    const auto camera = wp375Components(*service, rpc_id, "CameraHost");
    const auto light = wp375Components(*service, rpc_id, "LightHost");
    const auto collider = wp375Components(*service, rpc_id, "ColliderHost");
    const auto behavior = wp375Components(*service, rpc_id, "BehaviorHost");
    const auto object_id = [](const Json &object) {
        return object.at("authoring_object_id").get<std::uint64_t>();
    };

    commitWp374aEdit(
        *service, rpc_id, actor, wp375SceneRevision(*service, rpc_id),
        Json::array(
            {{{"op", "set_component_value"},
              {"object_id", object_id(camera)},
              {"component_slot", "camera"},
              {"field_path", "/yfov"},
              {"value", 1.1}},
             {{"op", "set_component_value"},
              {"object_id", object_id(light)},
              {"component_slot", "light"},
              {"field_path", "/intensity"},
              {"value", 4.5}},
             {{"op", "set_component_value"},
              {"object_id", object_id(collider)},
              {"component_slot", "collider"},
              {"field_path", "/radius"},
              {"value", 2.25}},
             {{"op", "set_component_value"},
              {"object_id", object_id(behavior)},
              {"component_slot", "behavior"},
              {"attachment_index", 1},
              {"field_path", "/params/label"},
              {"value", "after"}}}));

    const auto camera_state = GET_MODULE(Camera).snapshotPrepared();
    REQUIRE(camera_state.scene_cameras.at("CameraHost")
                .projection.yfov == Catch::Approx(1.1));

    const auto light_state = GET_MODULE(LightContainer).snapshotPrepared();
    const auto light_index = light_state.point_names.at("LightHost");
    REQUIRE(light_state.point_lights.at(light_index).intensity ==
            Catch::Approx(4.5));

    const auto physics_state = GET_MODULE(PhysWorld).snapshotPrepared();
    REQUIRE(physics_state.bindings.size() == 1);
    REQUIRE(physics_state.bindings.front().collider.radius ==
            Catch::Approx(2.25));

    const auto attachments = GET_MODULE(BehaviorAttachmentArena).snapshot();
    REQUIRE(attachments.size() == 1);
    REQUIRE(attachments.front().active);
    REQUIRE(Json::parse(attachments.front().canonical_params).at("label") ==
            "after");

    REQUIRE(wp375Component(
                wp375Components(*service, rpc_id, "CameraHost"), "camera")
                .at("authored_json")
                .at("yfov") == Catch::Approx(1.1));
    REQUIRE(wp375Component(
                wp375Components(*service, rpc_id, "LightHost"), "light")
                .at("authored_json")
                .at("intensity") == Catch::Approx(4.5));
    REQUIRE(wp375Component(
                wp375Components(*service, rpc_id, "ColliderHost"),
                "collider")
                .at("authored_json")
                .at("radius") == Catch::Approx(2.25));
    REQUIRE(wp375Component(
                wp375Components(*service, rpc_id, "BehaviorHost"),
                "behavior")
                .at("authored_json")
                .at("params")
                .at("label") == "after");
    wp375FinishRuntime(service, true);
#else
    FAIL("WP375a production behavior fixture was not composed");
#endif
}

#endif

TEST_CASE(
    "WP377 authored pelican_ray_instance force opaque reaches the material alpha profile row guard",
    "[WP377][WP369][ray-tracing][rt3-1][instance-flags][profile][production][gpu]") {
    setupLogger();
#if PELICAN_RUNTIME_SHADER_COMPILER
    for (const bool force_opaque : {false, true}) {
        DYNAMIC_SECTION("authored flag "
                        << (force_opaque ? "force_opaque"
                                         : "force_no_opaque")) {
            Wp374aProjectRoot project{
                force_opaque ? "wp377_force_opaque"
                             : "wp377_force_no_opaque"};
            writeWp374aProject(
                project.path, 2, true, true, true,
                Json{{"name", "pelican_ray_instance"},
                     {"instance_flags",
                      Json::array({force_opaque ? "force_opaque"
                                                : "force_no_opaque"})}},
                true);

            FastModuleContainer modules;
            loadWp374aScene(project.path);
            if (force_opaque) {
                CHECK_THROWS_WITH(
                    renderWp374aProductionRows(),
                    ContainsSubstring(
                        "pelican.ray_profile_opaque_as@1") &&
                        ContainsSubstring("force_opaque"));
            } else {
                const auto rows = renderWp374aProductionRows();
                REQUIRE_FALSE(rows.rows.empty());
                for (const auto &row : rows.rows) {
                    CHECK(row.instance_flags ==
                          rayInstanceForceNoOpaque);
                }
            }
            GET_MODULE(VulkanManageCore).waitIdle();
        }
    }
#else
    SUCCEED("WP377 GPU profile acceptance is compiler-ON only");
#endif
}


TEST_CASE("WP374a all ten root layout AS component states cross real SceneLoader",
          "[WP374a][WP374][ray-tracing][rt3-1][state-table][scene-loader][production][mutation-2][gpu]") {
    setupLogger();
    struct State {
        int number;
        std::optional<std::uint32_t> layout_version;
        bool with_as;
        bool with_component;
        std::string_view expected_error;
    };
    constexpr std::array states{
        State{1, std::nullopt, false, false, {}},
        State{2, std::nullopt, false, true,
              "pelican.ray_instance_requires_layout_v2@1"},
        State{3, 1, false, false, {}},
        State{4, 1, false, true,
              "pelican.ray_instance_requires_layout_v2@1"},
        State{5, 1, true, false,
              "pelican.ray_as_declaration_requires_layout_v2@1"},
        State{6, 1, true, true,
              "pelican.ray_as_declaration_requires_layout_v2@1"},
        State{7, 2, false, false, {}},
        State{8, 2, false, true, {}},
        State{9, 2, true, false, {}},
        State{10, 2, true, true, {}},
    };

    for (const auto &state : states) {
        DYNAMIC_SECTION("state #" << state.number) {
            INFO("WP374a state table stage: write project");
            Wp374aProjectRoot project{
                "state_" + std::to_string(state.number)};
            const bool renderable =
                state.with_component && state.layout_version == 2;
            writeWp374aProject(project.path, state.layout_version,
                                state.with_as, state.with_component,
                                renderable);
            INFO("WP374a state table stage: create production modules");
            FastModuleContainer modules;
            INFO("WP374a state table stage: load through SceneLoader");

#if !PELICAN_RUNTIME_SHADER_COMPILER
            if (state.layout_version) {
                CHECK_THROWS_WITH(
                    loadWp374aScene(project.path),
                    ContainsSubstring(
                        "pelican.ray_tracing.layout_requires_runtime_compiler@1"));
                continue;
            }
#endif
            if (!state.expected_error.empty()) {
                CHECK_THROWS_WITH(loadWp374aScene(project.path),
                                  ContainsSubstring(
                                      std::string{state.expected_error}));
                continue;
            }
            CHECK_NOTHROW(loadWp374aScene(project.path));
            const auto mode = GET_MODULE(RayLayoutRuntimeState).mode();
            if (!state.layout_version) {
                CHECK(mode == RayLayoutSemanticMode::LegacyNoLayout);
            } else if (*state.layout_version == 1) {
                CHECK(mode == RayLayoutSemanticMode::LayoutV1Q);
            } else {
                CHECK(mode == RayLayoutSemanticMode::LayoutV2);
            }
            if (state.with_component) {
                const auto id = GET_MODULE(SceneLoader).objectId("Chair");
                REQUIRE(id.has_value());
                const auto *component = GET_MODULE(ECSCore)
                                            .getTemplatePublicModule()
                                            .tryComponent<RayInstanceComponent>(
                                                *id);
                REQUIRE(component != nullptr);
                CHECK(component->mask == std::optional<std::uint8_t>{9});
            }
            if (auto *vkcore =
                    FastModuleContainer::tryGet<VulkanManageCore>()) {
                vkcore->waitIdle();
            }
        }
    }
}

#if PELICAN_WITH_RPC
TEST_CASE(
    "WP374a real RPC bytes drive add set save fresh reload remove into committed TLAS rows",
    "[WP374a][WP374][ray-tracing][rt3-1][rpc][production][tlas][mutation-1][gpu]") {
    setupLogger();
#if PELICAN_RUNTIME_SHADER_COMPILER
    Wp374aProjectRoot project{"rpc_full_route"};
    writeWp374aProject(project.path, 2, true, false, true);
    std::uint64_t rpc_id = 374000;
    std::uint64_t saved_object_id = 0;

    {
        FastModuleContainer modules;
        INFO("WP374a RPC stage: initial load");
        loadWp374aScene(project.path);
        INFO("WP374a RPC stage: production service");
        auto service = makeEditorRuntimeService();
        const auto tree = runWp374aRpcBytes(
            *service, rpc_id++, "scene_tree", Json::object());
        REQUIRE(tree.at("objects").size() == 1);
        saved_object_id = tree.at("objects")
                              .at(0)
                              .at("authoring_object_id")
                              .get<std::uint64_t>();
        const auto session = runWp374aRpcBytes(
            *service, rpc_id++, "open_editor_session",
            {{"display_name", "WP374a production RPC"}});
        const auto actor = session.at("actor_id").get<std::uint64_t>();

        commitWp374aEdit(
            *service, rpc_id, actor,
            tree.at("scene_revision").get<std::uint64_t>(),
            Json::array({{{"op", "add_component"},
                          {"object_id", saved_object_id},
                          {"component",
                           {{"name", "pelican_ray_instance"},
                            {"mask", 17},
                            {"instance_flags", {"force_opaque"}},
                            {"custom_index", 0x123456}}}}}));
        INFO("WP374a RPC stage: render after add");
        requireWp374aTlasValues(
            renderWp374aProductionRows(), saved_object_id, 17,
            rayInstanceForceOpaque, 0x123456);

        const auto after_add = runWp374aRpcBytes(
            *service, rpc_id++, "scene_tree", Json::object());
        commitWp374aEdit(
            *service, rpc_id, actor,
            after_add.at("scene_revision").get<std::uint64_t>(),
            Json::array(
                {{{"op", "set_component_value"},
                  {"object_id", saved_object_id},
                  {"component_slot", "pelican_ray_instance"},
                  {"field_path", "/mask"},
                  {"value", 34}},
                 {{"op", "set_component_value"},
                  {"object_id", saved_object_id},
                  {"component_slot", "pelican_ray_instance"},
                  {"field_path", "/instance_flags"},
                  {"value", {"force_no_opaque"}}},
                 {{"op", "set_component_value"},
                  {"object_id", saved_object_id},
                  {"component_slot", "pelican_ray_instance"},
                  {"field_path", "/custom_index"},
                 {"value", 0x654321}}}));
        INFO("WP374a RPC stage: render after set");
        requireWp374aTlasValues(
            renderWp374aProductionRows(), saved_object_id, 34,
            rayInstanceForceNoOpaque, 0x654321);

        const auto save = runWp374aRpcBytes(
            *service, rpc_id++, "save_scene", Json::object());
        INFO("WP374a RPC stage: saved");
        CHECK(save.at("scene_revision") ==
              runWp374aRpcBytes(*service, rpc_id++, "scene_tree",
                                Json::object())
                  .at("scene_revision"));
        const auto saved_bytes = readWp374aText(project.path / "scene.json");
        CHECK(saved_bytes.find("\"pelican_ray_instance\"") !=
              std::string::npos);
        CHECK(saved_bytes.find("\"ray_instance\"") == std::string::npos);
        service.reset();
        GET_MODULE(VulkanManageCore).waitIdle();
    }

    {
        FastModuleContainer modules;
        INFO("WP374a RPC stage: fresh load");
        loadWp374aScene(project.path);
        auto service = makeEditorRuntimeService();
        const auto tree = runWp374aRpcBytes(
            *service, rpc_id++, "scene_tree", Json::object());
        REQUIRE(tree.at("objects").size() == 1);
        const auto reloaded_object_id = tree.at("objects")
                                            .at(0)
                                            .at("authoring_object_id")
                                            .get<std::uint64_t>();
        CHECK(reloaded_object_id == saved_object_id);
        requireWp374aTlasValues(
            renderWp374aProductionRows(), reloaded_object_id, 34,
            rayInstanceForceNoOpaque, 0x654321);

        const auto session = runWp374aRpcBytes(
            *service, rpc_id++, "open_editor_session",
            {{"display_name", "WP374a fresh reload RPC"}});
        commitWp374aEdit(
            *service, rpc_id,
            session.at("actor_id").get<std::uint64_t>(),
            tree.at("scene_revision").get<std::uint64_t>(),
            Json::array({{{"op", "remove_component"},
                          {"object_id", reloaded_object_id},
                          {"component_slot", "pelican_ray_instance"}}}));
        requireWp374aTlasValues(
            renderWp374aProductionRows(), reloaded_object_id, 127,
            rayInstanceTriangleFacingCullDisable, 0);
        service.reset();
        GET_MODULE(VulkanManageCore).waitIdle();
    }
#else
    SUCCEED(
        "WP374a v2 production RPC rendering is compiler-ON only; the compiler-OFF precedence test remains active");
#endif
}
#endif

#if PELICAN_WITH_RPC
TEST_CASE(
    "WP379 production scene RPC and snapshot boundaries share component_unknown",
    "[WP379][component][namespace][production][rpc][snapshot][gpu][mutation-unknown]") {
    setupLogger();
#if PELICAN_RUNTIME_SHADER_COMPILER
    Wp374aProjectRoot invalid_scene{"wp379_unknown_scene"};
    Wp374aProjectRoot live_project{"wp379_unknown_rpc"};
    // Intentional old-name occurrence: the production SceneLoader negative.
    writeWp374aProject(
        invalid_scene.path, 2, true, true, true,
        Json{{"name", "ray_instance"}, {"marker", 379}});
    writeWp374aProject(live_project.path, 2, true, false, true);

    {
        FastModuleContainer modules;
        try {
            loadWp374aScene(invalid_scene.path);
            FAIL("old unregistered component name unexpectedly loaded");
        } catch (const ComponentUnknownError &error) {
            CHECK(std::string{error.code()} == "component_unknown@1");
            CHECK(error.payload() ==
                  nlohmann::ordered_json{{"name", "ray_instance"}});
            CHECK(std::string{error.what()} ==
                  "component_unknown@1{\"name\":\"ray_instance\"}");
        }
        GET_MODULE(VulkanManageCore).waitIdle();
    }

    // Keep the production SceneLoader boundary active without RPC; only the
    // editor and snapshot transport assertions depend on the RPC surface.
#if PELICAN_WITH_RPC
    {
        FastModuleContainer modules;
        loadWp374aScene(live_project.path);
        auto service = makeEditorRuntimeService();
        std::uint64_t rpc_id = 379000;
        const auto tree = runWp374aRpcBytes(
            *service, rpc_id++, "scene_tree", Json::object());
        REQUIRE(tree.at("objects").size() == 1);
        const auto object_id = tree.at("objects")
                                   .at(0)
                                   .at("authoring_object_id")
                                   .get<std::uint64_t>();
        const auto actor = wp375OpenActor(
            *service, rpc_id, "WP379 unknown component boundary");

        // Intentional old-name occurrence: real JSON-RPC add_component
        // rejection, with the same code/payload as SceneLoader.
        const auto rejected = runWp374aRpcBytes(
            *service, rpc_id++, "edit",
            {{"actor_id", actor},
             {"base_revision",
              tree.at("scene_revision").get<std::uint64_t>()},
             {"operations",
              Json::array({{{"op", "add_component"},
                            {"object_id", object_id},
                            {"component",
                             {{"name", "ray_instance"},
                              {"marker", 379}}}}})},
             {"coalesce_key", "wp379-unknown-component"}});
        REQUIRE(rejected.at("status") == "rejected");
        REQUIRE(rejected.at("error").at("code") ==
                std::string{componentUnknownErrorCode});
        CHECK(rejected.at("error").at("payload") ==
              Json{{"name", "ray_instance"}});
        CHECK(rejected.at("error").at("message") ==
              "component_unknown@1{\"name\":\"ray_instance\"}");

        // Intentional old-name occurrence: SnapshotV1 enters through the real
        // RpcServer and preserves snapshot_invalid as its outer contract.
        const auto snapshot_bytes =
            Json{{"schema", "pelican.scene"},
                 {"version", 1},
                 {"scenes",
                  {{"main",
                    {{"objects",
                      Json::array(
                          {{{"name", "SnapshotLegacyName"},
                            {"components",
                             Json::array({{{"name", "ray_instance"},
                                           {"marker", 379}}})}}})}}}}}}
                .dump();
        const auto digest = picosha2::hash256_hex_string(
            snapshot_bytes.begin(), snapshot_bytes.end());
        const auto snapshot_response = runWp374aRpcResponse(
            *service, rpc_id++, "import_scene_snapshot",
            {{"schema_version", 1},
             {"semantic_scene_bytes", snapshot_bytes},
             {"digest", {{"algorithm", "sha256"}, {"hex", digest}}},
             {"current_scene_id", "main"}});
        REQUIRE(snapshot_response.contains("error"));
        const auto &snapshot_error = snapshot_response.at("error");
        REQUIRE(snapshot_error.at("data").at("code") ==
                "snapshot_invalid");
        CHECK(snapshot_error.at("data").at("detail") ==
              "component_unknown@1{\"name\":\"ray_instance\"}");
        CHECK(wp375SceneRevision(*service, rpc_id) ==
              tree.at("scene_revision").get<std::uint64_t>());

        service.reset();
        GET_MODULE(VulkanManageCore).waitIdle();
    }
#endif
#else
    SUCCEED(
        "WP379 layout-v2 unknown-boundary production fixture is compiler-ON only; compiler-OFF precedence is covered by layout_requires_runtime_compiler@1");
#endif
}
#endif

TEST_CASE(
    "WP379 production bootstrap keeps external ray_instance beside the builtin canonical component",
    "[WP379][component][namespace][production][dll][coexistence][gpu][mutation-dll]") {
    setupLogger();
#if PELICAN_RUNTIME_SHADER_COMPILER && defined(PELICAN_WP379_RAY_INSTANCE_FIXTURE)
    Wp374aProjectRoot project{"wp379_external_coexistence"};
    writeWp374aProject(
        project.path, 2, true, true, true,
        Json{{"name", "pelican_ray_instance"}, {"mask", 37}});
    std::vector<std::array<std::uint32_t, 4>> control_row_values;

    {
        FastModuleContainer modules;
        loadWp374aScene(project.path);
        const auto *resolved =
            GET_MODULE(ProjectBasicConfig).resolvedScene().findScene("main");
        REQUIRE(resolved != nullptr);
        REQUIRE(resolved->objects.size() == 1);
        const auto authoring_object_id =
            resolved->objects.front().authoring_object_id.value;
        const auto rows = renderWp374aProductionRows();
        requireWp374aTlasValues(
            rows, authoring_object_id, 37,
            rayInstanceTriangleFacingCullDisable, 0);
        control_row_values =
            wp379ResolvedTlasRowValues(rows, authoring_object_id);
        REQUIRE_FALSE(control_row_values.empty());
        GET_MODULE(VulkanManageCore).waitIdle();
    }

    auto authored = Json::parse(readWp374aText(project.path / "scene.json"));
    // Intentional old-name occurrence: serialized external component owned by
    // the real fixture DLL, not an alias for the builtin.
    authored["scenes"]["main"]["objects"][0]["components"].push_back(
        {{"name", "ray_instance"}, {"marker", 379}});
    writeWp374aText(project.path / "scene.json", authored.dump(2));

    {
        FastModuleContainer modules;
        try {
            loadWp374aScene(project.path);
            FAIL("control without the fixture DLL unexpectedly resolved the old name");
        } catch (const ComponentUnknownError &error) {
            CHECK(std::string{error.code()} == "component_unknown@1");
            CHECK(error.payload() ==
                  nlohmann::ordered_json{{"name", "ray_instance"}});
        }
        GET_MODULE(VulkanManageCore).waitIdle();
    }

    {
        FastModuleContainer modules;
        const std::filesystem::path fixture_dll{
            PELICAN_WP379_RAY_INSTANCE_FIXTURE};
        loadWp374aScene(project.path, fixture_dll);
        REQUIRE(configuredGameLogicStatus().loaded);

        const auto &component_info = GET_MODULE(ComponentInfoManager);
        const auto builtin_id =
            component_info.getComponentIdByName("pelican_ray_instance");
        // Intentional old-name occurrence: query the external registration.
        const auto external_id =
            component_info.getComponentIdByName("ray_instance");
        CHECK(builtin_id != external_id);
        CHECK(component_info.get(builtin_id).owner ==
              internal::engineRegistrationOwner);
        CHECK(component_info.get(external_id).owner !=
              internal::engineRegistrationOwner);

        const auto *resolved =
            GET_MODULE(ProjectBasicConfig).resolvedScene().findScene("main");
        REQUIRE(resolved != nullptr);
        REQUIRE(resolved->objects.size() == 1);
        const auto &components = resolved->objects.front().components;
        CHECK(std::ranges::count(components, std::string{"pelican_ray_instance"},
                                 &ResolvedComponent::name) == 1);
        // Intentional old-name occurrence: both semantic names remain distinct.
        CHECK(std::ranges::count(components, std::string{"ray_instance"},
                                 &ResolvedComponent::name) == 1);

        const auto entity = GET_MODULE(SceneLoader).objectId("Chair");
        REQUIRE(entity.has_value());
        auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
        const auto *builtin = ecs.tryComponent<RayInstanceComponent>(*entity);
        const auto *external =
            ecs.tryComponent<Wp379ExternalRayInstanceComponent>(*entity);
        REQUIRE(builtin != nullptr);
        REQUIRE(external != nullptr);
        CHECK(builtin->mask == std::optional<std::uint8_t>{37});
        CHECK(external->marker == 379);
        const auto authoring_object_id =
            resolved->objects.front().authoring_object_id.value;
        const auto rows = renderWp374aProductionRows();
        requireWp374aTlasValues(
            rows, authoring_object_id, 37,
            rayInstanceTriangleFacingCullDisable, 0);
        const auto coexistence_row_values =
            wp379ResolvedTlasRowValues(rows, authoring_object_id);
        REQUIRE_FALSE(coexistence_row_values.empty());
        CHECK(coexistence_row_values == control_row_values);

        // Destroy values whose lifecycle callbacks live in the DLL before
        // unregistering and unloading that DLL.
        GET_MODULE(ECSCore).clearEntities();
        shutdownConfiguredGameLogic();
        CHECK_FALSE(configuredGameLogicStatus().loaded);
        GET_MODULE(VulkanManageCore).waitIdle();
    }
#elif !PELICAN_RUNTIME_SHADER_COMPILER
    SUCCEED(
        "WP379 DLL coexistence uses layout v2 and is compiler-ON only; compiler-OFF precedence is covered separately");
#else
    FAIL("WP379 production external component fixture was not composed");
#endif
}

TEST_CASE(
    "WP379a real game DLL RPC classification and terminal shutdown purge share the production boundary",
    "[WP379a][component][namespace][production][dll][rpc][cpu][mutation-owner][mutation-shutdown]") {
    setupLogger();
#if defined(_WIN32) && defined(PELICAN_WP379_RAY_INSTANCE_FIXTURE)
    const std::filesystem::path fixture_dll{
        PELICAN_WP379_RAY_INSTANCE_FIXTURE};
    const auto owner_count_before = internal::registrationOwnerCount();
    REQUIRE(owner_count_before == 0);

    {
        FastModuleContainer modules;
        auto &reloader = GET_MODULE(GameLogicReloader);
        REQUIRE(reloader.initialize(fixture_dll));
        auto &component_info = GET_MODULE(ComponentInfoManager);
        const auto external_id =
            component_info.getComponentIdByName("ray_instance");
        const auto external_owner = component_info.get(external_id).owner;
        REQUIRE(external_owner != internal::engineRegistrationOwner);
        CHECK(internal::componentRegistrationCount(external_owner) == 1);

#if PELICAN_WITH_RPC
        auto document = AuthoringSceneDocument::load(
            Json{{"schema", "pelican.scene"},
                 {"version", 1},
                 {"scenes",
                  {{"main",
                    {{"objects",
                      Json::array({{{"name", "RpcTarget"},
                                    {"components", Json::array()}}})}}}}}}
                .dump(),
            SceneRevision{1});
        auto resolved = ResolvedSceneResolver::resolve(
            document, SceneResolverGeneration{1}, {}, {}, {});
        EditorCommandService service{EditorCommandServiceDependencies{
            .document = [&]() -> const AuthoringSceneDocument & {
                return document;
            },
            .resolved_scene = [&]() -> const ResolvedScene & {
                return resolved;
            },
            .current_scene_id = [] { return std::string{"main"}; },
            .edit = EditorEditRuntimeDependencies{
                .document = [&]() -> const AuthoringSceneDocument & {
                    return document;
                },
                .current_scene_id = [] { return std::string{"main"}; },
                .execute = [](const EditorEditExecutionRequest &)
                    -> EditorProjectionResult {
                    throw std::logic_error(
                        "rejected WP379a edit must not execute");
                },
                .component_exists = [&](std::string_view name) {
                    return component_info.hasComponent(name);
                },
            },
        }};
        std::uint64_t rpc_id = 379100;
        const auto actor = wp375OpenActor(
            service, rpc_id, "WP379a external component classification");
        const auto object_id = resolved.scenes().front().objects.front()
                                   .authoring_object_id.value;
        const auto rejected = runWp374aRpcBytes(
            service, rpc_id++, "edit",
            {{"actor_id", actor},
             {"base_revision", document.revision().value},
             {"operations",
              Json::array({{{"op", "add_component"},
                            {"object_id", object_id},
                            {"component",
                             {{"name", "ray_instance"},
                              {"marker", 379}}}}})}});
        REQUIRE(rejected.at("status") == "rejected");
        REQUIRE(rejected.at("error").at("code") == "not_editable");
#endif

        bool external_component_visible_during_teardown = false;
        RuntimeTeardownGuard teardown{
            RuntimeTeardownActions{
                .owner_callbacks = [&] {
                    external_component_visible_during_teardown =
                        component_info.hasComponent("ray_instance");
                },
            },
            RuntimeTeardownMode::terminal_shutdown};
        internal::runTerminalShutdown(teardown);
        CHECK(external_component_visible_during_teardown);
        CHECK_FALSE(reloader.status().loaded);
        CHECK_THROWS_AS(component_info.getComponentIdByName("ray_instance"),
                        ComponentUnknownError);
        CHECK(internal::componentRegistrationCount(external_owner) == 0);
        CHECK_FALSE(internal::isRegistrationOwnerCurrent(external_owner));
    }

    CHECK(internal::registrationOwnerCount() == 0);

    // The owner slot and callback ledger must also permit a fresh load in the
    // same process after the terminal container has gone away.
    {
        FastModuleContainer modules;
        auto &reloader = GET_MODULE(GameLogicReloader);
        REQUIRE(reloader.initialize(fixture_dll));
        CHECK_NOTHROW(GET_MODULE(ComponentInfoManager)
                          .getComponentIdByName("ray_instance"));
        RuntimeTeardownGuard teardown{
            RuntimeTeardownMode::terminal_shutdown};
        internal::runTerminalShutdown(teardown);
        CHECK_FALSE(reloader.status().loaded);
    }
    CHECK(internal::registrationOwnerCount() == 0);
#else
    SKIP("WP379a real game-DLL unload fixture is Windows-only");
#endif
}

TEST_CASE(
    "WP375 production SceneLoader preflight rejects a non-renderable ray instance beside a valid scene",
    "[WP375][ray-tracing][scene-loader][production][preflight][mutation-preflight][gpu]") {
    setupLogger();
#if PELICAN_RUNTIME_SHADER_COMPILER
    Wp374aProjectRoot valid{"wp375_preflight_valid"};
    Wp374aProjectRoot invalid{"wp375_preflight_invalid"};
    writeWp374aProject(valid.path, 2, true, true, true);
    writeWp374aProject(invalid.path, 2, true, true, false);

    {
        FastModuleContainer modules;
        CHECK_NOTHROW(loadWp374aScene(valid.path));
        GET_MODULE(VulkanManageCore).waitIdle();
    }
    {
        FastModuleContainer modules;
        CHECK_THROWS_WITH(
            loadWp374aScene(invalid.path),
            ContainsSubstring("pelican.ray_instance_not_ray_traced@1"));
    }
#else
    SUCCEED("The caller mutation is exercised in the runtime-compiler ON production tier");
#endif
}

#if PELICAN_WITH_RPC
TEST_CASE(
    "WP375 production acceptance a materializes transform runtime before save and fresh reload",
    "[WP375][rpc][production][mutation-a][gpu]") {
    setupLogger();
    Wp374aProjectRoot project{"wp375_mutation_a"};
    writeWp374aProject(project.path, 2, false, false, true);
#if !PELICAN_RUNTIME_SHADER_COMPILER
    FastModuleContainer modules;
    INFO("WP375 acceptance a compiler-OFF layout precedence");
    REQUIRE_THROWS_WITH(
        configureWp374aRuntime(project.path),
        ContainsSubstring(
            "pelican.ray_tracing.layout_requires_runtime_compiler@1"));
    return;
#endif
    std::uint64_t rpc_id = 375100;

    {
        FastModuleContainer modules;
        loadWp374aScene(project.path);
        auto service = makeEditorRuntimeService();
        const auto actor = wp375OpenActor(
            *service, rpc_id, "WP375 materialize acceptance");
        const auto chair = wp375Components(*service, rpc_id, "Chair");
        const auto object_id =
            chair.at("authoring_object_id").get<std::uint64_t>();
        commitWp374aEdit(
            *service, rpc_id, actor, wp375SceneRevision(*service, rpc_id),
            Json::array({{{"op", "set_component_value"},
                          {"object_id", object_id},
                          {"component_slot", "transform"},
                          {"field_path", "/pos/0"},
                          {"value", 6.25}}}));

        const auto edited = wp375Components(*service, rpc_id, "Chair");
        REQUIRE(wp375Component(edited, "transform")
                    .at("runtime_json")
                    .at("world_trs")
                    .at("pos")
                    .at(0)
                    .get<double>() == Catch::Approx(6.25));
        wp375Save(*service, rpc_id);
        wp375FinishRuntime(service);
    }

    {
        FastModuleContainer modules;
        loadWp374aScene(project.path);
        auto service = makeEditorRuntimeService();
        const auto reloaded = wp375Components(*service, rpc_id, "Chair");
        const auto &transform = wp375Component(reloaded, "transform");
        REQUIRE(transform.at("authored_json")
                    .at("pos")
                    .at(0)
                    .get<double>() == Catch::Approx(6.25));
        REQUIRE(transform.at("runtime_json")
                    .at("world_trs")
                    .at("pos")
                    .at(0)
                    .get<double>() == Catch::Approx(6.25));
        wp375FinishRuntime(service);
    }
}

TEST_CASE(
    "WP375 production acceptance b persists committed target through fresh reload",
    "[WP375][rpc][production][mutation-b][gpu]") {
    setupLogger();
#if !PELICAN_RUNTIME_SHADER_COMPILER
    INFO("WP375 acceptance b compiler-OFF no-layout RPC persistence");
#endif
    Wp374aProjectRoot project{"wp375_mutation_b"};
    // The transform edit under test needs no model. A renderable object pulls
    // a B-layer surface whose source is unavailable without the runtime
    // shader compiler, so compiler-OFF keeps the fixture non-renderable.
    writeWp374aProject(project.path, std::nullopt, false, false,
                       PELICAN_RUNTIME_SHADER_COMPILER != 0);
#if !PELICAN_RUNTIME_SHADER_COMPILER
    // A non-renderable fixture writes no objects; the edit under test only
    // needs a transform, so give the same object name with a transform alone.
    writeWp374aText(
        project.path / "scene.json",
        Json{{"schema", "pelican.scene"},
             {"version", 1},
             {"scenes",
              {{"main",
                {{"objects",
                  Json::array(
                      {{{"name", "Chair"},
                        {"components",
                         Json::array(
                             {{{"name", "transform"},
                               {"pos", {0.0, 0.0, 0.0}},
                               {"rotation", {0.0, 0.0, 0.0, 1.0}},
                               {"scale", {1.0, 1.0, 1.0}}}})}}})}}}}}}
            .dump(2));
#endif
    std::uint64_t rpc_id = 375200;

    {
        FastModuleContainer modules;
        loadWp374aScene(project.path);
        auto service = makeEditorRuntimeService();
        const auto actor = wp375OpenActor(
            *service, rpc_id, "WP375 committed-target acceptance");
        const auto chair = wp375Components(*service, rpc_id, "Chair");
        commitWp374aEdit(
            *service, rpc_id, actor, wp375SceneRevision(*service, rpc_id),
            Json::array({{{"op", "set_component_value"},
                          {"object_id",
                           chair.at("authoring_object_id")},
                          {"component_slot", "transform"},
                          {"field_path", "/pos/0"},
                          {"value", 7.5}}}));
        wp375Save(*service, rpc_id);
        wp375FinishRuntime(service);
    }

    {
        FastModuleContainer modules;
        loadWp374aScene(project.path);
        auto service = makeEditorRuntimeService();
        const auto reloaded = wp375Components(*service, rpc_id, "Chair");
        REQUIRE(wp375Component(reloaded, "transform")
                    .at("authored_json")
                    .at("pos")
                    .at(0)
                    .get<double>() == Catch::Approx(7.5));
        wp375FinishRuntime(service);
    }
}

TEST_CASE(
    "WP375 production acceptance c contrasts active and unavailable behavior registries",
    "[WP375][rpc][production][mutation-c][behavior][gpu]") {
    setupLogger();
#if !PELICAN_RUNTIME_SHADER_COMPILER
    INFO("WP375 acceptance c compiler-OFF active and pending behavior registry contrast");
#endif
#ifdef PELICAN_WP375_BEHAVIOR_FIXTURE
    Wp374aProjectRoot active_project{"wp375_mutation_c_active"};
    Wp374aProjectRoot pending_project{"wp375_mutation_c_pending"};
    writeWp375BehaviorProject(active_project.path);
    writeWp375BehaviorProject(pending_project.path);
    const std::filesystem::path behavior_dll{
        PELICAN_WP375_BEHAVIOR_FIXTURE};
    std::uint64_t rpc_id = 375300;

    {
        FastModuleContainer modules;
        loadWp374aScene(active_project.path, behavior_dll);
        REQUIRE(configuredGameLogicStatus().loaded);
        auto service = makeEditorRuntimeService();
        const auto actor = wp375OpenActor(
            *service, rpc_id, "WP375 behavior-context acceptance");
        const auto before = wp375Components(*service, rpc_id, "BehaviorHost");
        const auto object_id =
            before.at("authoring_object_id").get<std::uint64_t>();
        const auto before_revision = wp375SceneRevision(*service, rpc_id);

        const auto unknown = finishWp374aEdit(
            *service, rpc_id, actor, before_revision,
            Json::array({{{"op", "add_component"},
                          {"object_id", object_id},
                          {"component_index", 1},
                          {"component",
                           {{"name", "behavior"},
                            {"type", "wp375_no_such_behavior"},
                            {"params", Json::object()}}}}}));
        REQUIRE(unknown.at("status") == "rejected");
        const auto &unknown_error = unknown.at("error");
        REQUIRE(unknown_error.at("code") == "behavior_type_unknown@1");
        REQUIRE(unknown_error.at("payload").at("type") ==
                "wp375_no_such_behavior");
        REQUIRE(unknown_error.at("payload").at("object_id") == object_id);
        REQUIRE(wp375SceneRevision(*service, rpc_id) == before_revision);
        const auto after_unknown =
            wp375Components(*service, rpc_id, "BehaviorHost");
        REQUIRE(after_unknown.at("components") == before.at("components"));
        REQUIRE(GET_MODULE(BehaviorAttachmentArena).snapshot().empty());

        commitWp374aEdit(
            *service, rpc_id, actor, before_revision,
            Json::array({{{"op", "add_component"},
                          {"object_id", object_id},
                          {"component_index", 1},
                          {"component",
                           {{"name", "behavior"},
                            {"type", "wp155_project_behavior"},
                            {"params", {{"label", "wp375"}}}}}}}));

        updateFrameState();
        updateFrameState();
        const auto edited =
            wp375Components(*service, rpc_id, "BehaviorHost");
        const auto &behavior = wp375Component(edited, "behavior");
        REQUIRE_FALSE(behavior.at("pending").get<bool>());
        REQUIRE(behavior.contains("runtime_json"));
        const auto attachments = GET_MODULE(BehaviorAttachmentArena).snapshot();
        REQUIRE(attachments.size() == 1);
        REQUIRE(attachments.front().active);
        REQUIRE_FALSE(attachments.front().pending);
        wp375Save(*service, rpc_id);
        wp375FinishRuntime(service, true);
    }

    {
        FastModuleContainer modules;
        loadWp374aScene(active_project.path, behavior_dll);
        auto service = makeEditorRuntimeService();
        const auto reloaded =
            wp375Components(*service, rpc_id, "BehaviorHost");
        REQUIRE_FALSE(
            wp375Component(reloaded, "behavior").at("pending").get<bool>());
        const auto attachments = GET_MODULE(BehaviorAttachmentArena).snapshot();
        REQUIRE(attachments.size() == 1);
        REQUIRE(attachments.front().active);
        wp375FinishRuntime(service, true);
    }

    {
        FastModuleContainer modules;
        loadWp374aScene(pending_project.path);
        REQUIRE_FALSE(configuredGameLogicStatus().loaded);
        auto service = makeEditorRuntimeService();
        const auto actor = wp375OpenActor(
            *service, rpc_id, "WP375 unavailable-behavior acceptance");
        const auto object =
            wp375Components(*service, rpc_id, "BehaviorHost");
        commitWp374aEdit(
            *service, rpc_id, actor, wp375SceneRevision(*service, rpc_id),
            Json::array({{{"op", "add_component"},
                          {"object_id",
                           object.at("authoring_object_id")},
                          {"component_index", 1},
                          {"component",
                           {{"name", "behavior"},
                            {"type", "wp155_project_behavior"},
                            {"params", {{"label", "wp375-pending"}}}}}}}));

        const auto pending =
            wp375Components(*service, rpc_id, "BehaviorHost");
        REQUIRE(wp375Component(pending, "behavior").at("pending").get<bool>());
        const auto attachments = GET_MODULE(BehaviorAttachmentArena).snapshot();
        REQUIRE(attachments.size() == 1);
        REQUIRE(attachments.front().pending);
        REQUIRE_FALSE(attachments.front().active);
        wp375Save(*service, rpc_id);
        wp375FinishRuntime(service, true);
    }

    {
        FastModuleContainer modules;
        loadWp374aScene(pending_project.path, behavior_dll);
        REQUIRE(configuredGameLogicStatus().loaded);
        auto service = makeEditorRuntimeService();
        const auto reloaded =
            wp375Components(*service, rpc_id, "BehaviorHost");
        REQUIRE_FALSE(
            wp375Component(reloaded, "behavior").at("pending").get<bool>());
        const auto attachments = GET_MODULE(BehaviorAttachmentArena).snapshot();
        REQUIRE(attachments.size() == 1);
        REQUIRE_FALSE(attachments.front().pending);
        REQUIRE(attachments.front().active);
        wp375FinishRuntime(service, true);
    }
#else
    FAIL("WP375 production behavior fixture was not composed");
#endif
}

TEST_CASE(
    "WP375 production acceptance d publishes spawn and destroy renderer participants at commit",
    "[WP375][rpc][production][mutation-d][renderer][gpu]") {
    setupLogger();
    requireWp375MissingRendererParticipant();
#if !PELICAN_RUNTIME_SHADER_COMPILER
    return;
#endif
    Wp374aProjectRoot project{"wp375_mutation_d"};
    writeWp374aProject(project.path, std::nullopt, false, false, true);
    std::uint64_t rpc_id = 375400;

    const auto require_renderer_instance =
        [&](EditorCommandService &service) -> ModelInstanceId {
        const auto object = wp375Components(service, rpc_id, "SpawnedModel");
        const auto &entity = object.at("entity_id");
        REQUIRE(entity.is_object());
        const GameObjectId id{
            entity.at("index").get<std::uint32_t>(),
            entity.at("gen").get<std::uint32_t>()};
        const auto *model = GET_MODULE(ECSCore)
                                .getTemplatePublicModule()
                                .tryComponent<SimpleModelViewComponent>(id);
        REQUIRE(model != nullptr);
        REQUIRE(model->model_instance_id.has_value());
        const auto instance = *model->model_instance_id;
        REQUIRE(GET_MODULE(PolygonInstanceContainer)
                    .isModelInstanceAlive(instance));
        return instance;
    };

    {
        FastModuleContainer modules;
        loadWp374aScene(project.path);
        auto service = makeEditorRuntimeService();
        const auto actor = wp375OpenActor(
            *service, rpc_id, "WP375 renderer-participant acceptance");
        commitWp374aEdit(
            *service, rpc_id, actor, wp375SceneRevision(*service, rpc_id),
            Json::array(
                {{{"op", "spawn"},
                  {"scene_id", "main"},
                  {"declaration_index", 1},
                  {"object",
                   {{"name", "SpawnedModel"},
                    {"components",
                     Json::array(
                         {{{"name", "transform"},
                           {"pos", {2.0, 0.0, 0.0}},
                           {"rotation", {0.0, 0.0, 0.0, 1.0}},
                           {"scale", {1.0, 1.0, 1.0}}},
                          {{"name", "simplemodelview"},
                           {"model", "chair"}}})}}}}}));
        // No ECS frame update is allowed here: this observes the adapter's
        // staged publication at the edit commit boundary.
        (void)require_renderer_instance(*service);
        wp375Save(*service, rpc_id);
        wp375FinishRuntime(service);
    }

    {
        FastModuleContainer modules;
        loadWp374aScene(project.path);
        auto service = makeEditorRuntimeService();
        // SceneLoader publishes the reloaded component as dirty and its
        // production ECS system materializes that baseline on the next tick.
        // Only the subsequent destroy is an editor adapter contrast here.
        GET_MODULE(ECSCore).update();
        const auto instance = require_renderer_instance(*service);
        const auto object = wp375Components(*service, rpc_id, "SpawnedModel");
        const auto actor = wp375OpenActor(
            *service, rpc_id, "WP375 renderer-destroy acceptance");
        commitWp374aEdit(
            *service, rpc_id, actor, wp375SceneRevision(*service, rpc_id),
            Json::array({{{"op", "destroy"},
                          {"object_id",
                           object.at("authoring_object_id")}}}));
        REQUIRE_FALSE(GET_MODULE(PolygonInstanceContainer)
                          .isModelInstanceAlive(instance));
        wp375FinishRuntime(service);
    }
}
#endif

#ifdef PELICAN_WP380_BEHAVIOR_FIXTURE
namespace {

using Wp380Probe = Wp380Fixture::ProbeV1;

Json wp380Scene(std::int32_t tag, std::string_view scene_name,
                std::string_view pending_scene, bool fail_after_effects) {
    Json objects = Json::array();
    for (std::int32_t index = 0; index < tag; ++index) {
        Json components = Json::array({
            {{"name", "transform"},
             {"pos", {tag * 10.0, index * 2.0, 0.0}},
             {"rotation", {0.0, 0.0, 0.0, 1.0}},
             {"scale", {1.0, 1.0, 1.0}}},
            {{"name", "simplemodelview"}, {"model", "chair"}},
            {{"name", "collider"},
             {"shape", "box"},
             {"half_extents", {0.25, 0.25, 0.25}},
             {"trigger", true}},
        });
        if (index == 0) {
            components.push_back(
                {{"name", "behavior"},
                 {"type", "wp380_scene_transaction_behavior"},
                 {"params",
                  {{"scene_tag", tag},
                   {"fail_after_effects", fail_after_effects},
                   {"pending_scene", pending_scene}}}});
        }
        objects.push_back(
            {{"name", "Actor" + std::to_string(tag) + "_" +
                          std::to_string(index)},
             {"components", std::move(components)}});
    }
    objects.push_back(
        {{"name", "Camera" + std::to_string(tag)},
         {"components",
          Json::array({
              {{"name", "transform"},
               {"pos", {0.0, 0.0, -4.0}},
               {"rotation", {0.0, 0.0, 0.0, 1.0}},
               {"scale", {1.0, 1.0, 1.0}}},
              {{"name", "camera"},
               {"type", "perspective"},
               {"yfov", 0.3 + tag * 0.1},
               {"znear", 0.1},
               {"zfar", 100.0}},
          })}});
    objects.push_back(
        {{"name", "Light" + std::to_string(tag)},
         {"components",
          Json::array({
              {{"name", "light"},
               {"type", "directional"},
               {"direction", {0.0, -1.0, 0.0}},
               {"intensity", tag + 0.25},
               {"color", {1.0, 1.0, 1.0}}},
          })}});
    return Json{{"objects", std::move(objects)}};
}

Json wp380Document(std::int32_t tag, std::string_view scene_name,
                   std::string_view pending_scene,
                   bool fail_after_effects) {
    return Json{
        {"schema", "pelican.scene"},
        {"version", 1},
        {"scenes",
         {{std::string{scene_name},
           wp380Scene(tag, scene_name, pending_scene,
                      fail_after_effects)},
          {std::string{pending_scene}, {{"objects", Json::array()}}}}},
    };
}

struct Wp380FixtureExports {
    PelicanWp380ReadProbeFn read_probe = nullptr;
    PelicanWp380ReadExpectedFn read_expected = nullptr;
};

Wp380FixtureExports wp380Exports(const std::filesystem::path &path) {
    const auto status = configuredGameLogicStatus();
    if (!status.loaded || status.source != path ||
        status.loaded_copy.empty()) {
        throw std::runtime_error(
            "WP380 behavior fixture is not the active game logic module");
    }
    auto module = GetModuleHandleW(status.loaded_copy.c_str());
    if (module == nullptr) {
        module = GetModuleHandleW(status.loaded_copy.filename().c_str());
    }
    if (module == nullptr) {
        throw std::runtime_error(
            "WP380 behavior fixture module is not loaded");
    }
    Wp380FixtureExports result{
        .read_probe = reinterpret_cast<PelicanWp380ReadProbeFn>(
            GetProcAddress(module, "pelican_wp380_fixture_read_probe")),
        .read_expected = reinterpret_cast<PelicanWp380ReadExpectedFn>(
            GetProcAddress(module, "pelican_wp380_fixture_read_expected")),
    };
    if (result.read_probe == nullptr || result.read_expected == nullptr) {
        throw std::runtime_error(
            "WP380 behavior fixture probe exports are missing");
    }
    return result;
}

void requireWp380StateFields(const Wp380Probe &actual,
                             const Wp380Probe &expected) {
    REQUIRE(actual.abi == Wp380Fixture::probe_abi_v1);
    REQUIRE(actual.scene_tag == expected.scene_tag);
    REQUIRE(actual.light_name_tag == expected.light_name_tag);
    REQUIRE(actual.light_intensity ==
            Catch::Approx(expected.light_intensity));
    REQUIRE(actual.camera_yfov == Catch::Approx(expected.camera_yfov));
    REQUIRE(actual.ecs_x == Catch::Approx(expected.ecs_x));
    REQUIRE(actual.collider_count == expected.collider_count);
    REQUIRE(actual.active_trigger_pair_count ==
            expected.active_trigger_pair_count);
    REQUIRE(actual.renderer_instance_count ==
            expected.renderer_instance_count);
    REQUIRE(actual.renderer_ordinal == expected.renderer_ordinal);
    REQUIRE(actual.binding_count == expected.binding_count);
    REQUIRE(actual.behavior_count == expected.behavior_count);
}

void requireWp380Observed(const Wp380Probe &actual,
                          const Wp380Probe &expected) {
    requireWp380StateFields(actual, expected);
    REQUIRE(actual.random_value ==
            Catch::Approx(expected.random_value).epsilon(1e-15));
    REQUIRE(actual.pending_request_issued ==
            expected.pending_request_issued);
}

void requireWp380CommittedPhaseTrace(const SceneLoadCommitted &committed) {
    constexpr std::array expected{
        SceneLoadPhase::prepare,
        SceneLoadPhase::early_publish,
        SceneLoadPhase::publish,
        SceneLoadPhase::activate,
        SceneLoadPhase::commit_metadata,
        SceneLoadPhase::retire,
        SceneLoadPhase::flush,
        SceneLoadPhase::finish,
    };
    REQUIRE(std::ranges::equal(committed.phase_trace.view(), expected));
    REQUIRE_FALSE(committed.phase_trace.overflow);
}

struct Wp380RuntimeCleanup {
    ~Wp380RuntimeCleanup() {
        try {
            if (auto *scene = FastModuleContainer::tryGet<SceneLoader>())
                scene->drainRetirementQueue();
            if (auto *arena =
                    FastModuleContainer::tryGet<BehaviorAttachmentArena>())
                arena->deactivateAll();
            shutdownConfiguredGameLogic();
            if (auto *vulkan =
                    FastModuleContainer::tryGet<VulkanManageCore>())
                vulkan->waitIdle();
        } catch (...) {
        }
    }
};

TEST_CASE(
    "WP380d empty scene commits the complete executor trace without overflow",
    "[WP380d][transaction][trace][gpu]") {
    setupLogger();
    Wp374aProjectRoot project{"wp380d_empty_scene_trace"};
    writeWp374aProject(project.path, std::nullopt, false, false, true);
    const auto a_document = wp380Document(1, "A", "A_pending", false);
    const Json empty_document{
        {"schema", "pelican.scene"},
        {"version", 1},
        {"scenes", {{"empty", {{"objects", Json::array()}}}}},
    };
    writeWp374aText(project.path / "scene.json", a_document.dump(2));

    FastModuleContainer modules;
    Wp380RuntimeCleanup cleanup;
    loadWp374aScene(project.path, std::nullopt, "A", 380);
    auto &config = GET_MODULE(ProjectBasicConfig);
    auto &loader = GET_MODULE(SceneLoader);
    std::optional<SceneLoadResult> result;
    config.importSceneDocument(
        empty_document.dump(), [&](SceneLoadRequest &&request) {
            request.scene_id = "empty";
            auto loaded = loader.load(std::move(request));
            result = loaded;
            return loaded;
        });

    REQUIRE(result.has_value());
    const auto *committed = std::get_if<SceneLoadCommitted>(&*result);
    REQUIRE(committed != nullptr);
    requireWp380CommittedPhaseTrace(*committed);
    REQUIRE(loader.currentScene() == "empty");
    loader.drainRetirementQueue();
}

std::string_view wp380MutationId(std::string_view owner,
                                 std::string_view field) {
    const auto result = test_support::manifestMutationId(owner, field);
    if (result.empty()) {
        throw std::logic_error("WP380 manifest mutation row is missing");
    }
    return result;
}

bool wp380HasLiveTransformX(float expected) {
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    for (const auto entity : ecs.liveEntities()) {
        if (const auto *transform =
                ecs.tryComponent<TransformComponent>(entity);
            transform != nullptr && transform->pos.x == expected) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE(
    "WP380 A B C D uses fixture expectations rollback and no-drift control",
    "[WP380][transaction][production][fixture][gpu]") {
    setupLogger();
    Wp374aProjectRoot project{"wp380_scene_transaction"};
    writeWp374aProject(project.path, std::nullopt, false, false, true);
    const auto a_document = wp380Document(1, "A", "A_pending", false);
    const auto b_document = wp380Document(2, "B", "D", false);
    const auto c_document = wp380Document(3, "C", "E", true);
    const auto d_document = wp380Document(4, "D", "F", false);
    writeWp374aText(project.path / "scene.json", a_document.dump(2));

    const std::filesystem::path fixture_path{
        PELICAN_WP380_BEHAVIOR_FIXTURE};
    Wp380Probe control_d{};
    DeterministicRng::PreparedSequence control_rng_d{};
    std::optional<SceneId> control_pending_d;

    // Control sequence omits failed C. D must have the same semantic state as
    // the A/B/C(fail)/D sequence below; authority alone deliberately has no
    // equality requirement because failed candidates consume their epoch.
    {
        FastModuleContainer control_modules;
        Wp380RuntimeCleanup control_cleanup;
        loadWp374aScene(project.path, fixture_path, "A", 380);
        auto &control_config = GET_MODULE(ProjectBasicConfig);
        auto &control_loader = GET_MODULE(SceneLoader);
        internal::clearPendingEvents();
        const auto import_control = [&](const Json &document,
                                        std::string scene_id) {
            control_config.importSceneDocument(
                document.dump(), [&](SceneLoadRequest &&request) {
                    request.scene_id = std::move(scene_id);
                    return control_loader.load(std::move(request));
                });
        };
        import_control(b_document, "B");
        import_control(d_document, "D");

        const auto control_fixture = wp380Exports(fixture_path);
        Wp380Probe expected_d{};
        REQUIRE(control_fixture.read_expected(4, &expected_d));
        REQUIRE(control_fixture.read_probe(4, &control_d));
        requireWp380Observed(control_d, expected_d);
        control_rng_d = GET_MODULE(DeterministicRng).prepareSequence();
        control_pending_d = control_loader.pendingSceneForTesting();
        REQUIRE(control_pending_d == std::optional<SceneId>{"F"});
        REQUIRE(test_support::authoring(control_config.sceneDocument())
                    .rawJson() == d_document);
        REQUIRE(control_loader.activeAuthority().scene_epoch == 3);
        control_loader.drainRetirementQueue();
    }

    FastModuleContainer modules;
    Wp380RuntimeCleanup cleanup;
    loadWp374aScene(project.path, fixture_path, "A", 380);
    const auto fixture = wp380Exports(fixture_path);
    auto &config = GET_MODULE(ProjectBasicConfig);
    auto &loader = GET_MODULE(SceneLoader);
    auto &events = internal::getEventRegisterer();

    Wp380Probe expected_a{};
    Wp380Probe observed_a{};
    REQUIRE(fixture.read_expected(1, &expected_a));
    REQUIRE(fixture.read_probe(1, &observed_a));
    requireWp380Observed(observed_a, expected_a);
    internal::clearPendingEvents();

    std::optional<SceneLoadResult> b_result;
    config.importSceneDocument(
        b_document.dump(), [&](SceneLoadRequest &&request) {
            request.scene_id = "B";
            auto result = loader.load(std::move(request));
            b_result = result;
            return result;
        });
    REQUIRE(b_result.has_value());
    REQUIRE(std::holds_alternative<SceneLoadCommitted>(*b_result));
    requireWp380CommittedPhaseTrace(
        std::get<SceneLoadCommitted>(*b_result));

    Wp380Probe expected_b{};
    Wp380Probe observed_b{};
    REQUIRE(fixture.read_expected(2, &expected_b));
    REQUIRE(fixture.read_probe(2, &observed_b));
    requireWp380Observed(observed_b, expected_b);

    Wp380Probe committed_b{.scene_tag = 2};
    pelican_wp380_host_read_probe(&committed_b);
    requireWp380StateFields(committed_b, expected_b);
    REQUIRE(loader.currentScene() == "B");
    REQUIRE(loader.pendingSceneForTesting() ==
            std::optional<SceneId>{"D"});
    const auto authority_b = loader.activeAuthority();
    REQUIRE(authority_b.scene_epoch == 2);
    REQUIRE(test_support::authoring(config.sceneDocument()).rawJson() ==
            b_document);

    const auto rng_b = GET_MODULE(DeterministicRng).prepareSequence();
    const auto event_b = events.isolationSnapshot();
    REQUIRE(event_b.pending.size() == 2);
    const auto pending_b = events.pendingEventsForTesting();
    const auto fixture_event = std::find_if(
        pending_b.begin(), pending_b.end(), [](const auto &event) {
            return event.type ==
                   std::type_index{typeid(Wp380Fixture::Event)};
        });
    REQUIRE(fixture_event != pending_b.end());
    const auto fixture_payload =
        std::static_pointer_cast<const Wp380Fixture::Event>(
            fixture_event->payload);
    REQUIRE(fixture_payload->scene_tag == 2);
    REQUIRE(fixture_payload->payload_sentinel == 2380);
    const auto loaded_event = std::find_if(
        pending_b.begin(), pending_b.end(), [](const auto &event) {
            return event.type == std::type_index{typeid(SceneLoaded)};
        });
    REQUIRE(loaded_event != pending_b.end());
    REQUIRE(std::static_pointer_cast<const SceneLoaded>(
                loaded_event->payload)
                ->scene_name == "B");

    std::optional<SceneLoadResult> c_result;
    REQUIRE_THROWS_WITH(
        config.importSceneDocument(
            c_document.dump(), [&](SceneLoadRequest &&request) {
                request.scene_id = "C";
                auto result = loader.load(std::move(request));
                c_result = result;
                return result;
            }),
        ContainsSubstring("WP380 fixture activation failure"));
    REQUIRE(c_result.has_value());
    const auto *failure = std::get_if<PreCommitFailure>(&*c_result);
    REQUIRE(failure != nullptr);
    REQUIRE(failure->phase == SceneLoadPhase::activate);

    Wp380Probe expected_c{};
    Wp380Probe observed_c{};
    REQUIRE(fixture.read_expected(3, &expected_c));
    REQUIRE(fixture.read_probe(3, &observed_c));
    requireWp380Observed(observed_c, expected_c);

    Wp380Probe after_c{.scene_tag = 2};
    pelican_wp380_host_read_probe(&after_c);
    requireWp380StateFields(after_c, expected_b);
    REQUIRE(test_support::authoring(config.sceneDocument()).rawJson() ==
            b_document);
    REQUIRE(loader.currentScene() == "B");
    REQUIRE(loader.activeAuthority() == authority_b);
    REQUIRE(loader.pendingSceneForTesting() ==
            std::optional<SceneId>{"D"});
    const auto rng_after_c = GET_MODULE(DeterministicRng).prepareSequence();
    REQUIRE(rng_after_c.current_seed == rng_b.current_seed);
    REQUIRE(rng_after_c.state == rng_b.state);
    REQUIRE(rng_after_c.increment == rng_b.increment);
    const auto event_after_c = events.isolationSnapshot();
    REQUIRE(event_after_c.pending == event_b.pending);
    REQUIRE(event_after_c.deliver_now == event_b.deliver_now);
    REQUIRE(event_after_c.payload_load_calls == event_b.payload_load_calls);

    REQUIRE(failure->exception != nullptr);

    std::optional<SceneLoadResult> d_result;
    config.importSceneDocument(
        d_document.dump(), [&](SceneLoadRequest &&request) {
            request.scene_id = "D";
            auto result = loader.load(std::move(request));
            d_result = result;
            return result;
        });
    REQUIRE(d_result.has_value());
    const auto *committed_d = std::get_if<SceneLoadCommitted>(&*d_result);
    REQUIRE(committed_d != nullptr);
    requireWp380CommittedPhaseTrace(*committed_d);
    // C consumed epoch 3 even though it rolled back.  D therefore proves the
    // failed candidate authority cannot be reused (same-process ABA).
    REQUIRE(committed_d->active_authority.scene_epoch == 4);

    Wp380Probe expected_d{};
    Wp380Probe observed_d{};
    REQUIRE(fixture.read_expected(4, &expected_d));
    REQUIRE(fixture.read_probe(4, &observed_d));
    requireWp380Observed(observed_d, expected_d);

    Wp380Probe committed_d_probe{.scene_tag = 4};
    pelican_wp380_host_read_probe(&committed_d_probe);
    requireWp380StateFields(committed_d_probe, expected_d);
    // This comparison is against a separately executed A/B/D control, not
    // against the implementation's own B or D measurements.
    requireWp380StateFields(committed_d_probe, control_d);
    REQUIRE(observed_d.random_value ==
            Catch::Approx(control_d.random_value).epsilon(1e-15));
    REQUIRE(loader.pendingSceneForTesting() == control_pending_d);
    const auto rng_d = GET_MODULE(DeterministicRng).prepareSequence();
    REQUIRE(rng_d.current_seed == control_rng_d.current_seed);
    REQUIRE(rng_d.state == control_rng_d.state);
    REQUIRE(rng_d.increment == control_rng_d.increment);
    REQUIRE(test_support::authoring(config.sceneDocument()).rawJson() ==
            d_document);
    loader.drainRetirementQueue();
}

TEST_CASE(
    "WP380c every real participant mutation changes its manifest field",
    "[WP380c][transaction][manifest][mutation][gpu]") {
    enum class MutationScenario {
        metadata_document,
        metadata_projection,
        transient_models,
        ecs_liveness,
        light_names,
        behavior_attachments,
        event_pending,
        rng_current_seed,
        rng_state,
        pending_scene,
    };
    struct MutationCase {
        std::string_view owner;
        std::string_view field;
        SceneLoadFaultPhase phase;
        MutationScenario scenario;
    };
    constexpr std::array cases{
        MutationCase{"metadata", "document", SceneLoadFaultPhase::commit,
                     MutationScenario::metadata_document},
        MutationCase{"metadata", "projection", SceneLoadFaultPhase::prepare,
                     MutationScenario::metadata_projection},
        MutationCase{"scene_loader", "transient_models",
                     SceneLoadFaultPhase::retire,
                     MutationScenario::transient_models},
        MutationCase{"ecs", "entity_liveness", SceneLoadFaultPhase::publish,
                     MutationScenario::ecs_liveness},
        MutationCase{"light", "directional_names",
                     SceneLoadFaultPhase::publish,
                     MutationScenario::light_names},
        MutationCase{"behavior_arena", "scene_attachments",
                     SceneLoadFaultPhase::activate,
                     MutationScenario::behavior_attachments},
        MutationCase{"event_outbox", "pending_events",
                     SceneLoadFaultPhase::flush,
                     MutationScenario::event_pending},
        MutationCase{"rng_sequence", "current_seed",
                     SceneLoadFaultPhase::flush,
                     MutationScenario::rng_current_seed},
        MutationCase{"rng_sequence", "state", SceneLoadFaultPhase::flush,
                     MutationScenario::rng_state},
        MutationCase{"pending_scene", "request", SceneLoadFaultPhase::flush,
                     MutationScenario::pending_scene},
    };
    STATIC_REQUIRE(cases.size() == 10);

    for (const auto &entry : cases) {
        DYNAMIC_SECTION(entry.owner << '.' << entry.field) {
            setupLogger();
            Wp374aProjectRoot project{"wp380c_manifest_mutation"};
            writeWp374aProject(project.path, std::nullopt, false, false, true);
            const auto a_document = wp380Document(1, "A", "A_pending", false);
            const auto b_document = wp380Document(2, "B", "D", false);
            const auto c_document = wp380Document(3, "C", "E", true);
            writeWp374aText(project.path / "scene.json", a_document.dump(2));

            const std::filesystem::path fixture_path{
                PELICAN_WP380_BEHAVIOR_FIXTURE};
            FastModuleContainer modules;
            Wp380RuntimeCleanup cleanup;
            loadWp374aScene(project.path, fixture_path, "A", 380);
            const auto fixture = wp380Exports(fixture_path);
            auto &config = GET_MODULE(ProjectBasicConfig);
            auto &loader = GET_MODULE(SceneLoader);
            auto &events = internal::getEventRegisterer();
            internal::clearPendingEvents();

            const auto mutation = wp380MutationId(entry.owner, entry.field);
            test_support::ManifestSceneLoadFault injector{mutation};
            struct InjectorReset {
                SceneLoader &loader;
                ~InjectorReset() {
                    test_support::setSceneLoadFaultInjectorForTesting(
                        loader, nullptr);
                }
            } reset{loader};
            const auto enable = [&] {
                injector.setEnabled(true);
                test_support::setSceneLoadFaultInjectorForTesting(
                    loader, &injector);
            };
            const auto import = [&](const Json &document,
                                    std::string scene_id) {
                std::optional<SceneLoadResult> result;
                config.importSceneDocument(
                    document.dump(), [&](SceneLoadRequest &&request) {
                        request.scene_id = std::move(scene_id);
                        auto loaded = loader.load(std::move(request));
                        result = loaded;
                        return loaded;
                    });
                REQUIRE(result.has_value());
                return *result;
            };

            if (entry.scenario == MutationScenario::metadata_projection) {
                const auto generation =
                    config.sceneProjectionState().resolverGeneration();
                enable();
                REQUIRE_THROWS_WITH(
                    import(b_document, "B"),
                    ContainsSubstring("injected scene metadata prepare drop"));
                REQUIRE(config.sceneProjectionState().resolverGeneration() ==
                        generation);
                REQUIRE(loader.currentScene() == "A");
            } else if (entry.scenario ==
                       MutationScenario::transient_models) {
                auto &geometry = GET_MODULE(VertBufContainer);
                const auto baseline =
                    geometry.allocatedIndexCountForTesting();
                (void)loader.loadTransientGltf("models/chair.glb",
                                               std::nullopt);
                REQUIRE(geometry.allocatedIndexCountForTesting() > baseline);
                enable();
                const auto result = import(b_document, "B");
                REQUIRE(std::holds_alternative<SceneLoadCommitted>(result));
                loader.drainRetirementQueue();
                REQUIRE(geometry.allocatedIndexCountForTesting() > baseline);
            } else if (entry.scenario == MutationScenario::light_names) {
                enable();
                const auto result = import(b_document, "B");
                REQUIRE(std::holds_alternative<SceneLoadCommitted>(result));
                Wp380Probe observed{};
                REQUIRE(fixture.read_probe(2, &observed));
                REQUIRE(observed.light_name_tag == 1);
            } else if (entry.scenario ==
                       MutationScenario::behavior_attachments) {
                enable();
                const auto result = import(b_document, "B");
                REQUIRE(std::holds_alternative<SceneLoadCommitted>(result));
                Wp380Probe absent{};
                REQUIRE_FALSE(fixture.read_probe(2, &absent));
            } else {
                const auto b_result = import(b_document, "B");
                REQUIRE(std::holds_alternative<SceneLoadCommitted>(b_result));
                const auto rng_before =
                    GET_MODULE(DeterministicRng).prepareSequence();
                const auto events_before = events.isolationSnapshot();
                enable();

                std::optional<SceneLoadResult> c_result;
                REQUIRE_THROWS_WITH(
                    config.importSceneDocument(
                        c_document.dump(), [&](SceneLoadRequest &&request) {
                            request.scene_id = "C";
                            auto loaded = loader.load(std::move(request));
                            c_result = loaded;
                            return loaded;
                        }),
                    ContainsSubstring("WP380 fixture activation failure"));
                REQUIRE(c_result.has_value());
                REQUIRE(std::holds_alternative<PreCommitFailure>(*c_result));

                if (entry.scenario ==
                    MutationScenario::metadata_document) {
                    REQUIRE(test_support::authoring(config.sceneDocument())
                                .rawJson() == c_document);
                } else if (entry.scenario ==
                           MutationScenario::ecs_liveness) {
                    REQUIRE(wp380HasLiveTransformX(30.0F));
                } else if (entry.scenario ==
                           MutationScenario::event_pending) {
                    REQUIRE(events.isolationSnapshot().pending !=
                            events_before.pending);
                } else if (entry.scenario ==
                           MutationScenario::rng_current_seed) {
                    REQUIRE(GET_MODULE(DeterministicRng)
                                .prepareSequence()
                                .current_seed != rng_before.current_seed);
                } else if (entry.scenario ==
                           MutationScenario::rng_state) {
                    REQUIRE(GET_MODULE(DeterministicRng)
                                .prepareSequence()
                                .state != rng_before.state);
                } else {
                    REQUIRE(entry.scenario ==
                            MutationScenario::pending_scene);
                    REQUIRE(loader.pendingSceneForTesting() ==
                            std::optional<SceneId>{"E"});
                }
            }
            REQUIRE(injector.saw(entry.phase, entry.owner));
        }
    }
}

TEST_CASE(
    "WP380c precommit model failure reports release error and restores all GPU ownership columns",
    "[WP380c][transaction][model-release][gpu]") {
    setupLogger();
    Wp374aProjectRoot project{"wp380c_model_release"};
    writeWp374aProject(project.path, std::nullopt, false, false, true);
    TestGltfFragmentFixture::writeGlb(project.path / "models" /
                                      "textured.glb");
    TestMorphFixture::writeGlb(project.path / "models" / "skinned.glb",
                               {.skinned = true, .target_count = 0});
    TestMorphFixture::writeGlb(project.path / "models" / "morph.glb");
    writeWp374aText(
        project.path / "retry_bindings.json",
        R"json({"schema":"pelican.material_bindings","version":1,"model":"models/morph.glb","bindings":[{"usd_path":"/Retry/Missing","mesh":999,"primitive":0,"material":"missing","routing":{"alpha_mode":"opaque","double_sided":false}}]})json");
    writeWp374aText(
        project.path / "assets.json",
        Json{{"schema", "pelican.asset_data"},
             {"version", 1},
             {"models",
              Json::array(
                  {{{"name", "chair"},
                    {"path", "models/textured.glb"}},
                   {{"name", "skinned"},
                    {"path", "models/skinned.glb"}},
                   {{"name", "failing"},
                    {"path", "models/morph.glb"},
                    {"material_bindings", "retry_bindings.json"}}})},
             {"textures", Json::array()}}
            .dump(2));

    FastModuleContainer modules;
    Wp380RuntimeCleanup cleanup;
    (void)configureWp374aRuntime(project.path, 380);
    TestSupport::requireVulkanDevice("WP380c model release acceptance");
    GET_MODULE(ECSPredefinedRegistration).reg();
    (void)GET_MODULE(StandardMaterialResource);
    auto &geometry = GET_MODULE(VertBufContainer);
    auto &materials = GET_MODULE(MaterialContainer);
    const auto baseline_indices = geometry.allocatedIndexCountForTesting();
    const auto baseline_static =
        geometry.allocatedVertexCountForTesting(false);
    const auto baseline_skinned =
        geometry.allocatedVertexCountForTesting(true);
    const auto baseline_morph =
        geometry.allocatedMorphDeltaCountForTesting();
    const auto baseline_materials = materials.materialCountForTesting();
    const auto baseline_textures = materials.textureCountForTesting();

    test_support::setMorphMetadataReleaseFailureForTesting(geometry, true);
    auto &config = GET_MODULE(ProjectBasicConfig);
    const auto result = GET_MODULE(SceneLoader).load(
        SceneLoadRequest::borrowed("main", config.sceneProjectionState(),
                                   SceneLoadRoot::startup));
    test_support::setMorphMetadataReleaseFailureForTesting(geometry, false);

    const auto *failure = std::get_if<PreCommitFailure>(&result);
    REQUIRE(failure != nullptr);
    REQUIRE(failure->phase == SceneLoadPhase::prepare);
    REQUIRE(failure->error.find("targets missing GLB mesh 999 primitive 0") !=
            std::string::npos);
    REQUIRE(failure->release_errors ==
            std::vector<std::string>{"vertbuf.morph_delta"});
    REQUIRE_FALSE(FastModuleContainer::isInitialized<ModelAssetContainer>());
    REQUIRE(geometry.allocatedIndexCountForTesting() == baseline_indices);
    REQUIRE(geometry.allocatedVertexCountForTesting(false) == baseline_static);
    REQUIRE(geometry.allocatedVertexCountForTesting(true) == baseline_skinned);
    REQUIRE(geometry.allocatedMorphDeltaCountForTesting() == baseline_morph);
    REQUIRE(materials.materialCountForTesting() == baseline_materials);
    REQUIRE(materials.textureCountForTesting() == baseline_textures);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE(
    "WP380c production pending RPC and import roots reach the executor",
    "[WP380c][transaction][roots][gpu]") {
    enum class RootCase {
        pending,
        rpc,
        import_v1,
        import_v2,
    };
    constexpr std::array cases{
        RootCase::pending,
#if PELICAN_WITH_RPC
        RootCase::rpc,
#endif
        RootCase::import_v1,
        RootCase::import_v2,
    };

    for (const auto root_case : cases) {
        const auto section_name =
            root_case == RootCase::pending
                ? "pending"
                : root_case == RootCase::rpc
                      ? "rpc"
                      : root_case == RootCase::import_v1 ? "import_v1"
                                                         : "import_v2";
        DYNAMIC_SECTION(section_name) {
        setupLogger();
        Wp374aProjectRoot project{"wp380c_root"};
        writeWp374aProject(project.path, std::nullopt, false, false, true);
        const auto a_document = wp380Document(1, "A", "A_pending", false);
        const auto b_document = wp380Document(2, "B", "B_pending", false);
        writeWp374aText(project.path / "scene.json", a_document.dump(2));

        FastModuleContainer modules;
        Wp380RuntimeCleanup cleanup;
        loadWp374aScene(project.path, std::nullopt, "A", 380);
        auto &loader = GET_MODULE(SceneLoader);
        auto &config = GET_MODULE(ProjectBasicConfig);

        SceneLoadRoot root = SceneLoadRoot::pending;
        std::string_view owner = "root.pending";
        std::string_view field = "apply";
        if (root_case == RootCase::rpc) {
            root = SceneLoadRoot::rpc;
            owner = "root.rpc";
            field = "direct_load";
        } else if (root_case == RootCase::import_v1) {
            root = SceneLoadRoot::import_unversioned;
            owner = "root.import.unversioned";
            field = "callback";
        } else if (root_case == RootCase::import_v2) {
            root = SceneLoadRoot::import_versioned;
            owner = "root.import.versioned";
            field = "callback";
        }
        INFO("production root=" << owner);
        test_support::ManifestSceneLoadFault injector{
            wp380MutationId(owner, field)};
        test_support::setSceneLoadFaultInjectorForTesting(loader, &injector);

        if (root_case == RootCase::pending) {
            injector.setEnabled(false);
            loader.requestLoad("A_pending");
            REQUIRE_NOTHROW(updateFrameState());
            REQUIRE(loader.currentScene() == "A_pending");
            const auto *const resolved_pending =
                config.resolvedScene().findScene("A_pending");
            REQUIRE(resolved_pending != nullptr);
            REQUIRE(resolved_pending->objects.empty());

            injector.resetCalls();
            injector.setEnabled(true);
            loader.requestLoad("A_pending");
            REQUIRE_THROWS_WITH(
                updateFrameState(),
                ContainsSubstring("injected scene-load root disconnect"));
            REQUIRE(loader.pendingSceneForTesting() ==
                    std::optional<SceneId>{"A_pending"});
        } else if (root_case == RootCase::rpc) {
#if PELICAN_WITH_RPC
            const auto request =
                Json{{"jsonrpc", "2.0"},
                     {"id", 3803},
                     {"method", "load_scene"},
                     {"params", {{"name", "A_pending"}}}}
                    .dump() +
                "\n";
            injector.setEnabled(false);
            std::istringstream input{request};
            std::ostringstream output;
            runEngineRpcServer(input, output);
            const auto response = Json::parse(output.str());
            REQUIRE(response.contains("result"));
            REQUIRE(loader.currentScene() == "A_pending");
            const auto *const resolved_pending =
                config.resolvedScene().findScene("A_pending");
            REQUIRE(resolved_pending != nullptr);
            REQUIRE(resolved_pending->objects.empty());

            injector.resetCalls();
            injector.setEnabled(true);
            std::istringstream failing_input{request};
            std::ostringstream failing_output;
            runEngineRpcServer(failing_input, failing_output);
            const auto failing_response = Json::parse(failing_output.str());
            REQUIRE(failing_response.contains("error"));
            REQUIRE(failing_response.at("error").dump().find(
                        "injected scene-load root disconnect") !=
                    std::string::npos);
#endif
        } else {
            const auto version = root_case == RootCase::import_v1 ? 1U : 2U;
            injector.setEnabled(false);
            REQUIRE_NOTHROW(config.importSceneDocument(
                b_document.dump(), [&](SceneLoadRequest &&request) {
                    request.scene_id = "B";
                    return loader.load(std::move(request));
                }, version));
            REQUIRE(loader.currentScene() == "B");
            const auto *const resolved_b =
                config.resolvedScene().findScene("B");
            REQUIRE(resolved_b != nullptr);
            REQUIRE(resolved_b->objects.size() == 4);

            injector.resetCalls();
            injector.setEnabled(true);
            REQUIRE_THROWS_WITH(
                config.importSceneDocument(
                    b_document.dump(),
                    [&](SceneLoadRequest &&request) {
                        request.scene_id = "B";
                        return loader.load(std::move(request));
                    },
                    version),
                ContainsSubstring("injected scene-load root disconnect"));
            REQUIRE(loader.currentScene() == "B");
        }
        REQUIRE(injector.saw(SceneLoadFaultPhase::prepare, owner));
        test_support::setSceneLoadFaultInjectorForTesting(loader, nullptr);
        internal::clearPendingEvents();
        }
    }
}

TEST_CASE(
    "WP380c PelicanCore startup root reaches the executor",
    "[WP380c][transaction][roots][startup][gpu]") {
    setupLogger();
    Wp374aProjectRoot project{"wp380c_startup_root"};
    writeWp374aProject(project.path, std::nullopt, false, false, false);
    const Json settings{
        {"basic_config",
         {{"window_size", {{"width", 16}, {"height", 16}}},
          {"scene_data_json", "scene.json"},
          {"asset_data_json", "assets.json"},
          {"default_scene_id", "main"},
          {"rendering_config_json", "hybrid.json"},
          {"default_rendering_pass", "main_render"}}}};

    const auto configure_startup = [&] {
        auto &launch = FastModuleContainer::get<EngineLaunchConfig>();
        launch.headless = true;
        launch.headless_extent = vk::Extent2D{16, 16};
        launch.headless_frames = 1;
        FastModuleContainer::get<PathResolver>().setup(project.path, false);
    };

    test_support::ManifestSceneLoadFault injector{
        wp380MutationId("root.startup", "direct_load")};
    struct DefaultInjectorReset {
        ~DefaultInjectorReset() {
            test_support::setDefaultSceneLoadFaultInjectorForTesting(nullptr);
        }
    } reset_default_injector;
    injector.setEnabled(false);
    configure_startup();
    test_support::setDefaultSceneLoadFaultInjectorForTesting(&injector);
    {
        PelicanCore control{settings.dump()};
        REQUIRE(control.run());
    }
    REQUIRE(injector.saw(SceneLoadFaultPhase::prepare, "root.startup"));

    injector.resetCalls();
    injector.setEnabled(true);
    configure_startup();
    test_support::setDefaultSceneLoadFaultInjectorForTesting(&injector);
    {
        PelicanCore injected{settings.dump()};
        REQUIRE_FALSE(injected.run());
    }
    REQUIRE(injector.saw(SceneLoadFaultPhase::prepare, "root.startup"));
}

#if defined(PELICAN_WP380B_EFFECT_FIXTURE) && PELICAN_WITH_AUDIO
namespace {

void writeWp380bU16(std::ofstream &file, std::uint16_t value) {
    file.put(static_cast<char>(value & 0xff));
    file.put(static_cast<char>((value >> 8) & 0xff));
}

void writeWp380bU32(std::ofstream &file, std::uint32_t value) {
    writeWp380bU16(file, static_cast<std::uint16_t>(value));
    writeWp380bU16(file, static_cast<std::uint16_t>(value >> 16));
}

void writeWp380bTone(const std::filesystem::path &path) {
    std::ofstream file{path, std::ios::binary};
    file.write("RIFF", 4); writeWp380bU32(file, 44);
    file.write("WAVEfmt ", 8); writeWp380bU32(file, 16);
    writeWp380bU16(file, 1); writeWp380bU16(file, 1);
    writeWp380bU32(file, 8000); writeWp380bU32(file, 16000);
    writeWp380bU16(file, 2); writeWp380bU16(file, 16);
    file.write("data", 4); writeWp380bU32(file, 8);
    writeWp380bU16(file, 0); writeWp380bU16(file, 512);
    writeWp380bU16(file, 0);
    writeWp380bU16(file, static_cast<std::uint16_t>(-512));
}

Json wp380bDocument(std::string_view scene_name, std::string_view slot,
                    bool fail, std::string_view available_scene,
                    bool retire_effects = false,
                    bool early_effects = false) {
    Json components = Json::array({
        {{"name", "transform"},
         {"pos", {8.0, 3.0, 0.0}},
         {"rotation", {0.0, 0.0, 0.0, 1.0}},
         {"scale", {1.0, 1.0, 1.0}}},
        {{"name", "localtransform"},
         {"pos", {8.0, 3.0, 0.0}},
         {"rotation", {0.0, 0.0, 0.0, 1.0}},
         {"scale", {1.0, 1.0, 1.0}}},
        {{"name", "sprite_view"}, {"texture", "white"}},
        {{"name", "behavior"},
         {"type", "wp380b_effect_behavior"},
         {"params",
          {{"sound", "project://tone.wav"},
           {"slot", slot},
           {"init_scene", available_scene},
           {"retire_slot", retire_effects ? "retired_b" : ""},
           {"retire_scene", retire_effects ? "only_d" : ""},
            {"retire_reject_scene", retire_effects ? "only_e" : ""},
            {"invalid_retire_sound", retire_effects},
            {"fail_after_effects", fail}}}},
    });
    if (early_effects) {
        Json early_component{{"name", "wp380e_early_effect"},
                             {"sound", "project://tone.wav"},
                             {"slot", "component_" + std::string{slot}}};
        components.insert(components.begin() + 3,
                          std::move(early_component));
    }
    Json document{
        {"schema", "pelican.scene"},
        {"version", 1},
        {"scenes",
         {{std::string{scene_name},
           {{"objects", Json::array({{{"name", "EffectHost"},
                                      {"components", components}}})}}}}},
    };
    document["scenes"][available_scene] = {{"objects", Json::array()}};
    return document;
}

Wp380bEffectProbe wp380bProbe(const std::filesystem::path &path) {
    const auto status = configuredGameLogicStatus();
    REQUIRE(status.source == path);
    auto module = GetModuleHandleW(status.loaded_copy.c_str());
    if (module == nullptr) {
        module = GetModuleHandleW(status.loaded_copy.filename().c_str());
    }
    REQUIRE(module != nullptr);
    const auto read = reinterpret_cast<decltype(&pelican_wp380b_effect_probe)>(
        GetProcAddress(module, "pelican_wp380b_effect_probe"));
    REQUIRE(read != nullptr);
    Wp380bEffectProbe result{};
    REQUIRE(read(&result));
    return result;
}

} // namespace

TEST_CASE(
    "WP380b frozen v2 DLL effects rollback and committed RPC effect failures",
    "[WP380b][transaction][effects][frozen-v2][rpc][gpu]") {
    setupLogger();
    Wp374aProjectRoot project{"wp380b_frozen_effect"};
    writeWp374aProject(project.path, std::nullopt, false, false, true);
    writeWp380bTone(project.path / "tone.wav");
    const auto a_document = Json{{"schema", "pelican.scene"},
                                 {"version", 1},
                                 {"scenes", {{"A", {{"objects", Json::array()}}}}}};
    const auto b_document =
        wp380bDocument("B", "slot_b", false, "only_e", true, true);
    const auto failed_document =
        wp380bDocument("failed", "slot_c", true, "only_e", false, true);
    auto c_document =
        wp380bDocument("C", "slot_c_committed", false, "only_d");
    const auto d_document =
        wp380bDocument("D", "slot_d", false, "only_d");
    c_document["scenes"]["D"] = d_document.at("scenes").at("D");
    writeWp374aText(project.path / "scene.json", a_document.dump(2));
    const std::filesystem::path fixture_path{PELICAN_WP380B_EFFECT_FIXTURE};

    FastModuleContainer modules;
    Wp380RuntimeCleanup cleanup;
    const auto project_json =
        Json{{"schema", "pelican.project"}, {"version", 1},
             {"name", "wp380b-frozen-effect"},
             {"engine_min_version", "0.1.0"}}.dump();
    loadWp374aScene(project.path, fixture_path, "A", 380,
                    project_json, project.path / "user");
    auto &loader = GET_MODULE(SceneLoader);
    auto &config = GET_MODULE(ProjectBasicConfig);
    auto &audio = GET_MODULE(Audio);
    GameContext context;

    const auto import = [&](const Json &document, std::string scene) {
        return config.importSceneDocument(
            document.dump(), [&](SceneLoadRequest &&request) {
                request.scene_id = std::move(scene);
                return loader.load(std::move(request));
            });
    };
    const auto b_result = import(b_document, "B");
    REQUIRE(std::holds_alternative<SceneLoadCommitted>(b_result.load_result));
    const auto b_voice_count = audio.voiceCountForTesting();
    REQUIRE(b_voice_count == 2);
    REQUIRE(context.loadData("slot_b") == Json{{"slot", "slot_b"}});
    REQUIRE(context.loadData("component_slot_b") ==
            Json{{"source", "component_callback"}});
    const auto b_live_count = GameObjects::liveCountForTesting();

    REQUIRE_THROWS_WITH(import(failed_document, "failed"),
                        ContainsSubstring("WP380b frozen v2 activation failure"));
    const auto c_handle = wp380bProbe(fixture_path).handle;
    REQUIRE(loader.currentScene() == "B");
    REQUIRE(audio.voiceCountForTesting() == b_voice_count);
    REQUIRE(GameObjects::liveCountForTesting() == b_live_count);
    REQUIRE(context.busVolume("se") == Catch::Approx(0.38F));
    REQUIRE_FALSE(context.loadData("slot_c").has_value());
    REQUIRE_FALSE(context.loadData("component_slot_c").has_value());
    REQUIRE(wp380bProbe(fixture_path).component_callback_ran == 1);
    REQUIRE(wp380bProbe(fixture_path).base_create_deferred == 1);

    const auto c_result = import(c_document, "C");
    const auto *c_effect_failure =
        std::get_if<CommittedWithEffectFailure>(&c_result.load_result);
    REQUIRE(c_effect_failure != nullptr);
    REQUIRE(std::any_of(
        c_effect_failure->effect_errors.begin(),
        c_effect_failure->effect_errors.end(), [](const auto &error) {
            return error.api == "loadScene" &&
                   error.error_code == "scene_retire_load_rejected@1" &&
                   error.error_fields.at("scene") == "only_e";
        }));
    REQUIRE(loader.currentScene() == "C");
    REQUIRE(loader.pendingSceneForTesting() ==
            std::optional<SceneId>{"only_d"});

#if PELICAN_WITH_RPC
    test_support::ManifestSceneLoadFault injector{
        scene_load_mutation::effect_save_data};
    injector.setEnabled(true);
    test_support::setSceneLoadFaultInjectorForTesting(loader, &injector);
    const auto request =
        Json{{"jsonrpc", "2.0"}, {"id", 3804},
             {"method", "load_scene"}, {"params", {{"name", "D"}}}}
            .dump() + "\n";
    std::istringstream input{request};
    std::ostringstream output;
    runEngineRpcServer(input, output);
    const auto response = Json::parse(output.str());
    REQUIRE_FALSE(response.contains("error"));
    REQUIRE(response.contains("result"));
    REQUIRE(response.at("result").at("committed") == true);
    const auto &effect_errors = response.at("result").at("effect_errors");
    REQUIRE(effect_errors.size() == 1);
    REQUIRE(effect_errors.front().at("api") == "saveData");
    REQUIRE(loader.currentScene() == "D");
    REQUIRE(wp380bProbe(fixture_path).handle != c_handle);
    REQUIRE(loader.pendingSceneForTesting() ==
            std::optional<SceneId>{"only_d"});
    test_support::setSceneLoadFaultInjectorForTesting(loader, nullptr);
#endif
    internal::clearPendingEvents();
}
#endif

TEST_CASE(
    "WP380 Light publish-drop mutation is visible to B onInit",
    "[WP380][transaction][manifest][mutation][light][gpu]") {
    setupLogger();
    Wp374aProjectRoot project{"wp380_light_publish_mutation"};
    writeWp374aProject(project.path, std::nullopt, false, false, true);
    const auto a_document = wp380Document(1, "A", "A_pending", false);
    const auto b_document = wp380Document(2, "B", "D", false);
    writeWp374aText(project.path / "scene.json", a_document.dump(2));
    const std::filesystem::path fixture_path{
        PELICAN_WP380_BEHAVIOR_FIXTURE};
    const auto mutation =
        wp380MutationId("light", "directional_names");
    test_support::ManifestSceneLoadFault injector{mutation};
    std::array<std::int32_t, 2> observed_name_tags{};

    for (std::size_t index = 0; index < observed_name_tags.size(); ++index) {
        const bool enabled = index != 0;
        FastModuleContainer modules;
        Wp380RuntimeCleanup cleanup;
        loadWp374aScene(project.path, fixture_path, "A", 380);
        auto &loader = GET_MODULE(SceneLoader);
        auto &config = GET_MODULE(ProjectBasicConfig);
        injector.resetCalls();
        injector.setEnabled(enabled);
        test_support::setSceneLoadFaultInjectorForTesting(loader, &injector);
        config.importSceneDocument(
            b_document.dump(), [&](SceneLoadRequest &&request) {
                request.scene_id = "B";
                return loader.load(std::move(request));
            });
        Wp380Probe observed_b{};
        REQUIRE(wp380Exports(fixture_path).read_probe(2, &observed_b));
        observed_name_tags[index] = observed_b.light_name_tag;
        REQUIRE(injector.saw(SceneLoadFaultPhase::publish, "light"));
        test_support::setSceneLoadFaultInjectorForTesting(loader, nullptr);
        internal::clearPendingEvents();
    }

    REQUIRE(observed_name_tags[0] == 2);
    // The mutation drops B's candidate map, so B's real onInit probe sees the
    // only live directional name from A.
    REQUIRE(observed_name_tags[1] == 1);
}

TEST_CASE(
    "WP380 ECS rollback-bypass mutation leaves the C entity live",
    "[WP380][transaction][manifest][mutation][ecs][gpu]") {
    setupLogger();
    Wp374aProjectRoot project{"wp380_ecs_rollback_mutation"};
    writeWp374aProject(project.path, std::nullopt, false, false, true);
    const auto a_document = wp380Document(1, "A", "A_pending", false);
    const auto b_document = wp380Document(2, "B", "D", false);
    const auto c_document = wp380Document(3, "C", "E", true);
    writeWp374aText(project.path / "scene.json", a_document.dump(2));
    const std::filesystem::path fixture_path{
        PELICAN_WP380_BEHAVIOR_FIXTURE};
    const auto mutation =
        wp380MutationId("ecs", "entity_liveness");
    test_support::ManifestSceneLoadFault injector{mutation};
    std::array<bool, 2> c_entity_live{};

    for (std::size_t index = 0; index < c_entity_live.size(); ++index) {
        const bool enabled = index != 0;
        FastModuleContainer modules;
        Wp380RuntimeCleanup cleanup;
        loadWp374aScene(project.path, fixture_path, "A", 380);
        auto &loader = GET_MODULE(SceneLoader);
        auto &config = GET_MODULE(ProjectBasicConfig);
        config.importSceneDocument(
            b_document.dump(), [&](SceneLoadRequest &&request) {
                request.scene_id = "B";
                return loader.load(std::move(request));
            });
        injector.resetCalls();
        injector.setEnabled(enabled);
        test_support::setSceneLoadFaultInjectorForTesting(loader, &injector);
        REQUIRE_THROWS_WITH(
            config.importSceneDocument(
                c_document.dump(), [&](SceneLoadRequest &&request) {
                    request.scene_id = "C";
                    return loader.load(std::move(request));
                }),
            ContainsSubstring("WP380 fixture activation failure"));
        c_entity_live[index] = wp380HasLiveTransformX(30.0F);
        REQUIRE(wp380HasLiveTransformX(20.0F));
        REQUIRE(injector.saw(SceneLoadFaultPhase::publish, "ecs"));
        test_support::setSceneLoadFaultInjectorForTesting(loader, nullptr);
        internal::clearPendingEvents();
    }

    REQUIRE_FALSE(c_entity_live[0]);
    REQUIRE(c_entity_live[1]);
}

TEST_CASE(
    "WP380 document-first metadata mutation leaves C document after failure",
    "[WP380][transaction][manifest][mutation][metadata][gpu]") {
    setupLogger();
    Wp374aProjectRoot project{"wp380_metadata_split_mutation"};
    writeWp374aProject(project.path, std::nullopt, false, false, true);
    const auto a_document = wp380Document(1, "A", "A_pending", false);
    const auto b_document = wp380Document(2, "B", "D", false);
    const auto c_document = wp380Document(3, "C", "E", true);
    writeWp374aText(project.path / "scene.json", a_document.dump(2));
    const std::filesystem::path fixture_path{
        PELICAN_WP380_BEHAVIOR_FIXTURE};
    const auto mutation = wp380MutationId("metadata", "document");
    test_support::ManifestSceneLoadFault injector{mutation};

    for (const bool enabled : {false, true}) {
        FastModuleContainer modules;
        Wp380RuntimeCleanup cleanup;
        loadWp374aScene(project.path, fixture_path, "A", 380);
        auto &loader = GET_MODULE(SceneLoader);
        auto &config = GET_MODULE(ProjectBasicConfig);
        config.importSceneDocument(
            b_document.dump(), [&](SceneLoadRequest &&request) {
                request.scene_id = "B";
                return loader.load(std::move(request));
            });
        injector.resetCalls();
        injector.setEnabled(enabled);
        test_support::setSceneLoadFaultInjectorForTesting(loader, &injector);
        REQUIRE_THROWS_WITH(
            config.importSceneDocument(
                c_document.dump(), [&](SceneLoadRequest &&request) {
                    request.scene_id = "C";
                    return loader.load(std::move(request));
                }),
            ContainsSubstring("WP380 fixture activation failure"));
        REQUIRE(test_support::authoring(config.sceneDocument()).rawJson() ==
                (enabled ? c_document : b_document));
        REQUIRE(injector.saw(SceneLoadFaultPhase::commit, "metadata"));
        test_support::setSceneLoadFaultInjectorForTesting(loader, nullptr);
        internal::clearPendingEvents();
    }
}
#endif


TEST_CASE(
    "WP382c resolve_ray_occurrence answers found, absent and ambiguous from the production scene",
    "[WP382c][wp382c][ray-tracing][rt3-1][scene-loader][production][mutation][gpu]") {
    setupLogger();
    // Decision 30 makes resolve_ray_occurrence part of WP382c's standalone
    // value, and decision 4 forbids first-wins on a colliding occurrence.
    // Everything below runs on the real composition: a loaded scene, the
    // production PolygonInstanceContainer inventory, and the same
    // resolveRayOccurrence the RPC dependency in editorruntimefactory.cpp
    // forwards to.
    Wp374aProjectRoot project{"wp382c_resolve"};
    writeWp374aProject(project.path, 2, true, false, true);
    std::filesystem::create_directories(project.path / "models");
    // "chair" is authored twice in the scene: each of its occurrences is
    // reachable through two instances, so resolving one must stay ambiguous
    // without either instance winning.
    TestGltfFragmentFixture::writeGlb(project.path / "models" / "chair.glb",
                                      false, false, false, true);
    // "twin" duplicates the root node name, so both of its primitives encode
    // to the same occurrence id inside one model and the loader flags them.
    TestGltfFragmentFixture::writeGlb(project.path / "models" / "twin.glb",
                                      true, false, false, true);
    // "solo" is authored once, so each of its occurrences resolves uniquely.
    TestGltfFragmentFixture::writeGlb(project.path / "models" / "solo.glb",
                                      false, false, false, true);
    const auto renderable = [](std::string_view name,
                               std::string_view model) {
        return Json{
            {"name", std::string{name}},
            {"components",
             Json::array(
                 {{{"name", "transform"},
                   {"pos", {0.0, 0.0, 0.0}},
                   {"rotation", {0.0, 0.0, 0.0, 1.0}},
                   {"scale", {1.0, 1.0, 1.0}}},
                  {{"name", "simplemodelview"},
                   {"model", std::string{model}}}})}};
    };
    writeWp374aText(
        project.path / "scene.json",
        Json{{"schema", "pelican.scene"},
             {"version", 1},
             {"scenes",
              {{"main",
                {{"objects",
                  Json::array({renderable("ChairA", "chair"),
                               renderable("ChairB", "chair"),
                               renderable("Twin", "twin"),
                               renderable("Solo", "solo")})}}}}}}
            .dump(2));
    writeWp374aText(
        project.path / "assets.json",
        Json{{"schema", "pelican.asset_data"},
             {"version", 1},
             {"models",
              Json::array({{{"name", "chair"}, {"path", "models/chair.glb"}},
                           {{"name", "twin"}, {"path", "models/twin.glb"}},
                           {{"name", "solo"}, {"path", "models/solo.glb"}}})},
             {"textures", Json::array()}}
            .dump(2));

    FastModuleContainer modules;
    loadWp374aScene(project.path);
    GET_MODULE(ECSCore).update();
    const auto &instances = GET_MODULE(PolygonInstanceContainer);

    // The catalog is the authored expectation, not the resolver's answer:
    // four objects x two primitives each.
    const auto inventory = instances.rayOccurrences();
    REQUIRE(inventory.size() == 8);
    std::map<std::string, std::vector<RayOccurrenceCatalogEntry>> by_id;
    for (const auto &entry : inventory) {
        REQUIRE_FALSE(entry.occurrence_id.empty());
        by_id[entry.occurrence_id].push_back(entry);
    }
    // chair: 2 ids x 2 instances, twin: 1 collided id x 2 primitives,
    // solo: 2 ids x 1 instance.
    REQUIRE(by_id.size() == 5);

    std::string unique_id;
    std::string duplicated_instance_id;
    std::string collided_id;
    for (const auto &[id, entries] : by_id) {
        const bool flagged =
            entries.front().eligibility ==
            RayOccurrenceEligibilityKind::ambiguous;
        if (entries.size() == 1) unique_id = id;
        else if (flagged) collided_id = id;
        else duplicated_instance_id = id;
    }
    INFO("unique=" << unique_id << " duplicated=" << duplicated_instance_id
                   << " collided=" << collided_id);
    REQUIRE_FALSE(unique_id.empty());
    REQUIRE_FALSE(duplicated_instance_id.empty());
    REQUIRE_FALSE(collided_id.empty());

    // found{q}: one candidate, and the q it reports is the inventory's q.
    const auto found = instances.resolveRayOccurrence(unique_id);
    REQUIRE(found.state == RayOccurrenceResolveState::found);
    REQUIRE(found.candidates.size() == 1);
    REQUIRE(found.candidates.front().occurrence_id == unique_id);
    REQUIRE(found.candidates.front().eligibility ==
            RayOccurrenceEligibilityKind::ok);
    REQUIRE(found.candidates.front().q == by_id.at(unique_id).front().q);

    // absent: a well-formed id that no occurrence carries.
    const auto absent = instances.resolveRayOccurrence(
        "ffffffffffffffffffffffffffffffff");
    REQUIRE(absent.state == RayOccurrenceResolveState::absent);
    REQUIRE(absent.candidates.empty());

    // ambiguous, case 1: two eligible instances share the id. Neither wins;
    // both candidates come back, and their q values differ, so a first-wins
    // resolver cannot answer this by picking one.
    const auto duplicated =
        instances.resolveRayOccurrence(duplicated_instance_id);
    REQUIRE(duplicated.state == RayOccurrenceResolveState::ambiguous);
    REQUIRE(duplicated.candidates.size() == 2);
    REQUIRE(duplicated.candidates.front().eligibility ==
            RayOccurrenceEligibilityKind::ok);
    REQUIRE(duplicated.candidates.front().q !=
            duplicated.candidates.back().q);

    // ambiguous, case 2: the loader itself flagged the collision inside one
    // model, so the resolver must not report it as found either.
    const auto collided = instances.resolveRayOccurrence(collided_id);
    REQUIRE(collided.state == RayOccurrenceResolveState::ambiguous);
    REQUIRE(collided.candidates.size() == 2);
    REQUIRE(collided.candidates.front().eligibility ==
            RayOccurrenceEligibilityKind::ambiguous);

#if PELICAN_WITH_RPC
    // The same three states through the production RPC composition. The
    // dependency built in editorruntimefactory.cpp forwards
    // resolve_ray_occurrence straight to the container queried above, so this
    // is the wiring the injected lambda in editorcommandservice_test.cpp
    // stands in for.
    std::uint64_t rpc_id = 382700;
    auto service = makeEditorRuntimeService();
    const auto rpc_found = runWp374aRpcBytes(
        *service, rpc_id++, "resolve_ray_occurrence",
        {{"occurrence_id", unique_id}});
    REQUIRE(rpc_found.at("status") == "found");
    REQUIRE(rpc_found.at("occurrence_id") == unique_id);
    REQUIRE(rpc_found.at("q") == found.candidates.front().q);
    const auto rpc_absent = runWp374aRpcBytes(
        *service, rpc_id++, "resolve_ray_occurrence",
        {{"occurrence_id", "ffffffffffffffffffffffffffffffff"}});
    REQUIRE(rpc_absent.at("status") == "absent");
    const auto rpc_duplicated = runWp374aRpcBytes(
        *service, rpc_id++, "resolve_ray_occurrence",
        {{"occurrence_id", duplicated_instance_id}});
    REQUIRE(rpc_duplicated.at("status") == "ambiguous");
    REQUIRE(rpc_duplicated.at("candidates").size() == 2);
    const auto rpc_collided = runWp374aRpcBytes(
        *service, rpc_id++, "resolve_ray_occurrence",
        {{"occurrence_id", collided_id}});
    REQUIRE(rpc_collided.at("status") == "ambiguous");
    wp375FinishRuntime(service);
#endif

    GET_MODULE(VulkanManageCore).waitIdle();
}

} // namespace Pelican

PELICAN_WP380_HOST_API void pelican_wp380_host_read_probe(
    Pelican::Wp380Fixture::ProbeV1 *probe) {
    using namespace Pelican;
    if (probe == nullptr || probe->abi != Wp380Fixture::probe_abi_v1)
        return;
    const auto tag = probe->scene_tag;
    probe->light_name_tag = 0;
    probe->light_intensity = -1.0F;
    for (std::int32_t candidate_tag = 1; candidate_tag <= 4;
         ++candidate_tag) {
        const auto intensity = GET_MODULE(LightContainer)
                                   .directionalLightIntensityForTesting(
                                       "Light" +
                                       std::to_string(candidate_tag));
        if (intensity) {
            probe->light_name_tag = candidate_tag;
            probe->light_intensity = *intensity;
            break;
        }
    }
    const auto actor_name =
        "Actor" + std::to_string(tag) + "_0";
    probe->camera_yfov = GET_MODULE(Camera).getProjectionSpec().yfov;
    const auto object = GET_MODULE(SceneLoader).objectId(actor_name);
    probe->ecs_x = -1.0F;
    if (object) {
        if (const auto *transform =
                GET_MODULE(ECSCore)
                    .getTemplatePublicModule()
                    .tryComponent<TransformComponent>(*object)) {
            probe->ecs_x = transform->pos.x;
        }
    }
    probe->collider_count =
        GET_MODULE(PhysWorld).colliderCountForTesting();
    probe->active_trigger_pair_count =
        GET_MODULE(PhysWorld).activeTriggerPairCountForTesting();
    const auto &renderer = GET_MODULE(PolygonInstanceContainer);
    probe->renderer_instance_count =
        renderer.liveModelInstanceCountForTesting();
    probe->renderer_ordinal =
        renderer.nextDrawDeclarationOrdinalForTesting();
    probe->binding_count =
        GET_MODULE(SceneLoader).runtimeObjectBindings().size();
    probe->behavior_count =
        GET_MODULE(BehaviorAttachmentArena).snapshot().size();
}
