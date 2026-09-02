#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <components/predefined.hpp>

#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/appflow/framephase.hpp"
#include "../src/core/communication/editorcommandservice.hpp"
#include "../src/core/communication/editorrpchandlers.hpp"
#include "../src/core/communication/editorruntimefactory.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/componentinfo.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/gamelogic/behaviorarena.hpp"
#include "../src/core/gamelogic/gamelogicreload.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/componentcodec.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/resolvedscene.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/log.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/renderingpass/renderingpassconfigloader.hpp"
#include "../src/core/renderer/polygoninstancecontainer.hpp"
#include "../src/core/shader/raygrouplayout.hpp"
#include "../src/core/vkcore/accelerationstructure.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/core/userpublic/details/component/registerer.hpp"
#include "../src/core/userpublic/details/ecs/componentdeclare.hpp"
#include "gltf_fragment_fixture.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include "raytracing_rt3_1_fixture.hpp"
#include "vulkan_test_support.hpp"

namespace Pelican {
namespace {

using Catch::Matchers::ContainsSubstring;
using Json = nlohmann::json;
using RayTracingRt31Fixture::accelerationDeclaration;
using RayTracingRt31Fixture::layoutRoot;

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
            Json{{"name", "ray_instance"}, {"mask", 9}}));
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

    Json rendering{
        {"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
    };
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

std::shared_ptr<const RayTracingProjectConfig>
configureWp374aRuntime(const std::filesystem::path &root) {
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setSourceByData(
        Json{{"basic_config",
              {{"window_size", {{"width", 32}, {"height", 32}}},
               {"scene_data_json", "scene.json"},
               {"asset_data_json", "assets.json"},
               {"default_scene_id", "main"},
               {"rendering_config_json", "hybrid.json"},
               {"default_rendering_pass", "main_render"}}}}
            .dump());
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
    std::optional<std::filesystem::path> game_logic_dll = std::nullopt) {
    (void)configureWp374aRuntime(root);
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
    GET_MODULE(SceneLoader).load("main");
}

Json runWp374aRpcBytes(EditorCommandService &service, std::uint64_t id,
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
    if (response.value("id", std::uint64_t{}) != id ||
        !response.contains("result")) {
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

Json wp375Components(EditorCommandService &service, std::uint64_t &rpc_id,
                     std::string_view name) {
    return runWp374aRpcBytes(
        service, rpc_id++, "get_components",
        {{"scene_id", "main"}, {"name", std::string{name}}});
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

void wp375FinishRuntime(std::unique_ptr<EditorCommandService> &service,
                        bool behavior = false) {
    service.reset();
    if (behavior) {
        GET_MODULE(BehaviorAttachmentArena).deactivateAll();
        shutdownConfiguredGameLogic();
    }
    GET_MODULE(VulkanManageCore).waitIdle();
}

} // namespace

TEST_CASE(
    "WP377 authored ray_instance force opaque reaches the material alpha profile row guard",
    "[WP377][WP369][ray-tracing][rt3-1][instance-flags][profile][production]") {
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
                Json{{"name", "ray_instance"},
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
          "[WP374a][WP374][ray-tracing][rt3-1][state-table][scene-loader][production][mutation-2]") {
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

TEST_CASE(
    "WP374a real RPC bytes drive add set save fresh reload remove into committed TLAS rows",
    "[WP374a][WP374][ray-tracing][rt3-1][rpc][production][tlas][mutation-1]") {
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
                           {{"name", "ray_instance"},
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
                  {"component_slot", "ray_instance"},
                  {"field_path", "/mask"},
                  {"value", 34}},
                 {{"op", "set_component_value"},
                  {"object_id", saved_object_id},
                  {"component_slot", "ray_instance"},
                  {"field_path", "/instance_flags"},
                  {"value", {"force_no_opaque"}}},
                 {{"op", "set_component_value"},
                  {"object_id", saved_object_id},
                  {"component_slot", "ray_instance"},
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
                          {"component_slot", "ray_instance"}}}));
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

TEST_CASE(
    "WP375 production SceneLoader preflight rejects a non-renderable ray instance beside a valid scene",
    "[WP375][ray-tracing][scene-loader][production][preflight][mutation-preflight]") {
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

TEST_CASE(
    "WP375 production acceptance a materializes transform runtime before save and fresh reload",
    "[WP375][rpc][production][mutation-a][gpu]") {
    setupLogger();
    Wp374aProjectRoot project{"wp375_mutation_a"};
    writeWp374aProject(project.path, std::nullopt, false, false, true);
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
    Wp374aProjectRoot project{"wp375_mutation_b"};
    writeWp374aProject(project.path, std::nullopt, false, false, true);
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
        REQUIRE(unknown_error.at("data").at("type") ==
                "wp375_no_such_behavior");
        REQUIRE(unknown_error.at("data").at("object_id") == object_id);
        REQUIRE(unknown_error.at("payload") == unknown_error.at("data"));
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


} // namespace Pelican
