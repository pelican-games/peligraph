#include "../src/core/communication/proceduralassetpublication.hpp"
#include <catch2/catch_test_macros.hpp>
#include "../src/core/shader/raytracingoracle.hpp"
#include <catch2/generators/catch_generators.hpp>
#include "../src/project/assetdataformat.hpp"
#include <catch2/catch_approx.hpp>
#include <bit>
#include <cmath>
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
#include "../src/core/renderingpass/computetask.hpp"
#include "../src/core/renderingpass/renderingpassconfigloader.hpp"
#include "../src/core/renderer/camera.hpp"
#include "../src/core/renderer/polygoninstancecontainer.hpp"
#include "../src/core/renderer/proceduralgeometrycontainer.hpp"
#include "../src/core/shader/raygrouplayout.hpp"
#include "../src/core/shader/pipelinefactory.hpp"
#include "../src/core/shader/raytracingvalidation.hpp"
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
#include "vat_fixture.hpp"

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

void writeRt6MultiGeometryGlb(const std::filesystem::path &path, bool deformed) {
    const auto original = TestMorphFixture::makeGlb();
    std::uint32_t json_size = 0;
    std::memcpy(&json_size, original.data() + 12, sizeof(json_size));
    auto document = Json::parse(original.begin() + 20, original.begin() + 20 + json_size);
    std::vector<std::uint8_t> binary(original.begin() + 28 + json_size, original.end());
    const auto primitive = document["meshes"][0]["primitives"][0];
    document["meshes"][0]["primitives"] = Json::array();
    auto material = document["materials"][0];
    material["name"] = "SecondMaterial";
    document["materials"].push_back(material);
    for (int side = 0; side < 2; ++side) {
        const auto offset = binary.size();
        const float center = side == 0 ? -2.0F : 2.0F;
        for (const auto &position : std::array{
                glm::vec3{center - 0.6F, -0.6F, 0.0F}, glm::vec3{center + 0.6F, -0.6F, 0.0F},
                glm::vec3{center - 0.6F, 0.6F, 0.0F}, glm::vec3{center + 0.6F, 0.6F, 0.0F}})
            TestMorphFixture::detail::raw(binary, position);
        const auto view = document["bufferViews"].size();
        document["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", offset},
            {"byteLength", binary.size() - offset}});
        const auto accessor = document["accessors"].size();
        document["accessors"].push_back({{"bufferView", view}, {"componentType", 5126},
            {"count", 4}, {"type", "VEC3"},
            {"min", {center - 0.6F, -0.6F, 0.0F}}, {"max", {center + 0.6F, 0.6F, 0.0F}}});
        auto part = primitive;
        part["attributes"]["POSITION"] = accessor;
        part["material"] = side;
        if (!deformed) part.erase("targets");
        document["meshes"][0]["primitives"].push_back(part);
    }
    if (!deformed) document["meshes"][0].erase("weights");
    document["buffers"][0]["byteLength"] = binary.size();
    auto json = document.dump();
    while (json.size() % 4) json.push_back(' ');
    std::vector<std::uint8_t> glb;
    TestMorphFixture::detail::u32(glb, 0x46546c67);
    TestMorphFixture::detail::u32(glb, 2);
    TestMorphFixture::detail::u32(glb, static_cast<std::uint32_t>(28 + json.size() + binary.size()));
    TestMorphFixture::detail::u32(glb, static_cast<std::uint32_t>(json.size()));
    TestMorphFixture::detail::u32(glb, 0x4e4f534a);
    glb.insert(glb.end(), json.begin(), json.end());
    TestMorphFixture::detail::u32(glb, static_cast<std::uint32_t>(binary.size()));
    TestMorphFixture::detail::u32(glb, 0x004e4942);
    glb.insert(glb.end(), binary.begin(), binary.end());
    TestGltfFragmentFixture::detail::writeBytes(path, glb);
}

TEST_CASE("RT6 multiple geometry BLAS preserve per-geometry records and refit",
          "[rt6][multi-geometry][ray-tracing][production][gpu]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    bool deformed = false;
    SECTION("static mesh") {}
    SECTION("morph mesh refit") { deformed = true; }
    setupLogger();
    Wp374aProjectRoot project{"rt6_multi_geometry"};
    writeWp374aProject(project.path, 2, true, true, true,
        Json{{"name", "pelican_ray_instance"}, {"custom_index", 71}}, true);
    writeRt6MultiGeometryGlb(project.path / "models/chair.glb", deformed);
    auto scene = Json::parse(readWp374aText(project.path / "scene.json"));
    scene["scenes"]["main"]["objects"][0]["components"][0]["pos"] = {0.0, 0.0, 8.0};
    writeWp374aText(project.path / "scene.json", scene.dump());
    const auto feature_path = project.path / "features/wp377_profile_owner.json";
    auto feature = Json::parse(readWp374aText(feature_path));
    const auto target_template = feature["render_targets"][0];
    feature["render_targets"] = Json::array();
    feature["compute_tasks"] = Json::array();
    for (const std::string ray_type : {"primary", "shadow"}) {
        const auto output = "multi_" + ray_type;
        auto target = target_template;
        target["name"] = output;
        target["usage"] = {"STORAGE", "TRANSFER_SRC"};
        feature["render_targets"].push_back(target);
        feature["compute_tasks"].push_back(RayTracingRt31Fixture::wp366TypedRayTracingTask(
            "trace_" + ray_type, output, "project://shaders/" + output, false));
        auto raygen = RayTracingRt31Fixture::wp366RaygenSource(
            "PELICAN_RAY_TYPE_" + ray_type, "PELICAN_MISS_sky", "pelican_resource_" + output);
        const std::string opaque = "gl_RayFlagsOpaqueEXT";
        raygen.replace(raygen.find(opaque), opaque.size(), "gl_RayFlagsNoneEXT");
        const std::string origin = "vec3(x, 0.0, 0.0)";
        raygen.replace(raygen.find(origin), origin.size(), "vec3(x, 0.4, 0.0)");
        const std::string lane = "float x = pixel.x == 0 ? -2.0 : (pixel.x == 1 ? 2.0 : 8.0);";
        raygen.replace(raygen.find(lane), lane.size(),
            "float x = pixel.x == 0 ? -2.0 : (pixel.x == 1 ? 2.0 : -1.1);");
        writeWp374aText(project.path / ("shaders/" + output + ".rgen"), raygen);
        auto hit = RayTracingRt31Fixture::wp366ClosestHitSource(
            ray_type == "primary" ? 100u : 200u, true, false);
        const std::string values = "0u, 0u";
        hit.replace(hit.find(values), values.size(), "gl_GeometryIndexEXT, ray_record.material_index");
        writeWp374aText(project.path / ("shaders/wp366_opaque_" + ray_type + ".rchit"), hit);
    }
    writeWp374aText(feature_path, feature.dump());
    FastModuleContainer modules;
    Wp380RuntimeCleanup cleanup;
    loadWp374aScene(project.path);
    const auto sample = [&](bool moved) {
        const auto rows = renderWp374aProductionRows();
        REQUIRE(rows.rows.size() == 2);
        REQUIRE(rows.rows[0].material_index != rows.rows[1].material_index);
        for (const std::string ray_type : {"primary", "shadow"}) {
            CAPTURE(ray_type, moved);
            const auto image = GET_MODULE(Renderer).readRenderTargetForTesting(
                "multi_" + ray_type, ImageSubresourceRange{});
            REQUIRE(image.bytes.size() >= 12 * sizeof(std::uint32_t));
            std::array<std::uint32_t, 12> pixels{};
            std::memcpy(pixels.data(), image.bytes.data(), sizeof(pixels));
            CHECK(pixels[0] == (ray_type == "primary" ? 100u : 200u));
            CHECK(pixels[4] == pixels[0]);
            CHECK(pixels[1] == 71u);
            CHECK(pixels[5] == 71u);
            REQUIRE(pixels[2] < 2u);
            REQUIRE(pixels[6] < 2u);
            CHECK(pixels[2] != pixels[6]);
            CHECK(pixels[3] == rows.rows[pixels[2]].material_index);
            CHECK(pixels[7] == rows.rows[pixels[6]].material_index);
            CHECK((pixels[9] != 0xffffffffu) == moved);
        }
        const auto diagnostics = GET_MODULE(Renderer).rayQueryAccelerationStructureDiagnosticsForTesting();
        CHECK(diagnostics.active_blas_count == 1);
        CHECK(diagnostics.tlas_instance_count == 1);
        CHECK(diagnostics.blas_build_count == 1);
        REQUIRE(diagnostics.active_blas_inputs.size() == 1);
        CHECK(diagnostics.active_blas_inputs[0].geometry_count == 2);
        return diagnostics;
    };
    const auto first = sample(false);
    CHECK(first.blas_update_count == 0);
    if (deformed) {
        auto &instances = GET_MODULE(PolygonInstanceContainer);
        const auto inventory = instances.rayQueryGeometryInstances();
        REQUIRE(inventory.size() == 2);
        const auto id = instances.modelInstanceIdAt(inventory[0].instance_index);
        REQUIRE(id);
        const float weight = 1.0F;
        PublishMorphWeightFrameDescV1 frame;
        frame.layout_generation = instances.morphLayoutGenerationForTesting(*id);
        frame.frame_revision = 1;
        frame.weights = &weight;
        frame.weight_count = 1;
        REQUIRE(instances.publishMorphWeightFrame(*id, frame) == Animation::Status::ok);
        const auto updated = sample(true);
        CHECK(updated.blas_update_count == 1);
    } else {
        const auto reused = sample(false);
        CHECK(reused.blas_update_count == 0);
    }
#else
    SKIP("RT6 multi geometry fixture requires the runtime compiler");
#endif
}

TEST_CASE("project AS connections override authored ray query tasks", "[rendercompiler][rt7][task-as]") {
    nlohmann::json config = {{"compute_tasks", nlohmann::json::array({
        {{"name", "query"}, {"shader", "project://shaders/query"},
         {"dispatch", {{"groups", {1, 1, 1}}}},
         {"ray_query", {{"acceleration_structure", "scene"}}}}
    })}};
    const auto parse = [&] { return parseComputeTaskDefinitionsFromConfigJson(config, nullptr); };
    REQUIRE(parse().front().ray_query_acceleration_structure == "scene");
    config["ray_task_acceleration_structures"] = {{"query", "actors"}};
    CHECK(parse().front().ray_query_acceleration_structure == "actors");
    CHECK(config["compute_tasks"][0]["ray_query"]["acceleration_structure"] == "scene");
    for (const auto &bad : nlohmann::json::array({
            nlohmann::json::array(), {{"query", ""}}, {{"query", 1}}, {{"missing", "scene"}}})) {
        config["ray_task_acceleration_structures"] = bad;
        CHECK_THROWS(parse());
    }
    config["ray_task_acceleration_structures"] = {{"query", "actors"}};
    config["compute_tasks"][0].erase("ray_query");
    CHECK_THROWS_WITH(parse(), Catch::Matchers::ContainsSubstring("does not declare"));
    config.erase("compute_tasks");
    CHECK_THROWS_WITH(parse(), Catch::Matchers::ContainsSubstring("unknown task"));
}

TEST_CASE("project callable initial values use the declared record schema", "[rendercompiler][rt7][callable-edit]") {
    Json config{{"compute_tasks", Json::array({{
        {"name", "trace"}, {"dispatch", {{"rays", {1, 1, 1}}}},
        {"ray_tracing", {{"raygen", "raygen"}, {"miss", {"miss"}}, {"closesthit", {"hit"}},
            {"callable", {"call"}},
            {"callable_record", Json::array({{{"name", "value"}, {"type", "u32"}}})},
            {"callable_records", Json::array({{{"value", 1}}})}}}
    }})}};
    const auto parse = [&] { return parseComputeTaskDefinitionsFromConfigJson(config); };
    const auto original = parse().front().ray_tracing->callable_records;
    config["ray_task_callable_records"] = {{"trace", Json::array({{{"value", 4294967295u}}})}};
    CHECK(bool(parse().front().ray_tracing->callable_records != original));
    CHECK(config["compute_tasks"][0]["ray_tracing"]["callable_records"][0]["value"] == 1);
    for (const auto &invalid : Json::array({Json::array(),
            Json{{"missing", Json::array({{{"value", 1}}})}},
            Json{{"trace", Json::array()}},
            Json{{"trace", Json::array({{{"value", -1}}})}},
            Json{{"trace", Json::array({{{"value", 4294967296ull}}})}}})) {
        config["ray_task_callable_records"] = invalid;
        CHECK_THROWS(parse());
    }
    config["ray_task_callable_records"] = Json::object();
    CHECK(bool(parse().front().ray_tracing->callable_records == original));
}

TEST_CASE("compute ray query declares its acceleration structure", "[rendercompiler][rt6][named-query]") {
    using Json = nlohmann::json;
    Json task{{"name", "query"}, {"shader", "query"},
        {"dispatch", {{"groups", {1, 1, 1}}}},
        {"ray_query", {{"acceleration_structure", "actors"}}}};
    const auto parse = [&] {
        return parseComputeTaskDefinitionsFromConfigJson({{"compute_tasks", Json::array({task})}});
    };
    REQUIRE(parse().front().ray_query_acceleration_structure == "actors");
    for (const auto &bad : Json::array({Json::object(), "actors",
            {{"acceleration_structure", ""}}, {{"acceleration_structure", 1}},
            {{"acceleration_structure", "actors"}, {"unknown", 1}}})) {
        task["ray_query"] = bad;
        CHECK_THROWS(parse());
    }
    task["ray_query"] = {{"acceleration_structure", "actors"}};
    task["ray_tracing"] = Json::object();
    CHECK_THROWS(parse());
}

