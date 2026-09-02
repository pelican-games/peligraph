#include <catch2/catch_test_macros.hpp>
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
                        bool renderable) {
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
        components.push_back({{"name", "ray_instance"}, {"mask", 9}});
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
        writeWp374aText(root / "ray_layout.json",
                        wp374aLayout(*layout_version, with_as).dump(2));
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

void loadWp374aScene(const std::filesystem::path &root) {
    (void)configureWp374aRuntime(root);
    // Acquire the device only after the fixture project root is configured;
    // acquiring it first makes the renderer read the default project layout.
    TestSupport::requireVulkanDevice("WP374a production acceptance");
    GET_MODULE(ECSPredefinedRegistration).reg();
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

Json commitWp374aEdit(EditorCommandService &service, std::uint64_t &rpc_id,
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

} // namespace


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


} // namespace Pelican