TEST_CASE("RT6 named TLAS tasks select independent scene subsets with shared records",
          "[rt6][named-tlas][ray-tracing][production][gpu]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    bool query = false;
    bool before_as = false;
    SECTION("ray tracing pipeline") {}
    SECTION("compute ray query") { query = true; }
    SECTION("independent compute before first scene AS build") { before_as = true; }
    setupLogger();
    Wp374aProjectRoot project{"rt6_named_tlas"};
    writeWp374aProject(project.path, 2, true, true, true,
        Json{{"name", "pelican_ray_instance"}, {"mask", 1}, {"custom_index", 71}}, true);
    TestMorphFixture::writeGlb(project.path / "models/chair.glb");
    auto scene = Json::parse(readWp374aText(project.path / "scene.json"));
    auto left = scene["scenes"]["main"]["objects"][0];
    left["name"] = "Left";
    left["components"][0]["pos"] = {-2.0, 0.0, 8.0};
    auto right = left;
    right["name"] = "Right";
    right["components"][0]["pos"] = {2.0, 0.0, 8.0};
    right["components"][2]["mask"] = 2;
    right["components"][2]["custom_index"] = 82;
    scene["scenes"]["main"]["objects"] = Json::array({left, right});
    writeWp374aText(project.path / "scene.json", scene.dump());
    auto rendering = Json::parse(readWp374aText(project.path / "hybrid.json"));
    rendering["scene_acceleration_structures"] = Json::array({
        {{"name", "left"}, {"instance_mask", 1}},
        {{"name", "right"}, {"instance_mask", 2}},
        {{"name", "empty"}, {"instance_mask", 0}}});
    writeWp374aText(project.path / "hybrid.json", rendering.dump());
    const auto feature_path = project.path / "features/wp377_profile_owner.json";
    auto feature = Json::parse(readWp374aText(feature_path));
    const auto target_template = feature["render_targets"][0];
    feature["render_targets"] = Json::array();
    feature["compute_tasks"] = Json::array();
    for (const std::string name : {"left", "right", "empty"}) {
        const auto output = "rt6_" + name;
        auto target = target_template;
        target["name"] = output;
        target["usage"] = {"STORAGE", "TRANSFER_SRC"};
        feature["render_targets"].push_back(target);
        auto task = RayTracingRt31Fixture::wp366TypedRayTracingTask(
            "trace_" + name, output, "project://shaders/" + output, false);
        task["ray_tracing"]["acceleration_structure"] = name;
        if (query) {
            feature["compute_tasks"].push_back(task);
            task["name"] = "query_" + name;
            task["after"] = {"trace_" + name};
            task.erase("ray_tracing");
            task["shader"] = "project://shaders/" + output;
            task["ray_query"] = {{"acceleration_structure", name}};
            task["dispatch"] = {{"groups", {3, 1, 1}}};
            std::string query_shader = R"glsl(#version 460
#extension GL_EXT_ray_query : require
#extension GL_GOOGLE_include_directive : require
#include "pelican_resource_ports.glsl"
layout(set=0,binding=6) uniform accelerationStructureEXT pelicanRayTracingScene;
layout(local_size_x=1, local_size_y=1, local_size_z=1) in;
void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    float x = pixel.x == 0 ? -2.0 : (pixel.x == 1 ? 2.0 : 8.0);
    rayQueryEXT query;
    rayQueryInitializeEXT(query, pelicanRayTracingScene, gl_RayFlagsOpaqueEXT,
        255u, vec3(x, 0.0, 0.0), 0.001, vec3(0.0, 0.0, 1.0), 4096.0);
    while (rayQueryProceedEXT(query)) {}
    uint custom = rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT
        ? 0xffffffffu : rayQueryGetIntersectionInstanceCustomIndexEXT(query, true);
    imageStore(OUTPUT, pixel, uvec4(0u, custom, 0u, 0u));
}
)glsl";
            query_shader.replace(query_shader.find("OUTPUT"), 6, "pelican_resource_" + output);
            writeWp374aText(project.path / ("shaders/" + output + ".comp"), query_shader);
        }
        feature["compute_tasks"].push_back(task);
        auto shader = RayTracingRt31Fixture::wp366RaygenSource(
            "PELICAN_RAY_TYPE_primary", "PELICAN_MISS_sky", "pelican_resource_" + output);
        const std::string opaque = "gl_RayFlagsOpaqueEXT";
        shader.replace(shader.find(opaque), opaque.size(), "gl_RayFlagsNoneEXT");
        writeWp374aText(project.path / ("shaders/" + output + ".rgen"), shader);
    }
    if (before_as) {
        auto target = target_template;
        target["name"] = "pre_as_output";
        target["usage"] = {"STORAGE", "TRANSFER_SRC"};
        feature["render_targets"].push_back(target);
        auto task = feature["compute_tasks"][0];
        task["name"] = "pre_as_compute";
        task.erase("ray_tracing");
        task["shader"] = "project://shaders/pre_as_compute";
        task["dispatch"] = {{"groups", {1, 1, 1}}};
        task["writes"] = {"pre_as_output"};
        task["resource_ports"] = {{"pre_as_output", {
            {"resource", "pre_as_output"}, {"access", "storage"}}}};
        feature["compute_tasks"].push_back(task);
        rendering["frame_node_order"] = {{"main_render", {"pre_as_compute", "__scene_acceleration_structure"}}};
        writeWp374aText(project.path / "hybrid.json", rendering.dump());
        writeWp374aText(project.path / "shaders/pre_as_compute.comp", R"glsl(#version 460
#extension GL_GOOGLE_include_directive : require
#include "pelican_resource_ports.glsl"
layout(local_size_x=1, local_size_y=1, local_size_z=1) in;
void main() { imageStore(pelican_resource_pre_as_output, ivec2(0), uvec4(42u)); }
)glsl");
    }
    writeWp374aText(feature_path, feature.dump());
    FastModuleContainer modules;
    Wp380RuntimeCleanup cleanup;
    loadWp374aScene(project.path);
    GET_MODULE(Renderer).setExecutionTracingForTesting(true);
    const auto rows = renderWp374aProductionRows();
    REQUIRE(rows.rows.size() == 2);
    const auto &execution_nodes = GET_MODULE(Renderer).lastExecutionTraceForTesting().at("nodes");
    REQUIRE(execution_nodes.size() >= 4);
    const std::size_t as_offset = before_as ? 1 : 0;
    CHECK(execution_nodes[as_offset].at("name") == "__scene_acceleration_structure");
    if (before_as) {
        CHECK(execution_nodes[0].at("name") == "pre_as_compute");
        const auto image = GET_MODULE(Renderer).readRenderTargetForTesting("pre_as_output", ImageSubresourceRange{});
        REQUIRE(image.bytes.size() >= sizeof(std::uint32_t));
        std::uint32_t pixel = 0;
        std::memcpy(&pixel, image.bytes.data(), sizeof(pixel));
        CHECK(pixel == 42u);
    }
    for (const auto *name : {"left", "right", "empty"}) {
        CAPTURE(name);
        const auto node = std::find_if(execution_nodes.begin() + as_offset + 1, execution_nodes.begin() + as_offset + 4,
            [&](const auto &entry) { return entry.at("name") == std::string{"__scene_acceleration_structure:"} + name; });
        REQUIRE(node != execution_nodes.begin() + as_offset + 4);
        CHECK(node->at("kind") == "acceleration_structure");
        CHECK(node->at("recorded") == true);
    }
    for (const std::string name : {"left", "right", "empty"}) {
        CAPTURE(name);
        const auto image = GET_MODULE(Renderer).readRenderTargetForTesting(
            "rt6_" + name, ImageSubresourceRange{});
        REQUIRE(image.bytes.size() >= 12 * sizeof(std::uint32_t));
        std::array<std::uint32_t, 12> pixels{};
        std::memcpy(pixels.data(), image.bytes.data(), sizeof(pixels));
        CHECK(pixels[1] == (name == "left" ? 71u : 0xffffffffu));
        CHECK(pixels[5] == (name == "right" ? 82u : 0xffffffffu));
        CHECK(pixels[9] == 0xffffffffu);
        if (query) CHECK(pixels[0] == 0u);
    }
    const auto diagnostics = GET_MODULE(Renderer).rayQueryAccelerationStructureDiagnosticsForTesting();
    CHECK(diagnostics.tlas_instance_count == 2);
    CHECK(diagnostics.named_tlas_instance_counts.at("left") == 1);
    CHECK(diagnostics.named_tlas_instance_counts.at("right") == 1);
    CHECK(diagnostics.named_tlas_instance_counts.at("empty") == 0);
    CHECK(diagnostics.tlas_build_count == 4);
    CHECK(diagnostics.blas_build_count == 2);
#if PELICAN_WITH_RPC
    {
        auto service = makeEditorRuntimeService(EditorRuntimeServiceOptions{.render_config_editing = true});
        std::uint64_t rpc_id = 70000;
        if (query) {
            const auto catalog = runWp374aRpcBytes(*service, rpc_id++, "get_compute_task_authoring", Json::object());
            const auto &tasks = catalog.at("tasks");
            const auto selected = std::find_if(tasks.begin(), tasks.end(), [](const auto &entry) {
                return entry.at("name") == "query_left";
            });
            REQUIRE(selected != tasks.end());
            auto changed = selected->at("declaration");
            changed.erase("ray_query");
            const auto saved = readWp374aText(feature_path);
            const auto accepted = runWp374aRpcBytes(*service, rpc_id++, "edit_compute_task", {
                {"base_source_digest", catalog.at("source_digest").at("hex")},
                {"base_task_source_digest", selected->at("source_digest").at("hex")},
                {"task_name", "query_left"}, {"task", changed}});
            REQUIRE(accepted.at("status") == "accepted");
            service->commitPendingEdits();
            const auto result = runWp374aRpcBytes(*service, rpc_id++, "get_edit_result", {{"ticket", accepted.at("ticket")}});
            INFO(result.dump());
            CHECK(result.at("committed") == false);
            CHECK(result.dump().find("does not declare ray_query") != std::string::npos);
            CHECK(readWp374aText(feature_path) == saved);
        }
        REQUIRE(execution_nodes[as_offset + 1].at("name") == "__scene_acceleration_structure:empty");
        const auto set_as_order = [&](const Json &orders) {
            const auto context = runWp374aRpcBytes(*service, rpc_id++, "get_frame_node_order", Json::object());
            const auto accepted = runWp374aRpcBytes(*service, rpc_id++, "edit_frame_node_order", {
                {"base_source_digest", context.at("source_digest").at("hex")}, {"orders", orders}});
            REQUIRE(accepted.at("status") == "accepted");
            service->commitPendingEdits();
            (void)renderWp374aProductionRows();
            const auto result = runWp374aRpcBytes(*service, rpc_id++, "get_edit_result", {{"ticket", accepted.at("ticket")}});
            INFO(result.dump());
            REQUIRE(result.at("committed") == true);
        };
        const std::string left_consumer = query ? "query_left" : "trace_left";
        set_as_order({{"main_render", {"__scene_acceleration_structure:left", left_consumer,
                                       "__scene_acceleration_structure:right", "__scene_acceleration_structure:empty"}}});
        // Publication replaces the targets at the boundary; read the next completed frame.
        (void)renderWp374aProductionRows();
        const auto ordered_nodes = GET_MODULE(Renderer).lastExecutionTraceForTesting().at("nodes");
        const auto execution_position = [&](const std::string &name) {
            const auto found = std::find_if(ordered_nodes.begin(), ordered_nodes.end(), [&](const auto &node) { return node.at("name") == name; });
            REQUIRE(found != ordered_nodes.end());
            return std::distance(ordered_nodes.begin(), found);
        };
        CHECK(execution_position("__scene_acceleration_structure:left") < execution_position(left_consumer));
        CHECK(execution_position(left_consumer) < execution_position("__scene_acceleration_structure:right"));
        CHECK(execution_position("__scene_acceleration_structure:right") < execution_position("__scene_acceleration_structure:empty"));
        for (const std::string name : {"left", "right"}) {
            const auto image = GET_MODULE(Renderer).readRenderTargetForTesting("rt6_" + name, ImageSubresourceRange{});
            std::array<std::uint32_t, 12> pixels{};
            REQUIRE(image.bytes.size() >= sizeof(pixels));
            std::memcpy(pixels.data(), image.bytes.data(), sizeof(pixels));
            CHECK(pixels[1] == (name == "left" ? 71u : 0xffffffffu));
            CHECK(pixels[5] == (name == "right" ? 82u : 0xffffffffu));
        }
        set_as_order(Json::object());
        auto before = runWp374aRpcBytes(*service, rpc_id++, "get_scene_acceleration_structures", Json::object());
        auto definitions = before.at("definitions");
        definitions[0]["instance_mask"] = 2;
        definitions[1]["instance_mask"] = 1;
        const auto accepted = runWp374aRpcBytes(*service, rpc_id++, "edit_scene_acceleration_structures", {
            {"base_source_digest", before.at("source_digest").at("hex")}, {"definitions", definitions}});
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPendingEdits();
        (void)renderWp374aProductionRows();
        const auto result = runWp374aRpcBytes(*service, rpc_id++, "get_edit_result", {{"ticket", accepted.at("ticket")}});
        REQUIRE(result.at("committed") == true);
        CHECK(Json::parse(readWp374aText(project.path / "hybrid.json")).at("scene_acceleration_structures") == definitions);
        // Publication retires the old targets at the frame boundary; draw the new generation.
        (void)renderWp374aProductionRows();
        const auto image = GET_MODULE(Renderer).readRenderTargetForTesting("rt6_left", ImageSubresourceRange{});
        std::array<std::uint32_t, 12> pixels{};
        REQUIRE(image.bytes.size() >= sizeof(pixels));
        std::memcpy(pixels.data(), image.bytes.data(), sizeof(pixels));
        CHECK(pixels[1] == 0xffffffffu);
        CHECK(pixels[5] == 82u);
        // Project-owned connections override the shared feature task without editing it.
        before = runWp374aRpcBytes(*service, rpc_id++, "get_scene_acceleration_structures", Json::object());
        const auto feature_before_connection = readWp374aText(feature_path);
        const Json connections = {{query ? "query_left" : "trace_left", "right"}};
        const auto connected = runWp374aRpcBytes(*service, rpc_id++, "edit_scene_acceleration_structures", {
            {"base_source_digest", before.at("source_digest").at("hex")},
            {"definitions", definitions}, {"task_connections", connections}});
        REQUIRE(connected.at("status") == "accepted");
        service->commitPendingEdits();
        (void)renderWp374aProductionRows();
        REQUIRE(runWp374aRpcBytes(*service, rpc_id++, "get_edit_result",
            {{"ticket", connected.at("ticket")}}).at("committed") == true);
        (void)renderWp374aProductionRows();
        const auto connected_image = GET_MODULE(Renderer).readRenderTargetForTesting("rt6_left", ImageSubresourceRange{});
        REQUIRE(connected_image.bytes.size() >= sizeof(pixels));
        std::memcpy(pixels.data(), connected_image.bytes.data(), sizeof(pixels));
        CHECK(pixels[1] == 71u);
        CHECK(pixels[5] == 0xffffffffu);
        CHECK(readWp374aText(feature_path) == feature_before_connection);
        CHECK(Json::parse(readWp374aText(project.path / "hybrid.json")).at("ray_task_acceleration_structures") == connections);
        // Removing a referenced AS must leave the source and active graph intact.
        before = runWp374aRpcBytes(*service, rpc_id++, "get_scene_acceleration_structures", Json::object());
        const auto source_before_failure = readWp374aText(project.path / "hybrid.json");
        const auto removal = runWp374aRpcBytes(*service, rpc_id++, "edit_scene_acceleration_structures", {
            {"base_source_digest", before.at("source_digest").at("hex")}, {"definitions", Json::array()}});
        REQUIRE(removal.at("status") == "accepted");
        service->commitPendingEdits();
        const auto rejected = runWp374aRpcBytes(*service, rpc_id++, "get_edit_result", {{"ticket", removal.at("ticket")}});
        REQUIRE(rejected.at("committed") == false);
        CHECK(readWp374aText(project.path / "hybrid.json") == source_before_failure);
        CHECK(runWp374aRpcBytes(*service, rpc_id++, "get_scene_acceleration_structures", Json::object()) == before);
    }
#endif
#else
    SKIP("RT6 named TLAS fixture requires the runtime compiler");
#endif
}

TEST_CASE("RT6 GPU generated indirect ray dimensions reach every 3D lane",
          "[rt6][indirect][ray-tracing][production][gpu]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    Wp374aProjectRoot project{"rt6_indirect_launch"};
    writeWp386aProceduralProject(project.path);
    const auto feature_path = project.path / "features/wp386_procedural.json";
    auto feature = Json::parse(readWp374aText(feature_path));
    feature["buffers"] = Json::array({{
        {"name", "ray_args"}, {"size", 16}, {"lifetime", "persistent"},
        {"command_layout", "ray_dispatch"}
    }});
    feature["compute_tasks"][0]["dispatch"] = {
        {"indirect", {{"buffer", "ray_args"}, {"offset", 4}}}};
    feature["compute_tasks"].insert(feature["compute_tasks"].begin(), Json{
        {"name", "build_ray_args"},
        {"shader", "project://shaders/build_ray_args"},
        {"writes", {"ray_args"}},
        {"resource_ports", {{"args", {
            {"resource", "ray_args"}, {"kind", "buffer"},
            {"element", "uvec4"}, {"access", "storage"}}}}},
        {"dispatch", {{"groups", {1, 1, 1}}}},
        {"schedule", "per_frame"}
    });
    writeWp374aText(project.path / "shaders/build_ray_args.comp", R"glsl(#version 460
#extension GL_GOOGLE_include_directive : require
#include "pelican_resource_ports.glsl"
layout(local_size_x=1, local_size_y=1, local_size_z=1) in;
void main() {
    pelican_resource_args.values[0] = uvec4(0u, 2u, 3u, 2u);
}
)glsl");
    writeWp374aText(feature_path, feature.dump());
    auto raygen = RayTracingRt31Fixture::wp366RaygenSource(
        "0u", "0u", "pelican_resource_wp386_out");
    const std::string pixel = "ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);";
    raygen.replace(raygen.find(pixel), pixel.size(), R"glsl(
    uint lane = gl_LaunchIDEXT.x + gl_LaunchSizeEXT.x *
                (gl_LaunchIDEXT.y + gl_LaunchSizeEXT.y * gl_LaunchIDEXT.z);
    ivec2 pixel = ivec2(lane, 0);
)glsl");
    const std::string result = "uvec4(payload.marker, payload.q, payload.flags, payload.tint)";
    raygen.replace(raygen.find(result), result.size(), R"glsl(
        uvec4(payload.marker + gl_LaunchIDEXT.x, gl_LaunchIDEXT.y, gl_LaunchIDEXT.z,
              gl_LaunchSizeEXT.x + 256u * gl_LaunchSizeEXT.y + 65536u * gl_LaunchSizeEXT.z)
)glsl");
    writeWp374aText(project.path / "shaders/wp386.rgen", raygen);

    FastModuleContainer modules;
    Wp380RuntimeCleanup cleanup;
    (void)configureWp374aRuntime(project.path);
    TestSupport::requireVulkanDevice("RT6 indirect ray launch");
    if (!GET_MODULE(VulkanManageCore).getRuntimeCapabilities().ray_tracing_indirect) {
        SKIP("rayTracingPipelineTraceRaysIndirect is unavailable");
    }
    GET_MODULE(ECSPredefinedRegistration).reg();
    test_support::loadScene(GET_MODULE(SceneLoader), "main");
    const auto check_lanes = [&](std::uint32_t width, std::uint32_t height,
                                 std::uint32_t depth) {
        auto &renderer = GET_MODULE(Renderer);
        renderer.setExecutionTracingForTesting(true);
        (void)renderWp374aProductionRows();
        const auto &nodes = renderer.lastExecutionTraceForTesting().at("nodes");
        REQUIRE_FALSE(nodes.empty());
        CHECK(nodes.front().at("kind") == "acceleration_structure");
        CHECK(nodes.front().at("recorded") == true);
        CHECK(nodes.front().at("name") == "__scene_acceleration_structure");
        const auto image = GET_MODULE(Renderer).readRenderTargetForTesting(
            "wp386_out", ImageSubresourceRange{});
        const auto lane_count = width * height * depth;
        REQUIRE(image.bytes.size() >= lane_count * 4 * sizeof(std::uint32_t));
        for (std::uint32_t z = 0; z < depth; ++z) {
            for (std::uint32_t y = 0; y < height; ++y) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    const auto lane = x + width * (y + height * z);
                    CAPTURE(x, y, z);
                    std::array<std::uint32_t, 4> actual{};
                    std::memcpy(actual.data(), image.bytes.data() + lane * sizeof(actual),
                                sizeof(actual));
                    const std::array<std::uint32_t, 4> expected{
                        0x386au + x, y, z, width + 256u * height + 65536u * depth};
                    CHECK(actual == expected);
                }
            }
        }
    };
    check_lanes(2, 3, 2);
    auto service = makeEditorRuntimeService(EditorRuntimeServiceOptions{.render_config_editing = true});
    std::uint64_t rpc_id = 93000;
    const auto edit_task = [&](const std::string &name, const std::function<void(Json &)> &modify) {
        const auto catalog = runWp374aRpcBytes(*service, rpc_id++, "get_compute_task_authoring", Json::object());
        const auto &tasks = catalog.at("tasks");
        const auto selected = std::find_if(tasks.begin(), tasks.end(), [&](const auto &entry) { return entry.at("name") == name; });
        REQUIRE(selected != tasks.end());
        REQUIRE(selected->at("editable") == true);
        auto declaration = selected->at("declaration");
        modify(declaration);
        const Json request{{"base_source_digest", catalog.at("source_digest").at("hex")},
                           {"base_task_source_digest", selected->at("source_digest").at("hex")},
                           {"task_name", name}, {"task", declaration}};
        const auto accepted = runWp374aRpcBytes(*service, rpc_id++, "edit_compute_task", request);
        INFO(accepted.dump());
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPendingEdits();
        (void)renderWp374aProductionRows();
        const auto result = runWp374aRpcBytes(*service, rpc_id++, "get_edit_result", {{"ticket", accepted.at("ticket")}});
        INFO(result.dump());
        REQUIRE(result.at("committed") == true);
        CHECK(runWp374aRpcBytes(*service, rpc_id++, "edit_compute_task", request).at("status") == "rejected");
    };
    writeWp374aText(project.path / "shaders/edited_ray_args.comp", R"glsl(#version 460
#extension GL_GOOGLE_include_directive : require
#include "pelican_resource_ports.glsl"
layout(local_size_x=1, local_size_y=1, local_size_z=1) in;
void main() { pelican_resource_args.values[0] = uvec4(0u, 1u, 2u, 1u); }
)glsl");
    const auto root_before = readWp374aText(project.path / "hybrid.json");
    edit_task("build_ray_args", [](Json &task) { task["shader"] = "project://shaders/edited_ray_args"; });
    check_lanes(1, 2, 1);
    edit_task("wp386_trace", [](Json &task) { task["dispatch"] = {{"rays", {2, 1, 1}}}; });
    check_lanes(2, 1, 1);
    CHECK(readWp374aText(project.path / "hybrid.json") == root_before);
    const auto commit_task_change = [&](const Json &accepted) {
        INFO(accepted.dump());
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPendingEdits();
        GET_MODULE(ECSCore).update();
        GET_MODULE(ECSCore).update();
        GET_MODULE(Renderer).render();
        GET_MODULE(VulkanManageCore).waitIdle();
        const auto changed = runWp374aRpcBytes(*service, rpc_id++, "get_edit_result", {{"ticket", accepted.at("ticket")}});
        INFO(changed.dump());
        REQUIRE(changed.at("committed") == true);
    };
    // Remove both kinds through the live editor, then recreate them in separate project features.
    const auto catalog = runWp374aRpcBytes(*service, rpc_id++, "get_compute_task_authoring", Json::object());
    for (const auto &entry : catalog.at("tasks")) {
        const auto current = runWp374aRpcBytes(*service, rpc_id++, "get_compute_task_authoring", Json::object());
        const auto &tasks = current.at("tasks");
        const auto selected = std::find_if(tasks.begin(), tasks.end(), [&](const auto &task) { return task.at("name") == entry.at("name"); });
        REQUIRE(selected != tasks.end());
        commit_task_change(runWp374aRpcBytes(*service, rpc_id++, "remove_compute_task", {
            {"base_source_digest", current.at("source_digest").at("hex")},
            {"base_task_source_digest", selected->at("source_digest").at("hex")},
            {"task_name", entry.at("name")}}));
    }
    REQUIRE(runWp374aRpcBytes(*service, rpc_id++, "get_compute_task_authoring", Json::object()).at("tasks").empty());
    for (const auto &entry : catalog.at("tasks")) {
        const auto current = runWp374aRpcBytes(*service, rpc_id++, "get_compute_task_authoring", Json::object());
        commit_task_change(runWp374aRpcBytes(*service, rpc_id++, "add_compute_task", {
            {"base_source_digest", current.at("source_digest").at("hex")},
            {"destination_reference", "project://features/new_" + entry.at("name").get<std::string>() + ".json"},
            {"task", entry.at("declaration")}}));
    }
    check_lanes(2, 1, 1);
    edit_task("wp386_trace", [](Json &task) { task["dispatch"] = {{"indirect", {{"buffer", "ray_args"}, {"offset", 4}}}}; });
    check_lanes(1, 2, 1);
    const auto execution_position = [&](const std::string &name) {
        const auto &nodes = GET_MODULE(Renderer).lastExecutionTraceForTesting().at("nodes");
        const auto found = std::find_if(nodes.begin(), nodes.end(), [&](const auto &node) { return node.at("name") == name; });
        REQUIRE(found != nodes.end());
        return found - nodes.begin();
    };
    REQUIRE(execution_position("scene_present") < execution_position("wp386_trace"));
    const auto order_context = runWp374aRpcBytes(*service, rpc_id++, "get_frame_node_order", Json::object());
    Json order_request{{"base_source_digest", order_context.at("source_digest").at("hex")},
                       {"orders", {{"main_render", {"wp386_trace", "scene_present"}}}}};
    commit_task_change(runWp374aRpcBytes(*service, rpc_id++, "edit_frame_node_order", order_request));
    check_lanes(1, 2, 1);
    CHECK(execution_position("wp386_trace") < execution_position("scene_present"));
    const auto ordered_root = readWp374aText(project.path / "hybrid.json");
    order_request["base_source_digest"] = runWp374aRpcBytes(*service, rpc_id++, "get_frame_node_order", Json::object()).at("source_digest").at("hex");
    order_request["orders"]["main_render"] = {"wp386_trace", "build_ray_args"};
    const auto rejected_order = runWp374aRpcBytes(*service, rpc_id++, "edit_frame_node_order", order_request);
    REQUIRE(rejected_order.at("status") == "accepted");
    service->commitPendingEdits();
    check_lanes(1, 2, 1);
    const auto order_result = runWp374aRpcBytes(*service, rpc_id++, "get_edit_result", {{"ticket", rejected_order.at("ticket")}});
    INFO(order_result.dump());
    CHECK(order_result.at("committed") == false);
    CHECK(readWp374aText(project.path / "hybrid.json") == ordered_root);
    CHECK(execution_position("wp386_trace") < execution_position("scene_present"));
#else
    SKIP("RT6 indirect production shader fixture requires the runtime compiler");
#endif
}

TEST_CASE("RT6 explicit ray launch dimensions and runtime edits reach every 3D lane",
          "[rt6][ray-tracing][production][gpu]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    Wp374aProjectRoot project{"rt6_launch_extent"};
    writeWp386aProceduralProject(project.path);
    const auto feature_path = project.path / "features/wp386_procedural.json";
    auto feature = Json::parse(readWp374aText(feature_path));
    feature["compute_tasks"][0]["dispatch"] = {{"rays", {2, 3, 2}}};
    writeWp374aText(feature_path, feature.dump());
    auto raygen = RayTracingRt31Fixture::wp366RaygenSource(
        "0u", "0u", "pelican_resource_wp386_out");
    const std::string pixel = "ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);";
    raygen.replace(raygen.find(pixel), pixel.size(), R"glsl(
    uint lane = gl_LaunchIDEXT.x + gl_LaunchSizeEXT.x *
                (gl_LaunchIDEXT.y + gl_LaunchSizeEXT.y * gl_LaunchIDEXT.z);
    ivec2 pixel = ivec2(lane, 0);
)glsl");
    const std::string result = "uvec4(payload.marker, payload.q, payload.flags, payload.tint)";
    raygen.replace(raygen.find(result), result.size(), R"glsl(
        uvec4(payload.marker + gl_LaunchIDEXT.x, gl_LaunchIDEXT.y, gl_LaunchIDEXT.z,
              gl_LaunchSizeEXT.x + 256u * gl_LaunchSizeEXT.y + 65536u * gl_LaunchSizeEXT.z)
)glsl");
    writeWp374aText(project.path / "shaders/wp386.rgen", raygen);

    FastModuleContainer modules;
    Wp380RuntimeCleanup cleanup;
    loadWp374aScene(project.path);
    const auto check_lanes = [&](std::uint32_t width, std::uint32_t height,
                                 std::uint32_t depth) {
        (void)renderWp374aProductionRows();
        const auto image = GET_MODULE(Renderer).readRenderTargetForTesting(
            "wp386_out", ImageSubresourceRange{});
        const auto lane_count = width * height * depth;
        REQUIRE(image.bytes.size() >= lane_count * 4 * sizeof(std::uint32_t));
        for (std::uint32_t z = 0; z < depth; ++z) {
            for (std::uint32_t y = 0; y < height; ++y) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    const auto lane = x + width * (y + height * z);
                    CAPTURE(x, y, z);
                    std::array<std::uint32_t, 4> actual{};
                    std::memcpy(actual.data(), image.bytes.data() + lane * sizeof(actual),
                                sizeof(actual));
                    const std::array<std::uint32_t, 4> expected{
                        0x386au + x, y, z, width + 256u * height + 65536u * depth};
                    CHECK(actual == expected);
                }
            }
        }
    };
    check_lanes(2, 3, 2);
    const GameContext context;
    context.setRayTracingDispatchExtent("wp386_trace", 3, 1, 2);
    check_lanes(3, 1, 2);
    CHECK_THROWS(context.setRayTracingDispatchExtent("wp386_trace", 3, 1, 0));
    check_lanes(3, 1, 2);
#else
    SUCCEED("RT6 production shader fixture requires the runtime compiler");
#endif
}

TEST_CASE("RT5 shared skinned mesh instances trace their current skin and morph deformation",
          "[rt5][ray-tracing][production][gpu]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    Wp374aProjectRoot project{"rt5_deformation"};
    writeWp374aProject(project.path, 2, true, false, true, std::nullopt, true);
    TestMorphFixture::writeGlb(project.path / "models/chair.glb", {.skinned = true});
    auto scene = Json::parse(readWp374aText(project.path / "scene.json"));
    auto left = scene["scenes"]["main"]["objects"][0];
    left["name"] = "Left";
    left["components"][0]["pos"] = {-2.0, 0.0, 8.0};
    auto right = left;
    right["name"] = "Right";
    right["components"][0]["pos"] = {2.0, 0.0, 8.0};
    scene["scenes"]["main"]["objects"] = Json::array({left, right});
    writeWp374aText(project.path / "scene.json", scene.dump());
    const auto feature_path = project.path / "features/wp377_profile_owner.json";
    auto feature = Json::parse(readWp374aText(feature_path));
    feature["render_targets"][0]["usage"].push_back("TRANSFER_SRC");
    writeWp374aText(feature_path, feature.dump());
    auto raygen = RayTracingRt31Fixture::wp366RaygenSource(
        "PELICAN_RAY_TYPE_primary", "PELICAN_MISS_sky", "pelican_resource_wp366_a");
    const std::string lane = "float x = pixel.x == 0 ? -2.0 : (pixel.x == 1 ? 2.0 : 8.0);";
    raygen.replace(raygen.find(lane), lane.size(),
        "float x = pixel.x == 0 ? -0.9 : (pixel.x == 1 ? 3.6 : -1.7);");
    const std::string origin = "vec3(x, 0.0, 0.0)";
    raygen.replace(raygen.find(origin), origin.size(), "vec3(x, 0.5, 0.0)");
    const std::string opaque_flag = "gl_RayFlagsOpaqueEXT";
    raygen.replace(raygen.find(opaque_flag), opaque_flag.size(), "gl_RayFlagsNoneEXT");
    writeWp374aText(project.path / "shaders/wp366_a.rgen", raygen);

    FastModuleContainer modules;
    Wp380RuntimeCleanup cleanup;
    loadWp374aScene(project.path);
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto sample = [&] {
        (void)renderWp374aProductionRows();
        const auto image = GET_MODULE(Renderer).readRenderTargetForTesting(
            "wp366_a", ImageSubresourceRange{});
        REQUIRE(image.bytes.size() >= 12 * sizeof(std::uint32_t));
        std::array<std::uint32_t, 12> pixels{};
        std::memcpy(pixels.data(), image.bytes.data(), sizeof(pixels));
        return std::array{pixels[1] != 0xffffffffu, pixels[5] != 0xffffffffu,
                          pixels[9] != 0xffffffffu};
    };
    const auto initial = sample();
    const auto first_as = GET_MODULE(Renderer).rayQueryAccelerationStructureDiagnosticsForTesting();
    CHECK(first_as.blas_build_count == 2);
    CHECK(first_as.blas_update_count == 0);
    CHECK_FALSE(initial[0]);
    CHECK_FALSE(initial[1]);
    CHECK(initial[2]);
    auto inventory = instances.rayQueryGeometryInstances();
    REQUIRE(inventory.size() == 2);
    REQUIRE(inventory[0].geometry_allocation_id == inventory[1].geometry_allocation_id);
    if (inventory[0].world_transform[3].x > inventory[1].world_transform[3].x)
        std::swap(inventory[0], inventory[1]);
    const auto left_id = instances.modelInstanceIdAt(inventory[0].instance_index);
    const auto right_id = instances.modelInstanceIdAt(inventory[1].instance_index);
    REQUIRE(left_id.has_value());
    REQUIRE(right_id.has_value());
    REQUIRE(*left_id != *right_id);
    const auto publish_morph = [&](float weight, std::uint64_t revision) {
        PublishMorphWeightFrameDescV1 frame;
        frame.layout_generation = instances.morphLayoutGenerationForTesting(*left_id);
        frame.frame_revision = revision;
        frame.weights = &weight;
        frame.weight_count = 1;
        REQUIRE(instances.publishMorphWeightFrame(*left_id, frame) == Animation::Status::ok);
    };
    glm::mat4 left_skin{1.0F};
    left_skin[3].x = 0.3F;
    glm::mat4 right_skin{1.0F};
    right_skin[3].x = 1.1F;
    instances.setSkinningPalette(*left_id, std::array{left_skin});
    instances.setSkinningPalette(*right_id, std::array{right_skin});
    publish_morph(1.0F, 1);
    // The left ray requires both morph and skin; the right uses skin alone.
    // Sharing either instance's output makes at least one of these rays miss.
    const auto deformed = sample();
    const auto updated_as = GET_MODULE(Renderer).rayQueryAccelerationStructureDiagnosticsForTesting();
    CHECK(updated_as.blas_build_count == first_as.blas_build_count);
    CHECK(updated_as.blas_update_count == 2);
    CHECK(deformed[0]);
    CHECK(deformed[1]);
    CHECK(deformed[2]);
    publish_morph(0.0F, 2);
    instances.setSkinningPalette(*right_id, std::array{glm::mat4{1.0F}});
    const auto restored = sample();
    CHECK(GET_MODULE(Renderer).rayQueryAccelerationStructureDiagnosticsForTesting().blas_update_count == 4);
    CHECK_FALSE(restored[0]);
    CHECK_FALSE(restored[1]);
    CHECK(restored[2]);
#else
    SUCCEED("RT5 production shader fixture requires the runtime compiler");
#endif
}

TEST_CASE("RT5 VAT ray hits follow interpolated playback with loop and clamp",
          "[rt5][vat][ray-tracing][production][gpu]") {
#if PELICAN_RUNTIME_SHADER_COMPILER && PELICAN_WITH_VAT
    const bool loop = GENERATE(false, true);
    CAPTURE(loop);
    setupLogger();
    Wp374aProjectRoot project{"rt5_vat"};
    writeWp374aProject(project.path, 2, true, false, true, std::nullopt, true);
    TestVatFixture::writeTinyVatGlb(project.path / "models/chair.glb", loop);
    auto scene = Json::parse(readWp374aText(project.path / "scene.json"));
    scene["scenes"]["main"]["objects"][0]["components"][0]["pos"] = {0.0, 0.0, 8.0};
    writeWp374aText(project.path / "scene.json", scene.dump());
    const auto feature_path = project.path / "features/wp377_profile_owner.json";
    auto feature = Json::parse(readWp374aText(feature_path));
    feature["render_targets"][0]["usage"].push_back("TRANSFER_SRC");
    writeWp374aText(feature_path, feature.dump());
    auto raygen = RayTracingRt31Fixture::wp366RaygenSource(
        "PELICAN_RAY_TYPE_primary", "PELICAN_MISS_sky", "pelican_resource_wp366_a");
    const std::string lane = "float x = pixel.x == 0 ? -2.0 : (pixel.x == 1 ? 2.0 : 8.0);";
    raygen.replace(raygen.find(lane), lane.size(),
        "float x = pixel.x == 0 ? -0.65 : (pixel.x == 1 ? -0.25 : 0.15);");
    const std::string opaque_flag = "gl_RayFlagsOpaqueEXT";
    raygen.replace(raygen.find(opaque_flag), opaque_flag.size(), "gl_RayFlagsNoneEXT");
    writeWp374aText(project.path / "shaders/wp366_a.rgen", raygen);

    FastModuleContainer modules;
    Wp380RuntimeCleanup cleanup;
    loadWp374aScene(project.path);
    const auto sample = [&](double time) {
        GET_MODULE(EngineTime).setTime(time);
        (void)renderWp374aProductionRows();
        const auto image = GET_MODULE(Renderer).readRenderTargetForTesting(
            "wp366_a", ImageSubresourceRange{});
        REQUIRE(image.bytes.size() >= 12 * sizeof(std::uint32_t));
        std::array<std::uint32_t, 12> pixels{};
        std::memcpy(pixels.data(), image.bytes.data(), sizeof(pixels));
        return std::array{pixels[1] != 0xffffffffu, pixels[5] != 0xffffffffu,
                          pixels[9] != 0xffffffffu};
    };
    // Each lane intersects only one of the start, interpolated, and final poses.
    // Reading bind-pose vertices or choosing a whole VAT frame fails this sequence.
    CHECK((sample(0.0) == std::array{true, false, false}));
    CHECK((sample(0.5) == std::array{false, true, false}));
    CHECK((sample(1.0) == std::array{false, false, true}));
    CHECK((sample(1.5) == (loop ? std::array{false, true, false}
                               : std::array{false, false, true})));
    CHECK((sample(2.5) == (loop ? std::array{false, true, false}
                               : std::array{false, false, true})));
    CHECK((sample(3.0) == std::array{false, false, true}));
#else
    SUCCEED("RT5 VAT production fixture requires VAT and the runtime compiler");
#endif
}

TEST_CASE("WP382d authored occurrence masks and custom indices reach the production GPU",
          "[WP382d][ray-tracing][rt3-1][production][gpu]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    Wp374aProjectRoot project{"wp382d_occurrence_masks"};
    writeWp374aProject(project.path, 2, true, true, true,
        Json{{"name", "pelican_ray_instance"}, {"mask", 3}}, true);
    const auto feature_path = project.path / "features" / "wp377_profile_owner.json";
    auto feature = Json::parse(readWp374aText(feature_path));
    feature["render_targets"][0]["usage"].push_back("TRANSFER_SRC");
    writeWp374aText(feature_path, feature.dump());
    // Reuse the existing importer fixture, retaining two named occurrences of
    // the same material at two independently visible positions.
    auto glb = TestGltfFragmentFixture::makeGlb(false, false, false, true);
    std::uint32_t json_size = 0;
    std::memcpy(&json_size, glb.data() + 12, sizeof(json_size));
    auto model = Json::parse(glb.begin() + 20, glb.begin() + 20 + json_size);
    model["nodes"][0]["translation"] = {-2.0, 0.0, 8.0};
    model["nodes"][2]["translation"] = {2.0, 0.0, 8.0};
    model["meshes"][1]["primitives"][0]["material"] = 0;
    auto json_bytes = model.dump();
    while (json_bytes.size() % 4) json_bytes.push_back(' ');
    const std::vector<std::uint8_t> bin_chunk(glb.begin() + 20 + json_size, glb.end());
    glb.clear();
    for (const auto word : {0x46546c67u, 2u,
             static_cast<std::uint32_t>(20 + json_bytes.size() + bin_chunk.size()),
             static_cast<std::uint32_t>(json_bytes.size()), 0x4e4f534au})
        TestGltfFragmentFixture::detail::appendU32(glb, word);
    glb.insert(glb.end(), json_bytes.begin(), json_bytes.end());
    glb.insert(glb.end(), bin_chunk.begin(), bin_chunk.end());
    TestGltfFragmentFixture::detail::writeBytes(project.path / "models" / "chair.glb", glb);

    auto raygen = RayTracingRt31Fixture::wp366RaygenSource(
        "PELICAN_RAY_TYPE_primary", "PELICAN_MISS_sky", "pelican_resource_wp366_a");
    const std::string lane = "float x = pixel.x == 0 ? -2.0 : (pixel.x == 1 ? 2.0 : 8.0);";
    raygen.replace(raygen.find(lane), lane.size(),
        "float x = (pixel.x % 2) == 0 ? -2.0 : 2.0;");
    const std::string mask = "gl_RayFlagsOpaqueEXT, 0xffu";
    raygen.replace(raygen.find(mask), mask.size(),
        "gl_RayFlagsNoneEXT, pixel.x < 2 ? 1u : 2u");
    writeWp374aText(project.path / "shaders" / "wp366_a.rgen", raygen);

    FastModuleContainer modules;
    Wp380RuntimeCleanup cleanup;
    loadWp374aScene(project.path);
    (void)renderWp374aProductionRows();
    const auto catalog = GET_MODULE(PolygonInstanceContainer).rayOccurrences();
    REQUIRE(catalog.size() == 2);
    REQUIRE(catalog[0].material == catalog[1].material);
    auto candidate = GET_MODULE(PolygonInstanceContainer).snapshotPreparedScene();
    auto candidate_declarations =
        std::make_shared<RaySceneDeclarationCatalog>(*candidate.ray_scene_declarations);
    for (auto &[object, row] : *candidate_declarations)
        row.value.mask = 0;
    candidate.ray_scene_declarations = std::move(candidate_declarations);
    const auto candidate_inventory = GET_MODULE(PolygonInstanceContainer)
        .rayQueryGeometryInstances(&candidate);
    REQUIRE(candidate_inventory.size() == 2);
    CHECK(candidate_inventory[0].ray_instance->mask == 0);
    CHECK(GET_MODULE(PolygonInstanceContainer).rayQueryGeometryInstances()[0]
        .ray_instance->mask == 3);
    auto document = Json::parse(readWp374aText(project.path / "scene.json"));
    auto &declaration = document["scenes"]["main"]["objects"][0]["components"][2];
    declaration["overrides"] = {{"$list", {{"v", 1}, {"next_id", 3}}},
        {"rows", Json::array()}};
    for (std::size_t index = 0; index < catalog.size(); ++index) {
        const bool left = catalog[index].node_path.front() == "RootA";
        declaration["overrides"]["rows"].push_back({
            {"$row", {{"id", index + 1}, {"v", 1}}},
            {"occurrence_id", catalog[index].occurrence_id},
            {"mask", left ? 1 : 2}, {"custom_index", left ? 71 : 82}});
    }
    const auto apply = [&] {
        return GET_MODULE(ProjectBasicConfig).importSceneDocument(document.dump(),
            [&](SceneLoadRequest &&request) {
                request.scene_id = "main";
                return GET_MODULE(SceneLoader).load(std::move(request));
            });
    };
    REQUIRE(std::holds_alternative<SceneLoadCommitted>(apply().load_result));
#if PELICAN_WITH_RPC
    struct RowPublicationFailure final : EditorProjectionFaultInjector {
        bool armed = false;
        bool fired = false;
        bool shouldFail(EditorProjectionFaultPoint point,
                        std::string_view adapter_name) noexcept override {
            if (!armed || point != EditorProjectionFaultPoint::AfterPublication ||
                adapter_name != "scene_pair") return false;
            armed = false;
            fired = true;
            return true;
        }
    } fault;
    auto service = makeEditorRuntimeService(
        EditorRuntimeServiceOptions{.projection_fault_injector = &fault});
    std::uint64_t rpc_id = 382000;
    const auto tree = runWp374aRpcBytes(*service, rpc_id++, "scene_tree", Json::object());
    const auto inventory_query = [&] {
        return runWp374aRpcBytes(*service, rpc_id++, "get_ray_occurrences",
            {{"name", document.at("scenes").at("main").at("objects").at(0).at("name")}});
    };
    // Read immediately after scene commit, before ECS update or rendering.
    const auto committed_inventory = inventory_query();
    REQUIRE(committed_inventory.at("overrides").size() == 2);
    CHECK(committed_inventory.at("rejected_rows").empty());
    for (const auto &row : committed_inventory.at("overrides")) {
        CHECK(row.at("address").at("component").at("type") == "pelican_ray_instance");
        CHECK(row.at("address").at("component").at("occurrence") == 2);
        CHECK(row.at("address").at("list_path") == "/overrides");
        REQUIRE(row.at("q").is_number_unsigned());
    }
#endif
    const auto overridden = renderWp374aProductionRows();
    REQUIRE(overridden.occurrence_overrides.size() == 2);
    for (const auto &row : overridden.occurrence_overrides) {
        REQUIRE(row.q.has_value());
        CHECK(overridden.rows.at(*row.q).instance_mask == row.mask);
    }
    auto output = GET_MODULE(Renderer).readRenderTargetForTesting("wp366_a", ImageSubresourceRange{});
    REQUIRE(output.bytes.size() >= 16 * sizeof(std::uint32_t));
    std::array<std::uint32_t, 16> pixels{};
    std::memcpy(pixels.data(), output.bytes.data(), sizeof(pixels));
    CHECK(pixels[1] == 71); // left, ray mask 1
    CHECK(pixels[5] == 0xffffffffu); // right, ray mask 1
    CHECK(pixels[9] == 0xffffffffu); // left, ray mask 2
    CHECK(pixels[13] == 82); // right, ray mask 2

#if PELICAN_WITH_RPC
    for (const auto &entry : committed_inventory.at("entries")) {
        const bool left = entry.at("node_path").at(0) == "RootA";
        REQUIRE(entry.at("q").is_number_unsigned());
        const auto q = entry.at("q").get<std::uint32_t>();
        const auto row = std::ranges::find(overridden.rows, q, &RaySceneRow::q);
        REQUIRE(row != overridden.rows.end());
        CHECK(entry.at("effective").at("mask") == (left ? 1 : 2));
        CHECK(entry.at("effective").at("custom_index") == (left ? 71 : 82));
        CHECK(row->instance_mask == (left ? 1 : 2));
        CHECK(row->instance_custom_index == (left ? 71 : 82));
    }
    const auto session = runWp374aRpcBytes(*service, rpc_id++, "open_editor_session",
                                         {{"display_name", "WP382d row deletion"}});
    auto operations = Json::array();
    for (const auto &row : committed_inventory.at("overrides")) {
        auto address = row.at("address");
        address["authoring_object_id"] = address.at("object");
        address.erase("object");
        operations.push_back({{"op", "remove_component_field"}, {"address", address}});
    }
    const auto before_plan = GET_MODULE(PolygonInstanceContainer).rayAsPlan();
    const auto before_inventory = GET_MODULE(PolygonInstanceContainer).rayAsInventory();
    const auto before_rpc = inventory_query();
    fault.armed = true;
    const auto rejected_edit = finishWp374aEdit(*service, rpc_id,
        session.at("actor_id").get<std::uint64_t>(),
        tree.at("scene_revision").get<std::uint64_t>(), operations);
    REQUIRE(rejected_edit.at("status") == "failed");
    REQUIRE(fault.fired);
    CHECK(GET_MODULE(PolygonInstanceContainer).rayAsPlan() == before_plan);
    CHECK(GET_MODULE(PolygonInstanceContainer).rayAsInventory() == before_inventory);
    CHECK(inventory_query() == before_rpc);
    const auto edited = finishWp374aEdit(*service, rpc_id,
        session.at("actor_id").get<std::uint64_t>(),
        tree.at("scene_revision").get<std::uint64_t>(), std::move(operations));
    REQUIRE(edited.at("status") == "committed");
    const auto erased_inventory = inventory_query();
    CHECK(erased_inventory.at("overrides").empty());
    for (const auto &entry : erased_inventory.at("entries"))
        CHECK(entry.at("effective").at("mask") == 3);
#else
    declaration["overrides"]["rows"] = Json::array();
    REQUIRE(std::holds_alternative<SceneLoadCommitted>(apply().load_result));
#endif
    const auto restored = renderWp374aProductionRows();
    REQUIRE(restored.rows.size() == 2);
    for (const auto &row : restored.rows) CHECK(row.instance_mask == 3);
    output = GET_MODULE(Renderer).readRenderTargetForTesting("wp366_a", ImageSubresourceRange{});
    std::memcpy(pixels.data(), output.bytes.data(), sizeof(pixels));
    for (const auto lane_index : {1, 5, 9, 13}) CHECK(pixels[lane_index] != 0xffffffffu);
#if PELICAN_WITH_RPC
    service.reset();
#endif
    declaration["overrides"] = {{"$list", {{"v", 1}, {"next_id", 4}}},
        {"rows", Json::array({{{"$row", {{"v", 1}, {"id", 3}}},
            {"occurrence_id", "ffffffffffffffffffffffffffffffff"}, {"mask", 0}}})}};
    const auto unresolved_import = apply();
    REQUIRE(std::holds_alternative<SceneLoadCommitted>(unresolved_import.load_result));
    const auto &diagnostics = std::get<SceneLoadCommitted>(unresolved_import.load_result).diagnostics;
    REQUIRE(diagnostics.rejected_rows.size() == 1);
    CHECK(diagnostics.rejected_rows[0].address.row_id == 3);
    CHECK(diagnostics.rejected_rows[0].address.component_witness.at("occurrence") == 2);
    CHECK(diagnostics.rejected_rows[0].error.at("code") == "OccurrenceAbsent");
#if PELICAN_WITH_RPC
    service = makeEditorRuntimeService();
    const auto unresolved_inventory = inventory_query();
    REQUIRE(unresolved_inventory.at("rejected_rows").size() == 1);
    CHECK(unresolved_inventory.at("rejected_rows").at(0).at("error").at("code") == "OccurrenceAbsent");
    CHECK(unresolved_inventory.at("overrides").at(0).at("q").is_null());
    service.reset();
#endif
    const auto unresolved_rows = renderWp374aProductionRows();
    for (const auto &row : unresolved_rows.rows) CHECK(row.instance_mask == 3);
    // The scene commit must supply both halves before a frame can repair them.
    auto &loader = GET_MODULE(SceneLoader);
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    for (const auto mutation : {scene_load_mutation::ray_occurrence_catalog,
                                scene_load_mutation::ray_as_plan}) {
        INFO("removed participant: " << mutation);
        test_support::ManifestSceneLoadFault injector{mutation};
        struct ResetInjector {
            SceneLoader &loader;
            ~ResetInjector() {
                test_support::setSceneLoadFaultInjectorForTesting(loader, nullptr);
            }
        } reset{loader};
        const auto complete = [&] {
            const auto plan = instances.rayAsPlan();
            const auto inventory = instances.rayAsInventory();
            return plan && inventory && plan->rows.size() == 2 &&
                   inventory->instances.size() == 2;
        };
        REQUIRE(complete());
        injector.setEnabled(true);
        test_support::setSceneLoadFaultInjectorForTesting(loader, &injector);
        REQUIRE(std::holds_alternative<SceneLoadCommitted>(apply().load_result));
        CHECK(injector.saw(SceneLoadFaultPhase::prepare, "ray"));
        CHECK_FALSE(complete());
        injector.setEnabled(false);
        REQUIRE(std::holds_alternative<SceneLoadCommitted>(apply().load_result));
        CHECK(complete());
    }
    loader.requestLoad("main");
    REQUIRE(loader.applyPendingLoad());
    const auto pending_diagnostics = loader.diagnosticsStatusJson().at("pending");
    REQUIRE(pending_diagnostics.at("rejected_rows").size() == 1);
    CHECK(pending_diagnostics.at("rejected_rows").at(0).at("address").at("row_id") == 3);
    CHECK(pending_diagnostics.at("rejected_rows").at(0).at("error").at("code") == "OccurrenceAbsent");
    GET_MODULE(VulkanManageCore).waitIdle();
#else
    SUCCEED("layout-v2 production execution requires the runtime compiler");
#endif
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

TEST_CASE("WP386b procedural record edits preserve AABB inputs and bounds edits preserve records",
          "[WP386b][ray-tracing][production][gpu]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    Wp374aProjectRoot project{"wp386b_change_matrix"};
    writeWp386aProceduralProject(project.path);
    FastModuleContainer modules;
    Wp380RuntimeCleanup cleanup;
    loadWp374aScene(project.path);
    auto &procedural = GET_MODULE(ProceduralGeometryContainer);
    auto &vkcore = GET_MODULE(VulkanManageCore);
    auto assets = parseAssetDataFormatJson(Json::parse(readWp374aText(project.path / "assets.json")));
    const auto *scene = GET_MODULE(ProjectBasicConfig).resolvedScene().findScene("main");
    REQUIRE(scene != nullptr);
    // Exercise changed asset declarations at the real participant boundary.
    // ProjectBasicConfig currently caches asset JSON; this is not an asset-file reload test.
    const auto capture = [&] {
        (void)renderWp374aProductionRows();
        const auto runtime = GET_MODULE(FrameGraphRuntimeContainer).snapshot();
        REQUIRE(runtime);
        REQUIRE(runtime->gpu_arena);
        const auto owner = std::ranges::find_if(runtime->gpu_arena->scopes,
            [](const auto &scope) { return scope.ray_tracing_project != nullptr; });
        REQUIRE(owner != runtime->gpu_arena->scopes.end());
        const auto *root = owner->acceleration_structures->activeGenerationRootForTesting();
        REQUIRE(root);
        REQUIRE(root->inventory);
        REQUIRE(root->resolved_rows);
        REQUIRE(root->sbt);
        REQUIRE(root->sbt->pipelines.size() == 1);
        REQUIRE(root->geometry_table);
        CHECK(root->inventory->generation == root->generation);
        CHECK(root->geometry_table->generation == root->generation);
        CHECK(root->sbt->generation == root->generation);
        CHECK(root->tlas_plan_generation == root->generation);
        REQUIRE(root->inventory->instances.size() == 1);
        REQUIRE(root->resolved_rows->rows.size() == 1);
        return *root;
    };
    const auto original = capture();
    SceneLoadPhaseTrace trace;
    const auto publish = [&] {
        auto prepared = procedural.prepareScene(assets, *scene, &vkcore,
            procedural.allocationGeneration() + 1, trace);
        procedural.publishPrepared(prepared, trace);
        procedural.enqueueRetirement(std::move(prepared), trace);
    };
    assets.procedural_geometry.front().record["radius"] = 2.0;
    publish();
    const auto record_edit = capture();
    CHECK(record_edit.generation != original.generation);
    CHECK(record_edit.resolved_rows->identity_digest_sha256 != original.resolved_rows->identity_digest_sha256);
    CHECK(bool(record_edit.resolved_rows->rows.front().record_data != original.resolved_rows->rows.front().record_data));
    CHECK(bool(record_edit.sbt->pipelines.front().packed_hit.bytes != original.sbt->pipelines.front().packed_hit.bytes));
    const auto &original_geometry = original.inventory->instances.front();
    const auto &record_geometry = record_edit.inventory->instances.front();
    CHECK(record_geometry.geometry_lease == original_geometry.geometry_lease);
    CHECK(record_geometry.aabb_buffer_address == original_geometry.aabb_buffer_address);
    CHECK(record_geometry.geometry_allocation_id == original_geometry.geometry_allocation_id);
    CHECK(record_geometry.world_transform == original_geometry.world_transform);
    CHECK(record_edit.tlas_plan == original.tlas_plan);

    assets.procedural_geometry.front().aabb_max[0] += 1.0f;
    publish();
    const auto bounds_edit = capture();
    CHECK(bounds_edit.generation != record_edit.generation);
    CHECK(bool(bounds_edit.resolved_rows->rows.front().record_data == record_edit.resolved_rows->rows.front().record_data));
    CHECK(bool(bounds_edit.sbt->pipelines.front().packed_hit.bytes == record_edit.sbt->pipelines.front().packed_hit.bytes));
    const auto &bounds_geometry = bounds_edit.inventory->instances.front();
    CHECK(bounds_geometry.geometry_lease != record_geometry.geometry_lease);
    CHECK(bounds_geometry.aabb_buffer_address != record_geometry.aabb_buffer_address);
    CHECK(bounds_geometry.geometry_allocation_id != record_geometry.geometry_allocation_id);
    CHECK(bounds_geometry.world_transform == record_geometry.world_transform);
    CHECK(bounds_edit.tlas_plan == record_edit.tlas_plan);

    // Publication rollback must retain the old buffer and record together.
    const auto before_rollback = procedural.proceduralRayInstances();
    const auto before_generation = procedural.allocationGeneration();
    assets.procedural_geometry.front().record["radius"] = 3.0;
    auto rejected = procedural.prepareScene(assets, *scene, &vkcore,
        before_generation + 1, trace);
    procedural.publishPrepared(rejected, trace);
    procedural.rollbackPrepared(rejected);
    CHECK(procedural.proceduralRayInstances() == before_rollback);
    CHECK(procedural.allocationGeneration() == before_generation);
#else
    SUCCEED("procedural change matrix requires the runtime compiler");
#endif
}

TEST_CASE("WP386b procedural registration order and activation rollback preserve rendered state",
          "[WP386b][ray-tracing][production][gpu]") {
#if PELICAN_RUNTIME_SHADER_COMPILER && defined(PELICAN_WP380_BEHAVIOR_FIXTURE)
    setupLogger();
    Wp374aProjectRoot project{"wp386b_activation"};
    writeWp386aProceduralProject(project.path);
    auto document = Json::parse(readWp374aText(project.path / "scene.json"));
    auto &objects = document["scenes"]["main"]["objects"];
    auto second = objects.front();
    second["name"] = "SecondSphere";
    objects.push_back(second);
    writeWp374aText(project.path / "scene.json", document.dump());
    auto assets_json = Json::parse(readWp374aText(project.path / "assets.json"));
    auto alternate = assets_json["procedural_geometry"][0];
    alternate["name"] = "alternate";
    alternate["record"]["radius"] = 2.0;
    alternate["aabb"]["max"] = {18.0, 18.0, 3.0};
    assets_json["procedural_geometry"].push_back(alternate);
    writeWp374aText(project.path / "assets.json", assets_json.dump());
    auto bindings = Json::parse(readWp374aText(project.path / "ray_bindings.json"));
    bindings["bindings"] = Json::array();
    writeWp374aText(project.path / "ray_bindings.json", bindings.dump());
    FastModuleContainer modules;
    Wp380RuntimeCleanup cleanup;
    loadWp374aScene(project.path, std::filesystem::path{PELICAN_WP380_BEHAVIOR_FIXTURE});
    auto &config = GET_MODULE(ProjectBasicConfig);
    auto &loader = GET_MODULE(SceneLoader);
    auto &procedural = GET_MODULE(ProceduralGeometryContainer);
    auto &renderer = GET_MODULE(PolygonInstanceContainer);
    const auto original_rows = renderWp374aProductionRows();
    REQUIRE(original_rows.rows.size() == 2);
    CHECK(original_rows.rows[0].scene_identity != original_rows.rows[1].scene_identity);
    const auto original_instances = procedural.proceduralRayInstances();
    REQUIRE(original_instances.size() == 2);
    CHECK(original_instances[0].geometry_lease == original_instances[1].geometry_lease);
    auto reversed = *config.resolvedScene().findScene("main");
    std::reverse(reversed.objects.begin(), reversed.objects.end());
    SceneLoadPhaseTrace trace;
    auto reordered = procedural.prepareScene(parseAssetDataFormatJson(assets_json), reversed,
        &GET_MODULE(VulkanManageCore), procedural.allocationGeneration(), trace);
    procedural.publishPrepared(reordered, trace);
    procedural.enqueueRetirement(std::move(reordered), trace);
    const auto reordered_rows = renderWp374aProductionRows();
    CHECK(reordered_rows.identity_digest_sha256 == original_rows.identity_digest_sha256);
    CHECK(procedural.proceduralRayInstances() == original_instances);

    const auto old_plan = renderer.rayAsPlan();
    const auto old_inventory = renderer.rayAsInventory();
    const auto old_generation = procedural.allocationGeneration();
    const auto old_retirements = procedural.retirementCount();
    auto rejected = document;
    auto &candidate = rejected["scenes"]["main"]["objects"][0]["components"];
    for (auto &component : candidate) {
        if (component["name"] == "pelican_procedural_geometry") component["asset"] = "alternate";
    }
    candidate.push_back({{"name", "behavior"}, {"type", "wp380_scene_transaction_behavior"},
        {"params", {{"scene_tag", 3}, {"fail_after_effects", true}, {"pending_scene", "main"}}}});
    REQUIRE_THROWS_WITH(config.importSceneDocument(rejected.dump(), [&](SceneLoadRequest &&request) {
        request.scene_id = "main";
        return loader.load(std::move(request));
    }), ContainsSubstring("WP380 fixture activation failure"));
    CHECK(procedural.proceduralRayInstances() == original_instances);
    CHECK(procedural.allocationGeneration() == old_generation);
    CHECK(procedural.retirementCount() == old_retirements);
    CHECK(renderer.rayAsPlan() == old_plan);
    CHECK(renderer.rayAsInventory() == old_inventory);
    const auto restored_rows = renderWp374aProductionRows();
    CHECK(restored_rows.identity_digest_sha256 == reordered_rows.identity_digest_sha256);
#else
    SUCCEED("procedural activation acceptance requires the runtime compiler and behavior fixture");
#endif
}

TEST_CASE("WP386b production pipeline validates procedural hit attributes across stages",
          "[WP386b][ray-tracing][rt3-2b][production][gpu]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    struct Case {
        std::string_view name;
        std::string_view intersection_type;
        std::string_view closest_type;
        std::string_view any_type;
        std::string_view diagnostic;
    };
    constexpr std::array cases{
        Case{"optional anyhit absent", "vec2", "vec2", "", ""},
        Case{"optional anyhit present", "vec2", "vec2", "vec2", ""},
        Case{"intersection missing", "", "vec2", "", "mismatch"},
        Case{"closest missing", "vec2", "", "", "mismatch"},
        Case{"closest mismatch", "vec2", "vec3", "", "mismatch"},
        Case{"anyhit mismatch", "vec2", "vec2", "vec3", "mismatch"},
        Case{"attribute exceeds device limit", "mat4", "mat4", "", "too_large"},
    };
    for (const auto &entry : cases) {
        DYNAMIC_SECTION(entry.name) {
            setupLogger();
            Wp374aProjectRoot project{"wp386b_attributes"};
            writeWp386aProceduralProject(project.path);
            const auto declaration = [](std::string_view type) {
                return type.empty() ? std::string{} :
                    "hitAttributeEXT " + std::string{type} + " hit_attribute;\n";
            };
            writeWp374aText(project.path / "shaders/wp386.rint",
                "#version 460\n#extension GL_EXT_ray_tracing : require\n" +
                declaration(entry.intersection_type) + "void main() {" +
                (entry.intersection_type.empty() ? std::string{} :
                    "hit_attribute = " + std::string{entry.intersection_type} + "(0.0);") + "}\n");
            auto closest = readWp374aText(project.path / "shaders/wp386.rchit");
            const std::string original = "hitAttributeEXT vec2 hit_attribute;";
            closest.replace(closest.find(original), original.size(),
                            declaration(entry.closest_type));
            writeWp374aText(project.path / "shaders/wp386.rchit", closest);
            const auto feature_path = project.path / "features/wp386_procedural.json";
            auto feature = Json::parse(readWp374aText(feature_path));
            auto &group = feature["compute_tasks"][0]["ray_tracing"]
                                 ["hit_shaders"]["primary"]["sphere"];
            group["intersection"] = "project://shaders/wp386";
            if (!entry.any_type.empty()) {
                group["anyhit"] = "project://shaders/wp386";
                writeWp374aText(project.path / "shaders/wp386.rahit",
                    "#version 460\n#extension GL_EXT_ray_tracing : require\n"
                    "struct Payload { uint marker; uint q; uint flags; uint tint; };\n"
                    "layout(location=0) rayPayloadInEXT Payload payload;\n" +
                    declaration(entry.any_type) +
                    "layout(shaderRecordEXT, std430) buffer RayRecord {\n"
                    "uint material_index; uint geometry_index; uint geometry_kind; float radius;\n"
                    "} ray_record;\n"
                    "void main() { if (hit_attribute.x < -1e20) ignoreIntersectionEXT; }\n");
            }
            writeWp374aText(feature_path, feature.dump());
            FastModuleContainer modules;
            const auto render = [&] {
                try {
                    loadWp374aScene(project.path);
                    if (entry.diagnostic == "too_large") {
                        const auto limit = GET_MODULE(VulkanManageCore).getRuntimeCapabilities()
                            .ray_tracing_facts.max_ray_hit_attribute_size;
                        const auto count = static_cast<std::uint64_t>(limit) / 16 + 1;
                        const auto oversized = "struct OversizedAttribute { vec4 values[" +
                            std::to_string(count) + "]; };\nhitAttributeEXT OversizedAttribute hit_attribute;\n";
                        writeWp374aText(project.path / "shaders/wp386.rint",
                            "#version 460\n#extension GL_EXT_ray_tracing : require\n" + oversized +
                            "void main() { hit_attribute.values[0] = vec4(0.0); }\n");
                        auto oversized_closest = closest;
                        const auto previous = declaration(entry.closest_type);
                        oversized_closest.replace(oversized_closest.find(previous), previous.size(), oversized);
                        writeWp374aText(project.path / "shaders/wp386.rchit", oversized_closest);
                    }
                    (void)renderWp374aProductionRows();
                } catch (const std::exception &error) {
                    TestSupport::skipIfVulkanDeviceUnavailable(
                        error, "Vulkan WP386b attribute fixture unavailable");
                    throw;
                }
            };
            if (entry.diagnostic.empty()) {
                CHECK_NOTHROW(render());
            } else {
                CHECK_THROWS_WITH(render(), ContainsSubstring(
                    "pelican.procedural_hit_attribute_" +
                    std::string{entry.diagnostic} + "@1"));
            }
        }
    }
#else
    SUCCEED("WP386b GPU acceptance is compiler-ON only");
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
    const auto classification = classifyRayQueryGeometry(snapshots);
    REQUIRE_FALSE(classification.eligible_instance_indices.empty());

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
    PelicanWp380SetCallableRecordFn set_callable_record = nullptr;
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
        .set_callable_record = reinterpret_cast<PelicanWp380SetCallableRecordFn>(
            GetProcAddress(module, "pelican_wp380_fixture_set_callable_record")),
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

TEST_CASE(
    "WP382d startup retains unresolved override diagnostics through the first frame",
    "[WP382d][transaction][startup][gpu]") {
    setupLogger();
    Wp374aProjectRoot project{"wp382d_startup_diagnostics"};
    const Json declaration{
        {"name", "pelican_ray_instance"}, {"mask", 9},
        {"overrides", {{"$list", {{"v", 1}, {"next_id", 4}}},
            {"rows", Json::array({{{"$row", {{"v", 1}, {"id", 3}}},
                {"occurrence_id", "ffffffffffffffffffffffffffffffff"},
                {"mask", 0}}})}}}};
    writeWp374aProject(project.path, 2, true, true, true, declaration, true);
    const Json settings{
        {"basic_config",
         {{"window_size", {{"width", 16}, {"height", 16}}},
          {"scene_data_json", "scene.json"},
          {"asset_data_json", "assets.json"},
          {"default_scene_id", "main"},
          {"rendering_config_json", "hybrid.json"},
          {"default_rendering_pass", "main_render"}}}};
    auto &launch = FastModuleContainer::get<EngineLaunchConfig>();
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    launch.headless_frames = 1;
    FastModuleContainer::get<PathResolver>().setup(project.path, false);

    // Observe the startup result from the real first frame, before terminal
    // teardown destroys SceneLoader. No alternate startup path is used.
    std::optional<Json> startup_diagnostics;
    const ScopedRayTracingSbtWriteObserver observer{[&](const auto &) {
        startup_diagnostics = GET_MODULE(SceneLoader)
                                  .diagnosticsStatusJson().at("startup");
    }};
    PelicanCore core{settings.dump()};
    REQUIRE(core.run());
    REQUIRE(startup_diagnostics.has_value());
    const auto &rejected = startup_diagnostics->at("rejected_rows");
    REQUIRE(rejected.size() == 1);
    CHECK(rejected.at(0).at("address").at("row_id") == 3);
    CHECK(rejected.at(0).at("error").at("code") == "OccurrenceAbsent");
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
    REQUIRE(found.candidates.front().q.has_value());
    REQUIRE(rpc_found.at("q") == *found.candidates.front().q);
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

namespace Pelican {
#if PELICAN_WITH_RPC
TEST_CASE("WP382d component lifecycle publishes the candidate ray plan before a frame",
          "[WP382d][ray-tracing][rt3-1][rpc][production][component-lifecycle][gpu]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    Wp374aProjectRoot project{"wp382d_component_lifecycle"};
    writeWp374aProject(project.path, 2, true, false, true);
    FastModuleContainer modules;
    Wp380RuntimeCleanup cleanup;
    loadWp374aScene(project.path);
    auto &renderer = GET_MODULE(PolygonInstanceContainer);
    const auto original_plan = renderer.rayAsPlan();
    REQUIRE(original_plan);
    REQUIRE_FALSE(original_plan->rows.empty());
    const auto original_count = original_plan->rows.size();
    const auto occurrence = renderer.rayAsInventory()->instances.front().occurrence_id;
    REQUIRE_FALSE(occurrence.empty());

    struct PublicationFailure final : EditorProjectionFaultInjector {
        bool armed = false;
        bool fired = false;
        bool shouldFail(EditorProjectionFaultPoint point,
                        std::string_view name) noexcept override {
            if (!armed || point != EditorProjectionFaultPoint::AfterPublication ||
                name != "scene_pair") return false;
            armed = false;
            fired = true;
            return true;
        }
    } fault;
    auto service = makeEditorRuntimeService(
        EditorRuntimeServiceOptions{.projection_fault_injector = &fault});
    std::uint64_t rpc_id = 382400;
    const auto tree = runWp374aRpcBytes(*service, rpc_id++, "scene_tree", Json::object());
    const auto object = tree.at("objects").at(0).at("authoring_object_id").get<std::uint64_t>();
    const auto session = runWp374aRpcBytes(*service, rpc_id++, "open_editor_session",
                                         {{"display_name", "WP382d component lifecycle"}});
    const auto actor = session.at("actor_id").get<std::uint64_t>();
    const auto query = [&] {
        return runWp374aRpcBytes(*service, rpc_id++, "get_ray_occurrences", {{"name", "Chair"}});
    };
    const auto edit = [&](const Json &operation) {
        const auto current = runWp374aRpcBytes(*service, rpc_id++, "scene_tree", Json::object());
        return finishWp374aEdit(*service, rpc_id, actor,
            current.at("scene_revision").get<std::uint64_t>(), Json::array({operation}));
    };
    const Json override_row{{"$row", {{"id", 1}, {"v", 1}}},
                            {"occurrence_id", occurrence}};
    const Json ray_component{{"name", "pelican_ray_instance"}, {"mask", 9},
        {"overrides", {{"$list", {{"v", 1}, {"next_id", 2}}},
                       {"rows", Json::array({override_row})}}}};
    REQUIRE(edit({{"op", "add_component"}, {"object_id", object},
                  {"component", ray_component}}).at("status") == "committed");
    REQUIRE(renderer.rayAsPlan()->rows.size() == original_count);
    CHECK(renderer.rayAsPlan()->rows.front().instance_mask == 9);
    const auto added_ray = query();
    REQUIRE(added_ray.at("overrides").size() == 1);
    CHECK(added_ray.at("overrides").at(0).at("q").is_number_unsigned());

    const Json remove_model{{"op", "remove_component"}, {"object_id", object},
                            {"component_slot", "simplemodelview"}};
    const Json add_model{{"op", "add_component"}, {"object_id", object},
                         {"component", {{"name", "simplemodelview"}, {"model", "chair"}}}};
    const auto check_failed_edit = [&](const Json &operation) {
        const auto before_plan = renderer.rayAsPlan();
        const auto before_inventory = renderer.rayAsInventory();
        const auto before_rpc = query();
        const auto before_live = renderer.liveModelInstanceCountForTesting();
        fault.armed = true;
        fault.fired = false;
        const auto failed = edit(operation);
        CHECK(failed.at("status") != "committed");
        REQUIRE(fault.fired);
        CHECK(renderer.rayAsPlan() == before_plan);
        CHECK(renderer.rayAsInventory() == before_inventory);
        CHECK(renderer.liveModelInstanceCountForTesting() == before_live);
        CHECK(query() == before_rpc);
    };
    check_failed_edit(remove_model);
    REQUIRE(edit(remove_model).at("status") == "committed");
    REQUIRE(renderer.rayAsPlan()->rows.empty());
    REQUIRE(renderer.rayAsInventory()->instances.empty());
    CHECK(renderer.liveModelInstanceCountForTesting() == 0);
    const auto removed = query();
    REQUIRE(removed.at("overrides").size() == 1);
    CHECK(removed.at("overrides").at(0).at("q").is_null());
    REQUIRE(removed.at("rejected_rows").size() == 1);
    CHECK(removed.at("rejected_rows").at(0).at("error").at("code") == "OccurrenceAbsent");

    check_failed_edit(add_model);
    REQUIRE(edit(add_model).at("status") == "committed");
    REQUIRE(renderer.rayAsPlan()->rows.size() == original_count);
    CHECK(renderer.rayAsPlan()->rows.front().instance_mask == 9);
    CHECK(renderer.liveModelInstanceCountForTesting() == 1);
    const auto restored = query();
    CHECK(restored.at("rejected_rows").empty());
    REQUIRE(restored.at("overrides").size() == 1);
    CHECK(restored.at("overrides").at(0).at("q").is_number_unsigned());

    const auto entity = GET_MODULE(SceneLoader).objectId("Chair");
    REQUIRE(entity);
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    const auto model_before_frame = ecs.tryComponent<SimpleModelViewComponent>(*entity)->model_instance_id;
    GET_MODULE(ECSCore).update();
    CHECK(ecs.tryComponent<SimpleModelViewComponent>(*entity)->model_instance_id == model_before_frame);

    REQUIRE(edit({{"op", "remove_component"}, {"object_id", object},
                  {"component_slot", "pelican_ray_instance"}}).at("status") == "committed");
    REQUIRE(renderer.rayAsPlan()->rows.size() == original_count);
    CHECK(renderer.rayAsPlan()->rows.front().instance_mask == 127);
    const auto removed_ray = query();
    CHECK(removed_ray.at("overrides").empty());
    CHECK(removed_ray.at("rejected_rows").empty());
    // An unrelated authored edit must preserve an independently placed model.
    const auto transient = renderer.placeModelInstance(
        GET_MODULE(ModelAssetContainer).getModelTemplateByName("chair"));
    renderer.setTrs(transient, {8.0F, 9.0F, 10.0F},
                    glm::quat{1.0F, 0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F});
    const auto transient_matrix = renderer.currentModelMatrixForTesting(transient);
    REQUIRE(edit({{"op", "add_component"}, {"object_id", object},
                  {"component", ray_component}}).at("status") == "committed");
    CHECK((renderer.currentModelMatrixForTesting(transient) == transient_matrix));
    const auto transient_inventory = renderer.rayAsInventory();
    const auto transient_row = std::find_if(transient_inventory->instances.begin(),
        transient_inventory->instances.end(), [&](const auto &row) {
            return row.instance_index == transient.index;
        });
    REQUIRE(transient_row != transient_inventory->instances.end());
    CHECK((transient_row->world_transform == transient_matrix));
    GET_MODULE(SceneLoader).drainRetirementQueue();
    renderer.clear();
    CHECK_FALSE(renderer.rayAsPlan());
    CHECK_FALSE(renderer.rayAsInventory());
    CHECK(renderer.rayOccurrences().empty());
    // Every publication was queried before a frame; the one ECS update above
    // also verifies that a committed model is not recreated on the next tick.
    service.reset();
#else
    SUCCEED("ray component lifecycle requires the runtime compiler");
#endif
}
#endif
} // namespace Pelican

namespace Pelican {
#if PELICAN_WITH_RPC
TEST_CASE("WP382d procedural editor lifecycle publishes and rolls back AABB state with its plan",
          "[WP382d][ray-tracing][rt3-2a][rpc][production][procedural-lifecycle][gpu]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    Wp374aProjectRoot project{"wp382d_procedural_editor"};
    writeWp386aProceduralProject(project.path);
    auto assets = Json::parse(readWp374aText(project.path / "assets.json"));
    auto alternate = assets.at("procedural_geometry").at(0);
    alternate["name"] = "alternate";
    alternate["aabb"]["max"] = {18.0, 18.0, 3.0};
    alternate["record"]["radius"] = 2.0;
    assets["procedural_geometry"].push_back(alternate);
    writeWp374aText(project.path / "assets.json", assets.dump());
    auto bindings = Json::parse(readWp374aText(project.path / "ray_bindings.json"));
    // This lifecycle intentionally removes every instance of either asset.
    // Use the existing procedural default group, with no unmatched selector.
    bindings["bindings"] = Json::array();
    writeWp374aText(project.path / "ray_bindings.json", bindings.dump());
    FastModuleContainer modules;
    Wp380RuntimeCleanup cleanup;
    loadWp374aScene(project.path);
    auto &renderer = GET_MODULE(PolygonInstanceContainer);
    auto &procedural = GET_MODULE(ProceduralGeometryContainer);
    REQUIRE(procedural.instanceCount() == 1);
    REQUIRE(renderer.rayAsPlan()->rows.size() == 1);
    const auto original_record = renderer.rayAsPlan()->rows.front().record_data;
    struct PublicationFailure final : EditorProjectionFaultInjector {
        bool armed = false;
        bool fired = false;
        bool shouldFail(EditorProjectionFaultPoint point, std::string_view name) noexcept override {
            if (!armed || point != EditorProjectionFaultPoint::AfterPublication || name != "scene_pair")
                return false;
            armed = false;
            fired = true;
            return true;
        }
    } fault;
    auto service = makeEditorRuntimeService(
        EditorRuntimeServiceOptions{.projection_fault_injector = &fault});
    std::uint64_t rpc_id = 382600;
    const auto tree = runWp374aRpcBytes(*service, rpc_id++, "scene_tree", Json::object());
    const auto object = tree.at("objects").at(0).at("authoring_object_id").get<std::uint64_t>();
    const auto session = runWp374aRpcBytes(*service, rpc_id++, "open_editor_session",
                                         {{"display_name", "WP382d procedural lifecycle"}});
    const auto actor = session.at("actor_id").get<std::uint64_t>();
    const auto edit = [&](const Json &operation) {
        const auto current = runWp374aRpcBytes(*service, rpc_id++, "scene_tree", Json::object());
        return finishWp374aEdit(*service, rpc_id, actor,
            current.at("scene_revision").get<std::uint64_t>(), Json::array({operation}));
    };
    const auto fail_then_commit = [&](const Json &operation) {
        const auto old_plan = renderer.rayAsPlan();
        const auto old_inventory = renderer.rayAsInventory();
        const auto old_instances = procedural.proceduralRayInstances();
        const auto old_generation = procedural.allocationGeneration();
        const auto old_retirements = procedural.retirementCount();
        const auto loaded_bindings = GET_MODULE(SceneLoader).runtimeObjectBindings();
        const std::vector<SceneRuntimeObjectBinding> old_bindings{
            loaded_bindings.begin(), loaded_bindings.end()};
        fault.armed = true;
        fault.fired = false;
        const auto failed = edit(operation);
        CHECK(failed.at("status") != "committed");
        REQUIRE(fault.fired);
        CHECK(renderer.rayAsPlan() == old_plan);
        CHECK(renderer.rayAsInventory() == old_inventory);
        CHECK(procedural.proceduralRayInstances() == old_instances);
        CHECK(procedural.allocationGeneration() == old_generation);
        CHECK(procedural.retirementCount() == old_retirements);
        const auto restored_bindings = GET_MODULE(SceneLoader).runtimeObjectBindings();
        CHECK(std::equal(old_bindings.begin(), old_bindings.end(),
            restored_bindings.begin(), restored_bindings.end(), [](const auto &left, const auto &right) {
                return left.authoring_object_id == right.authoring_object_id && left.object_id == right.object_id;
            }));
        REQUIRE(edit(operation).at("status") == "committed");
        CHECK(renderer.rayAsPlan()->rows.size() == procedural.instanceCount());
        CHECK(renderer.rayAsInventory()->instances.size() == procedural.instanceCount());
    };
    fail_then_commit({{"op", "set_component_value"}, {"object_id", object},
        {"component_slot", "pelican_procedural_geometry"}, {"field_path", "/asset"},
        {"value", "alternate"}});
    REQUIRE(procedural.instanceCount() == 1);
    CHECK(procedural.proceduralRayInstances().front().procedural_asset == "alternate");
    CHECK(renderer.rayAsInventory()->instances.front().procedural_record.at("radius") == 2.0);
    CHECK(bool(renderer.rayAsPlan()->rows.front().record_data != original_record));

    fail_then_commit({{"op", "remove_component"}, {"object_id", object},
                      {"component_slot", "pelican_procedural_geometry"}});
    CHECK(procedural.instanceCount() == 0);
    fail_then_commit({{"op", "add_component"}, {"object_id", object},
        {"component", {{"name", "pelican_procedural_geometry"}, {"asset", "alternate"}}}});
    REQUIRE(procedural.instanceCount() == 1);
    const auto shared_buffer = procedural.proceduralRayInstances().front().geometry_lease;
    REQUIRE(shared_buffer);
    fail_then_commit({{"op", "spawn"}, {"scene_id", "main"},
        {"object", {{"name", "SecondSphere"}, {"components", Json::array({
            {{"name", "transform"}, {"pos", {4, 0, 0}},
             {"rotation", {0, 0, 0, 1}}, {"scale", {1, 1, 1}}},
            {{"name", "pelican_procedural_geometry"}, {"asset", "alternate"}}
        })}}}});
    REQUIRE(procedural.instanceCount() == 2);
    for (const auto &instance : procedural.proceduralRayInstances()) {
        CHECK(instance.geometry_lease == shared_buffer);
        CHECK(instance.aabb_buffer_address != 0);
    }
    const auto spawned_tree = runWp374aRpcBytes(*service, rpc_id++, "scene_tree", Json::object());
    const auto spawned = std::find_if(spawned_tree.at("objects").begin(), spawned_tree.at("objects").end(),
        [](const auto &entry) { return entry.at("name") == "SecondSphere"; });
    REQUIRE(spawned != spawned_tree.at("objects").end());
    fail_then_commit({{"op", "destroy"},
        {"object_id", spawned->at("authoring_object_id").template get<std::uint64_t>()}});
    REQUIRE(procedural.instanceCount() == 1);
    const auto before_ray_edit = procedural.allocationGeneration();
    REQUIRE(edit({{"op", "add_component"}, {"object_id", object},
        {"component", {{"name", "pelican_ray_instance"}, {"mask", 7}}}}).at("status") == "committed");
    CHECK(procedural.allocationGeneration() == before_ray_edit);
    CHECK(procedural.proceduralRayInstances().front().geometry_lease == shared_buffer);
    REQUIRE(renderer.rayAsPlan()->rows.size() == 1);
    CHECK(renderer.rayAsPlan()->rows.front().instance_mask == 7);
    service.reset();
    GET_MODULE(VulkanManageCore).waitIdle();
    procedural.drainRetirements();
    CHECK(procedural.retirementCount() == 0);
    CHECK(procedural.proceduralRayInstances().front().geometry_lease == shared_buffer);
#else
    SUCCEED("procedural editor lifecycle requires the runtime compiler");
#endif
}
#endif
} // namespace Pelican

namespace Pelican {
#if PELICAN_WITH_RPC && PELICAN_RUNTIME_SHADER_COMPILER
TEST_CASE("RT7 project preset pass shader edits publish to the production renderer",
          "[rt7][preset-pass-edit][production][gpu]") {
    setupLogger();
    Wp374aProjectRoot project{"rt7_preset_edit"};
    writeWp374aProject(project.path, std::nullopt, false, false, false);
    for (const auto &[name, color] : std::array{
             std::pair{"red", "1,0,0,1"}, std::pair{"green", "0,1,0,1"}}) {
        writeWp374aText(project.path / "shaders" / (std::string{name} + ".frag"),
            std::string{"#version 450\nlayout(location=0) out vec4 color;\nvoid main(){color=vec4("} + color + ");}\n");
    }
    FastModuleContainer modules;
    Wp380RuntimeCleanup cleanup;
    (void)configureWp374aRuntime(project.path);
    TestSupport::requireVulkanDevice("RT7 preset pass editing");
    GET_MODULE(ECSPredefinedRegistration).reg();
    test_support::loadScene(GET_MODULE(SceneLoader), "main");
    const auto render = [] {
        GET_MODULE(ECSCore).update();
        GET_MODULE(ECSCore).update();
        GET_MODULE(Renderer).render();
        GET_MODULE(VulkanManageCore).waitIdle();
    };
    render();
    auto service = makeEditorRuntimeService(EditorRuntimeServiceOptions{.render_config_editing = true});
    std::uint64_t rpc_id = 91000;
    auto context = runWp374aRpcBytes(*service, rpc_id++, "get_render_authoring_context", Json::object());
    const auto publish = [&](const Json &accepted) {
        INFO(accepted.dump());
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPendingEdits();
        render();
        const auto result = runWp374aRpcBytes(*service, rpc_id++, "get_edit_result", {{"ticket", accepted.at("ticket")}});
        INFO(result.dump());
        REQUIRE(result.at("committed") == true);
    };
    publish(runWp374aRpcBytes(*service, rpc_id++, "fork_render_pipeline", {
        {"base_source_digest", context.at("source_digest").at("hex")},
        {"base_preset_digest", context.at("pipeline_preset").at("source_digest").at("hex")},
        {"destination_reference", "project://pipelines/local.json"}}));
    const auto root_bytes = readWp374aText(project.path / "hybrid.json");
    for (const auto *color : {"red", "green"}) {
        context = runWp374aRpcBytes(*service, rpc_id++, "get_render_authoring_context", Json::object());
        const auto &passes = context.at("graphs").at(0).at("passes");
        const auto selected = std::find_if(passes.begin(), passes.end(), [](const auto &pass) {
            return pass.at("name") == "scene_present";
        });
        REQUIRE(selected != passes.end());
        publish(runWp374aRpcBytes(*service, rpc_id++, "edit_preset_pass", {
            {"base_source_digest", context.at("source_digest").at("hex")},
            {"base_preset_digest", context.at("pipeline_preset").at("source_digest").at("hex")},
            {"graph", "main_render"}, {"pass_name", "scene_present"}, {"input", Json::array()},
            {"output", selected->at("preset_declaration").at("output")},
            {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", std::string{"project://shaders/"} + color}}}}));
        const auto pixels = GET_MODULE(Renderer).readRenderTargetForTesting("display", ImageSubresourceRange{});
        REQUIRE(pixels.format == vk::Format::eB8G8R8A8Srgb);
        REQUIRE(pixels.bytes.size() >= 4);
        CHECK((pixels.bytes[1] > 240) == (std::string_view{color} == "green"));
        CHECK(readWp374aText(project.path / "hybrid.json") == root_bytes);
    }
    service.reset();
    service = makeEditorRuntimeService(EditorRuntimeServiceOptions{.render_config_editing = true});
    const auto reopened = runWp374aRpcBytes(*service, rpc_id++, "get_render_authoring_context", Json::object());
    CHECK(reopened.at("pipeline_preset").at("reference") == "project://pipelines/local.json");
    render();
    CHECK(GET_MODULE(Renderer).readRenderTargetForTesting("display", ImageSubresourceRange{}).bytes[1] > 240);
}

TEST_CASE("RT4 callable dispatch selects its SBT record and reloads its implementation",
          "[rt4][ray-tracing][production][gpu]") {
    const bool dynamic_records = GENERATE(false, true);
    CAPTURE(dynamic_records);
    setupLogger();
    Wp374aProjectRoot project{"rt4_callable"};
    writeWp386aProceduralProject(project.path);
    auto feature = Json::parse(readWp374aText(project.path / "features/wp386_procedural.json"));
    auto &ray = feature["compute_tasks"][0]["ray_tracing"];
    ray["callable"] = {"project://shaders/first", "project://shaders/second"};
    if (dynamic_records) {
        ray["callable_record"] = Json::array({{{"name", "increment"}, {"type", "u32"}}});
        ray["callable_records"] = Json::array({{{"increment", 100}}, {{"increment", 200}}});
        auto rendering = Json::parse(readWp374aText(project.path / "hybrid.json"));
        rendering["ray_task_callable_records"] = {{"wp386_trace",
            Json::array({{{"increment", 100}}, {{"increment", 250}}})}};
        writeWp374aText(project.path / "hybrid.json", rendering.dump());
    }
    ray["hit_shaders"]["primary"]["sphere"]["intersection"] = "project://shaders/hit";
    writeWp374aText(project.path / "features/wp386_procedural.json", feature.dump());
    writeWp374aText(project.path / "shaders/hit.rint", R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
hitAttributeEXT vec2 hit_attribute;
void main() { hit_attribute = vec2(0); reportIntersectionEXT(1.5, 0u); }
)glsl");
    auto closest = readWp374aText(project.path / "shaders/wp386.rchit");
    const std::string old_main = "void main() { payload.marker = 0xdead386au; }";
    closest.replace(closest.find(old_main), old_main.size(), R"glsl(
layout(location=0) callableDataEXT uint value;
void main() {
    value = 7u;
    executeCallableEXT(1u, 0);
    payload.marker = value;
}
)glsl");
    writeWp374aText(project.path / "shaders/wp386.rchit", closest);
    const auto callable_source = [&](std::uint32_t increment) {
        if (dynamic_records) {
            return std::string{R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
layout(location=0) callableDataInEXT uint value;
layout(shaderRecordEXT, std430) buffer CallableRecord { uint increment; } record;
void main() { value += record.increment * )glsl"} + std::to_string(increment) + "u; }\n";
        }
        return std::string{R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
layout(location=0) callableDataInEXT uint value;
void main() { value += )glsl"} + std::to_string(increment) + "u; }\n";
    };
    writeWp374aText(project.path / "shaders/first.rcall", callable_source(dynamic_records ? 1 : 100));
    writeWp374aText(project.path / "shaders/second.rcall", callable_source(dynamic_records ? 1 : 200));
    FastModuleContainer modules;
    Wp380RuntimeCleanup cleanup;
    (void)configureWp374aRuntime(project.path);
    TestSupport::requireVulkanDevice("RT4 callable dispatch");
    GET_MODULE(ECSPredefinedRegistration).reg();
#ifdef PELICAN_WP380_BEHAVIOR_FIXTURE
    if (dynamic_records) {
        GET_MODULE(EngineLaunchConfig).game_logic_dll =
            std::filesystem::path{PELICAN_WP380_BEHAVIOR_FIXTURE};
        REQUIRE(initializeConfiguredGameLogic());
    }
#endif
    test_support::loadScene(GET_MODULE(SceneLoader), "main");
    const auto marker = [&] {
        (void)renderWp374aProductionRows();
        const auto output = GET_MODULE(Renderer).readRenderTargetForTesting(
            "wp386_out", ImageSubresourceRange{});
        REQUIRE(output.bytes.size() >= sizeof(std::uint32_t));
        std::uint32_t value;
        std::memcpy(&value, output.bytes.data(), sizeof(value));
        return value;
    };
    CHECK(marker() == (dynamic_records ? 257u : 207u));
    if (dynamic_records) {
        std::size_t pipeline_creates = 0;
        ScopedRayTracingPipelineCreateObserver observer{
            [&](const RayTracingPipelineCreateObservation &) { ++pipeline_creates; }};
        const GameContext context;
#ifdef PELICAN_WP380_BEHAVIOR_FIXTURE
        const std::filesystem::path fixture_path{PELICAN_WP380_BEHAVIOR_FIXTURE};
        const auto fixture = wp380Exports(fixture_path);
        REQUIRE(fixture.set_callable_record != nullptr);
        fixture.set_callable_record("wp386_trace", 1, 400);
#else
        context.setRayTracingCallableRecord("wp386_trace", 1, Json{{"increment", 400}});
#endif
        CHECK(marker() == 407u);
        CHECK_THROWS(context.setRayTracingCallableRecord(
            "missing_ray_task", 1, Json{{"increment", 500}}));
        CHECK_THROWS(context.setRayTracingCallableRecord(
            "wp386_trace", 2, Json{{"increment", 500}}));
        CHECK_THROWS(context.setRayTracingCallableRecord(
            "wp386_trace", 1, Json{{"increment", "invalid"}}));
        CHECK(marker() == 407u);
        CHECK(pipeline_creates == 0);
    }
    writeWp374aText(project.path / "shaders/second.rcall", callable_source(dynamic_records ? 3 : 300));
    auto prepared = GET_MODULE(ShaderLibrary).prepareReloadAll();
    std::erase_if(prepared.candidates, [](const auto &candidate) {
        return candidate.replacement.source_path.filename() != "second.rcall";
    });
    REQUIRE(prepared.candidates.size() == 1);
    const auto rebuilt = GET_MODULE(PipelineFactory).rebuildPrepared(std::move(prepared));
    REQUIRE(rebuilt.committed);
    REQUIRE(rebuilt.failed_pipelines == 0);
    CHECK(marker() == (dynamic_records ? 1207u : 307u));
    if (dynamic_records) {
        auto service = makeEditorRuntimeService(EditorRuntimeServiceOptions{.render_config_editing = true});
        std::uint64_t rpc_id = 90000;
        auto before = runWp374aRpcBytes(*service, rpc_id++, "get_ray_callable_records", Json::object());
        REQUIRE(before.at("tasks").size() == 1);
        CHECK(before.at("tasks")[0].at("authored_records")[1].at("increment") == 200);
        CHECK(before.at("tasks")[0].at("records")[1].at("increment") == 250);
        const auto feature_path = project.path / "features/wp386_procedural.json";
        const auto feature_bytes = readWp374aText(feature_path);
        const Json records = Json::array({{{"increment", 100}}, {{"increment", 600}}});
        const Json mapping{{"wp386_trace", records}};
        const auto edit = [&](const Json &overrides) {
            const auto accepted = runWp374aRpcBytes(*service, rpc_id++, "edit_ray_callable_records", {
                {"base_source_digest", before.at("source_digest").at("hex")}, {"task_records", overrides}});
            REQUIRE(accepted.at("status") == "accepted");
            service->commitPendingEdits();
            (void)renderWp374aProductionRows();
            REQUIRE(runWp374aRpcBytes(*service, rpc_id++, "get_edit_result",
                {{"ticket", accepted.at("ticket")}}).at("committed") == true);
        };
        edit(mapping);
        CHECK(marker() == 1807u);
        CHECK(readWp374aText(feature_path) == feature_bytes);
        CHECK(Json::parse(readWp374aText(project.path / "hybrid.json")).at("ray_task_callable_records") == mapping);
        CHECK(runWp374aRpcBytes(*service, rpc_id++, "edit_ray_callable_records", {
            {"base_source_digest", before.at("source_digest").at("hex")},
            {"task_records", mapping}}).at("status") == "rejected");
        service.reset();
        service = makeEditorRuntimeService(EditorRuntimeServiceOptions{.render_config_editing = true});
        before = runWp374aRpcBytes(*service, rpc_id++, "get_ray_callable_records", Json::object());
        CHECK(before.at("tasks")[0].at("records") == records);
        auto invalid = mapping;
        invalid["wp386_trace"][1]["increment"] = -1;
        CHECK(runWp374aRpcBytes(*service, rpc_id++, "edit_ray_callable_records", {
            {"base_source_digest", before.at("source_digest").at("hex")},
            {"task_records", invalid}}).at("status") == "rejected");
        CHECK(marker() == 1807u);
        edit(Json::object());
        CHECK(marker() == 607u);
    }
}

TEST_CASE("RT6 procedural geometries share one BLAS with independent records",
          "[rt6][multi-aabb][ray-tracing][production][gpu]") {
    setupLogger();
    Wp374aProjectRoot project{"rt6_multi_aabb"};
    writeWp386aProceduralProject(project.path);
    auto assets_json = Json::parse(readWp374aText(project.path / "assets.json"));
    auto left = assets_json["procedural_geometry"][0];
    left["aabb"] = {{"min", {-3.5, -1.5, 6.5}}, {"max", {-0.5, 1.5, 9.5}}};
    left["record"] = {{"radius", 1.0}, {"center", {-2.0, 0.0, 8.0}}};
    auto right = left;
    right["name"] = "right_sphere";
    right["aabb"] = {{"min", {0.5, -1.5, 6.5}}, {"max", {3.5, 1.5, 9.5}}};
    right["record"] = {{"radius", 1.25}, {"center", {2.0, 0.0, 8.0}}};
    assets_json["procedural_geometry"] = Json::array({left, right});
    writeWp374aText(project.path / "assets.json", assets_json.dump());
    auto scene_json = Json::parse(readWp374aText(project.path / "scene.json"));
    for (auto &component : scene_json["scenes"]["main"]["objects"][0]["components"])
        if (component.at("name") == "pelican_procedural_geometry") {
            component.erase("asset");
            component["assets"] = {left.at("name"), "right_sphere"};
        }
    scene_json["scenes"]["main"]["objects"][0]["components"].push_back(
        {{"name", "pelican_ray_instance"}, {"custom_index", 71}});
    writeWp374aText(project.path / "scene.json", scene_json.dump());
    auto layout = Json::parse(readWp374aText(project.path / "ray_layout.json"));
    layout["record"].push_back({{"name", "center"}, {"type", "vec3"}});
    writeWp374aText(project.path / "ray_layout.json", layout.dump());
    auto feature = Json::parse(readWp374aText(project.path / "features/wp386_procedural.json"));
    feature["compute_tasks"][0]["ray_tracing"]["hit_shaders"]["primary"]["sphere"]["intersection"] = "engine://procedural_sphere";
    writeWp374aText(project.path / "features/wp386_procedural.json", feature.dump());
    writeWp374aText(project.path / "shaders/wp386.rchit", R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require
#include "__pelican_ray_group_layout.glsl"
struct Payload { uint marker; uint q; uint flags; uint tint; };
layout(location=0) rayPayloadInEXT Payload payload;
hitAttributeEXT vec2 hit_attribute;
layout(shaderRecordEXT, std430) buffer SphereRecord {
    PELICAN_RAY_ENGINE_RECORD_MEMBERS
    float radius;
    vec3 center;
} sphere;
void main() {
    payload.marker = 0xaabb6u;
    payload.q = sphere.geometry_index;
    payload.flags = gl_GeometryIndexEXT;
    payload.tint = floatBitsToUint(sphere.radius);
}
)glsl");
    FastModuleContainer modules;
    Wp380RuntimeCleanup cleanup;
    loadWp374aScene(project.path);
    const auto sample = [&](float left_radius) {
        const auto rows = renderWp374aProductionRows();
        REQUIRE(rows.rows.size() == 2);
        CHECK(rows.rows[0].scene_identity == rows.rows[1].scene_identity);
        CHECK(rows.rows[0].geometry_index != rows.rows[1].geometry_index);
        const auto image = GET_MODULE(Renderer).readRenderTargetForTesting("wp386_out", ImageSubresourceRange{});
        std::array<std::uint32_t, 12> pixels{};
        REQUIRE(image.bytes.size() >= sizeof(pixels));
        std::memcpy(pixels.data(), image.bytes.data(), sizeof(pixels));
        CHECK(pixels[0] == 0xaabb6u);
        CHECK(pixels[4] == 0xaabb6u);
        CHECK(pixels[8] == 0x386au);
        REQUIRE(pixels[1] < 2u);
        REQUIRE(pixels[5] < 2u);
        CHECK(pixels[1] != pixels[5]);
        // Scene geometry-table indices and BLAS-local geometry indices are
        // independent; asset names can change their relative ordering.
        CHECK(pixels[2] < 2u);
        CHECK(pixels[6] < 2u);
        CHECK(pixels[2] != pixels[6]);
        CHECK(std::bit_cast<float>(pixels[3]) == left_radius);
        CHECK(std::bit_cast<float>(pixels[7]) == 1.25f);
        const auto diagnostics = GET_MODULE(Renderer).rayQueryAccelerationStructureDiagnosticsForTesting();
        CHECK(diagnostics.active_blas_count == 1);
        CHECK(diagnostics.tlas_instance_count == 1);
        REQUIRE(diagnostics.active_blas_inputs.size() == 1);
        CHECK(diagnostics.active_blas_inputs[0].geometry_kind == RayGeometryKind::aabbs);
        CHECK(diagnostics.active_blas_inputs[0].geometry_count == 2);
        return diagnostics;
    };
    auto initial = sample(1.0f);
    CHECK(sample(1.0f).blas_build_count == initial.blas_build_count);
    auto service = makeEditorRuntimeService({.render_config_editing = true});
    std::uint64_t rpc_id = 386600;
    const auto catalog = [&] {
        return runWp374aRpcBytes(*service, rpc_id++, "get_procedural_assets", Json::object());
    };
    const auto finish = [&](const Json &submitted) {
        if (submitted.at("status") != "accepted") return submitted;
        service->commitPendingEdits();
        return runWp374aRpcBytes(*service, rpc_id++, "get_edit_result", {{"ticket", submitted.at("ticket")}});
    };
    const auto edit = [&](const Json &fields) {
        auto request = fields;
        request["asset_name"] = "sphere_asset";
        request["base_source_digest"] = catalog().at("source_digest").at("hex");
        return finish(runWp374aRpcBytes(*service, rpc_id++, "edit_procedural_asset", request));
    };
    {
        auto create = Json{{"operation", "create"}, {"asset_name", "created_sphere"},
            {"aabb", right.at("aabb")}, {"record", right.at("record")},
            {"base_source_digest", catalog().at("source_digest").at("hex")}};
        const auto created = finish(runWp374aRpcBytes(*service, rpc_id++, "edit_procedural_asset", create));
        INFO(created.dump());
        REQUIRE(created.at("status") == "committed");
        CHECK(catalog().at("assets").size() == 3);
        CHECK(Json::parse(GET_MODULE(ProjectBasicConfig).assetDataJson()).at("procedural_geometry").size() == 3);
        create["base_source_digest"] = catalog().at("source_digest").at("hex");
        const auto duplicate = runWp374aRpcBytes(*service, rpc_id++, "edit_procedural_asset", create);
        REQUIRE(duplicate.at("status") == "rejected");
        CHECK(duplicate.at("error").at("code") == "asset_already_exists");
        CHECK(sample(1.0f).blas_build_count == initial.blas_build_count);
        const auto tree = runWp374aRpcBytes(*service, rpc_id++, "scene_tree", Json::object());
        const auto session = runWp374aRpcBytes(*service, rpc_id++, "open_editor_session", {{"display_name", "Create procedural asset"}});
        const auto attached = finishWp374aEdit(*service, rpc_id, session.at("actor_id").get<std::uint64_t>(),
            tree.at("scene_revision").get<std::uint64_t>(), Json::array({
                {{"op", "remove_component"}, {"object_id", tree.at("objects")[0].at("authoring_object_id")},
                 {"component_slot", "pelican_procedural_geometry"}},
                {{"op", "add_component"}, {"object_id", tree.at("objects")[0].at("authoring_object_id")},
                 {"component", {{"name", "pelican_procedural_geometry"},
                                {"assets", {"sphere_asset", "created_sphere"}}}}}}));
        INFO(attached.dump());
        REQUIRE(attached.at("status") == "committed");
        const auto instances = GET_MODULE(ProceduralGeometryContainer).proceduralRayInstances();
        CHECK(std::any_of(instances.begin(), instances.end(), [](const auto &instance) {
            return instance.procedural_asset == "created_sphere";
        }));
        initial = sample(1.0f);
    }
    const auto entity = GET_MODULE(SceneLoader).objectId("Sphere");
    REQUIRE(entity);
    auto *live_transform = GET_MODULE(ECSCore).getTemplatePublicModule().tryComponent<TransformComponent>(*entity);
    REQUIRE(live_transform);
    live_transform->pos.z = 2.0f;
    const auto check_live_transform = [&] {
        const auto inventory = GET_MODULE(PolygonInstanceContainer).rayAsInventory();
        REQUIRE(inventory);
        REQUIRE(inventory->instances.size() == 2);
        for (const auto &instance : inventory->instances)
            CHECK(instance.world_transform[3].z == 2.0f);
    };
    auto record = left.at("record");
    record["radius"] = 1.125;
    const auto record_result = edit({{"record", record}});
    INFO(record_result.dump());
    REQUIRE(record_result.at("status") == "committed");
    check_live_transform();
    const auto record_edit = sample(1.125f);
    CHECK(record_edit.blas_build_count == initial.blas_build_count);
    CHECK(record_edit.blas_update_count == initial.blas_update_count);
    const auto asset_path = project.path / "assets.json";
    CHECK(Json::parse(readWp374aText(asset_path))["procedural_geometry"][0]["record"] == record);
    auto bounds = left.at("aabb");
    bounds["min"][0] = -3.625;
    const auto bounds_result = edit({{"aabb", bounds}});
    INFO(bounds_result.dump());
    REQUIRE(bounds_result.at("status") == "committed");
    check_live_transform();
    const auto bounds_edit = sample(1.125f);
    CHECK(bounds_edit.blas_build_count == record_edit.blas_build_count + 1);
    CHECK(bounds_edit.blas_update_count == record_edit.blas_update_count);
    auto &procedural = GET_MODULE(ProceduralGeometryContainer);
    const auto stable_instances = procedural.proceduralRayInstances();
    const auto stable_cache = GET_MODULE(ProjectBasicConfig).assetDataJson();
    const auto stable_bytes = readWp374aText(asset_path);
    auto invalid_record = record;
    invalid_record["radius"] = "not a float";
    const auto rejected = edit({{"record", invalid_record}});
    CHECK(rejected.at("status") != "committed");
    CHECK(readWp374aText(asset_path) == stable_bytes);
    CHECK(GET_MODULE(ProjectBasicConfig).assetDataJson() == stable_cache);
    CHECK(procedural.proceduralRayInstances() == stable_instances);
    CHECK(sample(1.125f).blas_build_count == bounds_edit.blas_build_count);

    auto uncommitted_assets = Json::parse(stable_bytes);
    uncommitted_assets["procedural_geometry"][0]["aabb"]["min"][0] = -3.75;
    {
        auto prepared = PreparedProceduralAssetPublication::prepare(
            GET_MODULE(ProjectBasicConfig), GET_MODULE(PathResolver), procedural,
            GET_MODULE(PolygonInstanceContainer), &GET_MODULE(VulkanManageCore), "main", uncommitted_assets.dump());
        CHECK_THROWS_WITH(prepared->commit([] { throw std::runtime_error("source write failed"); }), "source write failed");
    }
    CHECK(procedural.proceduralRayInstances() == stable_instances);
    CHECK(GET_MODULE(ProjectBasicConfig).assetDataJson() == stable_cache);
    CHECK(readWp374aText(asset_path) == stable_bytes);
    CHECK(sample(1.125f).blas_build_count == bounds_edit.blas_build_count);

    const auto stale_digest = catalog().at("source_digest").at("hex");
    const auto external_bytes = stable_bytes + "\n";
    writeWp374aText(asset_path, external_bytes);
    const auto stale = finish(runWp374aRpcBytes(*service, rpc_id++, "edit_procedural_asset",
        {{"base_source_digest", stale_digest}, {"asset_name", "sphere_asset"}, {"record", record}}));
    CHECK(stale.at("status") != "committed");
    CHECK(readWp374aText(asset_path) == external_bytes);
    CHECK(GET_MODULE(ProjectBasicConfig).assetDataJson() == stable_cache);
    CHECK(procedural.proceduralRayInstances() == stable_instances);
    CHECK(sample(1.125f).blas_build_count == bounds_edit.blas_build_count);
    service.reset();
    auto externally_changed = Json::parse(stable_bytes);
    externally_changed["procedural_geometry"][0]["record"]["radius"] = 1.375;
    writeWp374aText(asset_path, externally_changed.dump());
    service = makeEditorRuntimeService({.render_config_editing = true});
    CHECK_FALSE(catalog().at("editable").get<bool>());
}

TEST_CASE("WP386b sphere reports hits and reload publishes final entry classification",
          "[WP386b][ray-tracing][production][gpu]") {
    const bool invalid_record = GENERATE(false, true);
    CAPTURE(invalid_record);
    setupLogger();
    Wp374aProjectRoot project{"wp386b_sphere"};
    writeWp386aProceduralProject(project.path);
    auto assets = Json::parse(readWp374aText(project.path / "assets.json"));
    auto &sphere = assets["procedural_geometry"][0];
    sphere["aabb"] = {{"min", {-4.125, -4.125, 3.875}}, {"max", {4.125, 4.125, 12.125}}};
    sphere["record"] = {{"radius", 4.0}, {"center", {0.0, 0.0, 8.0}}};
    if (invalid_record) sphere["record"]["radius"] = "not a float";
    writeWp374aText(project.path / "assets.json", assets.dump());
    auto layout = Json::parse(readWp374aText(project.path / "ray_layout.json"));
    layout["record"].push_back({{"name", "center"}, {"type", "vec3"}});
    writeWp374aText(project.path / "ray_layout.json", layout.dump());
    auto feature = Json::parse(readWp374aText(project.path / "features/wp386_procedural.json"));
    feature["compute_tasks"][0]["ray_tracing"]["hit_shaders"]["primary"]["sphere"]["intersection"] = "project://shaders/sphere";
    writeWp374aText(project.path / "features/wp386_procedural.json", feature.dump());
    writeWp374aText(project.path / "shaders/wp386.rchit", R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require
#include "__pelican_ray_group_layout.glsl"
struct Payload { uint marker; uint q; uint flags; uint tint; };
layout(location=0) rayPayloadInEXT Payload payload;
hitAttributeEXT vec2 hit_attribute;
layout(shaderRecordEXT, std430) buffer SphereRecord {
    PELICAN_RAY_ENGINE_RECORD_MEMBERS
    float radius;
    vec3 center;
} sphere;
void main() {
    payload.marker = 0x386bu;
    payload.q = floatBitsToUint(gl_HitTEXT);
    payload.flags = gl_HitKindEXT;
    payload.tint = floatBitsToUint(sphere.radius);
}
)glsl");
    FastModuleContainer modules;
    Wp380RuntimeCleanup cleanup;
    (void)configureWp374aRuntime(project.path);
    const auto off_source = GET_MODULE(PathResolver).loadText("engine://procedural_no_report.rint");
    const auto on_source = GET_MODULE(PathResolver).loadText("engine://procedural_sphere.rint");
    writeWp374aText(project.path / "shaders/sphere.rint", off_source);
    TestSupport::requireVulkanDevice("WP386b sphere acceptance");
    GET_MODULE(ECSPredefinedRegistration).reg();
    if (invalid_record) {
        REQUIRE_THROWS_WITH(test_support::loadScene(GET_MODULE(SceneLoader), "main"),
            ContainsSubstring("pelican.ray_binding_record_invalid@1"));
        return;
    }
    test_support::loadScene(GET_MODULE(SceneLoader), "main");
    const auto geometry = GET_MODULE(ProceduralGeometryContainer).proceduralRayInstances();
    const auto status = [&] {
        std::istringstream input{Json{{"jsonrpc", "2.0"}, {"id", 3862},
            {"method", "get_status"}, {"params", Json::object()}}.dump() + "\n"};
        std::ostringstream output;
        runEngineRpcServer(input, output);
        const auto response = Json::parse(output.str());
        REQUIRE_FALSE(response.contains("error"));
        return response.at("result").at("ray_tracing").at("procedural_oracle");
    };
    const auto pixels = [&] {
        (void)renderWp374aProductionRows();
        const auto output = GET_MODULE(Renderer).readRenderTargetForTesting("wp386_out", ImageSubresourceRange{});
        std::array<std::uint32_t, 12> values{};
        REQUIRE(output.bytes.size() >= sizeof(values));
        std::memcpy(values.data(), output.bytes.data(), sizeof(values));
        return values;
    };
    CHECK(pixels()[0] == 0x386au);
    const auto off_status = status();
    REQUIRE(off_status.size() == 1);
    CHECK(off_status[0]["classification"] == "sequence_mismatch");
    const auto off_digest = off_status[0]["entry_digest"].get<std::string>();
    const auto reload = [&](const std::string &source) {
        writeWp374aText(project.path / "shaders/sphere.rint", source);
        auto prepared = GET_MODULE(ShaderLibrary).prepareReloadAll();
        std::erase_if(prepared.candidates, [](const auto &candidate) {
            return candidate.replacement.expected_stage != ShaderStage::intersection;
        });
        std::string digest;
        for (const auto &candidate : prepared.candidates) {
            if (candidate.replacement.expected_stage != ShaderStage::intersection) continue;
            digest = selectRayTracingEntryPoint(candidate.replacement.reflection.ray_tracing_module,
                ShaderStage::intersection, "main", "sphere reload").entry_digest;
        }
        REQUIRE_FALSE(digest.empty());
        const auto rebuilt = GET_MODULE(PipelineFactory).rebuildPrepared(std::move(prepared));
        REQUIRE(rebuilt.committed);
        REQUIRE(rebuilt.failed_pipelines == 0);
        return digest;
    };
    const auto on_digest = reload(on_source);
    REQUIRE(on_digest != off_digest);
    const auto on_status = status();
    const auto require_eligible = [&](const Json &wire) {
        if (wire.size() != 1 || wire[0].value("entry_digest", "") != on_digest ||
            wire[0].value("classification", "") != "eligible")
            throw std::runtime_error("procedural_oracle_shader_ineligible@1");
    };
    REQUIRE_NOTHROW(require_eligible(on_status));
    REQUIRE_THROWS_WITH(require_eligible(Json::array()), "procedural_oracle_shader_ineligible@1");
    auto stale = on_status;
    stale[0]["entry_digest"] = off_digest;
    REQUIRE_THROWS_WITH(require_eligible(stale), "procedural_oracle_shader_ineligible@1");
    const auto hit = pixels();
    CHECK(hit[0] == 0x386bu);
    CHECK(hit[4] == 0x386bu);
    CHECK(hit[8] == 0x386au);
    for (std::size_t index = 0; index < 2; ++index) {
        const auto oracle = traceProceduralSphereOracle(
            {{index == 0 ? -2.0 : 2.0, 0.0, 0.0}, {0.0, 0.0, 1.0}},
            {0.0, 0.0, 8.0}, 4.0, 0x386bu);
        REQUIRE(oracle.hit.has_value());
        CHECK(rayTracingOracleContainsGpuHit(*oracle.hit,
              std::bit_cast<float>(hit[index * 4 + 1]), hit[index * 4]));
    }
    CHECK(hit[2] == 0u);
    CHECK(std::bit_cast<float>(hit[3]) == 4.0f);
    auto relaxed_source = on_source;
    for (auto at = relaxed_source.find("precise "); at != std::string::npos;
         at = relaxed_source.find("precise "))
        relaxed_source.erase(at, std::string_view{"precise "}.size());
    CHECK(reload(relaxed_source) != on_digest);
    CHECK(status()[0]["classification"] == "sequence_mismatch");
    CHECK(pixels()[0] == 0x386bu); // Ineligible for the oracle still loads and renders.
    CHECK(reload(off_source) == off_digest);
    CHECK(pixels()[0] == 0x386au);
    CHECK(status()[0]["classification"] == "sequence_mismatch");
    CHECK(GET_MODULE(ProceduralGeometryContainer).proceduralRayInstances() == geometry);
}
#endif
} // namespace Pelican
