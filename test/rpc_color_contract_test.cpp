#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/ecs/predefined/modelview.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/log.hpp"
#include "../src/core/material/materialcontainer.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/vertbufcontainer.hpp"
#include "../src/core/renderer/polygoninstancecontainer.hpp"
#include "../src/core/userpublic/gameobjects.hpp"
#include "../src/core/vkcore/core.hpp"
#include "gltf_fragment_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stb_image.h>
#include <tuple>
#include <vector>

namespace Pelican {
namespace {

void writeFile(const std::filesystem::path &path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary};
    file << contents;
}

struct TempProject {
    std::filesystem::path root;
    ~TempProject() { std::filesystem::remove_all(root); }
};

nlohmann::json runRpcRequest(std::uint64_t id, std::string_view method,
                             nlohmann::json params) {
    std::istringstream input{
        nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method},
                       {"params", std::move(params)}}.dump() + "\n"};
    std::ostringstream output;
    runEngineRpcServer(input, output);
    return nlohmann::json::parse(output.str());
}

struct RenderCommandIdentity {
    std::uint32_t index_count = 0;
    std::uint32_t instance_count = 0;
    std::uint32_t first_index = 0;
    std::int32_t vertex_offset = 0;
    std::uint32_t first_instance = 0;
    GlobalMaterialId material{};
    std::uint32_t source_material_index = 0;
    std::uint32_t node_index = 0;
    bool skinned = false;
    PrimitiveViewVisibility view_visibility = PrimitiveViewVisibility::both;

    bool operator==(const RenderCommandIdentity &) const = default;
};

struct TransientInventory {
    std::size_t entities = 0;
    std::size_t instances = 0;
    std::size_t textures = 0;
    std::size_t materials = 0;
    std::size_t indices = 0;
    std::size_t vertices = 0;
    std::size_t skinned_vertices = 0;
    std::size_t morph_deltas = 0;
    std::optional<GameObjectId> anchor_object;
    std::optional<ModelInstanceId> anchor_instance;
    std::vector<RenderCommandIdentity> commands;

    bool operator==(const TransientInventory &other) const {
        const auto same_instance =
            anchor_instance.has_value() == other.anchor_instance.has_value() &&
            (!anchor_instance || anchor_instance->value == other.anchor_instance->value);
        return entities == other.entities && instances == other.instances &&
               textures == other.textures && materials == other.materials &&
               indices == other.indices && vertices == other.vertices &&
               skinned_vertices == other.skinned_vertices &&
               morph_deltas == other.morph_deltas &&
               anchor_object == other.anchor_object && same_instance &&
               commands == other.commands;
    }
};

TransientInventory transientInventory(std::string_view anchor_name) {
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    auto &materials = GET_MODULE(MaterialContainer);
    auto &geometry = GET_MODULE(VertBufContainer);
    auto &scene = GET_MODULE(SceneLoader);

    TransientInventory result{
        .entities = GameObjects::liveCountForTesting(),
        .instances = instances.instanceCountForTesting(),
        .textures = materials.textureCountForTesting(),
        .materials = materials.materialCountForTesting(),
        .indices = geometry.allocatedIndexCountForTesting(),
        .vertices = geometry.allocatedVertexCountForTesting(),
        .skinned_vertices = geometry.allocatedVertexCountForTesting(true),
        .morph_deltas = geometry.allocatedMorphDeltaCountForTesting(),
        .anchor_object = scene.objectId(anchor_name),
    };
    if (result.anchor_object) {
        const auto *component = GET_MODULE(ECSCore)
                                    .getTemplatePublicModule()
                                    .tryComponent<SimpleModelViewComponent>(*result.anchor_object);
        if (component != nullptr) result.anchor_instance = component->model_instance_id;
    }
    for (const auto &entry : instances.renderCommandsForTesting()) {
        result.commands.push_back(RenderCommandIdentity{
            .index_count = entry.command.indexCount,
            .instance_count = entry.command.instanceCount,
            .first_index = entry.command.firstIndex,
            .vertex_offset = entry.command.vertexOffset,
            .first_instance = entry.command.firstInstance,
            .material = entry.material,
            .source_material_index = entry.source_material_index,
            .node_index = entry.node_index,
            .skinned = entry.skinned,
            .view_visibility = entry.view_visibility,
        });
    }
    return result;
}

} // namespace

TEST_CASE("RPC capture reports contract 2 and absolute encoded-sRGB color", "[rpc][color][headless]") {
    const bool fallback = GENERATE(false, true);
    CAPTURE(fallback);
    setupLogger(true);
    FastModuleContainer modules;
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("pelican_rpc_color_" + std::to_string(fallback) + "_" + std::to_string(suffix));
    const auto capture = root / "capture.png";

    writeFile(root / "scene.json", R"json({
  "schema":"pelican.scene","version":1,
  "scenes":{"default_scene":{"objects":[]}}
})json");
    writeFile(root / "assets.json", R"json({"models":[]})json");
    writeFile(root / "ui/ui.json", R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
    writeFile(root / "shaders/fullscreen.vert", R"glsl(
#version 450
layout(location=0) out vec2 uv;
void main(){ uv=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2); gl_Position=vec4(uv*2.0-1.0,0,1); }
)glsl");
    writeFile(root / "shaders/half.frag", R"glsl(
#version 450
layout(location=0) out vec4 outColor;
void main(){ outColor=vec4(0.5,0.5,0.5,0.25); }
)glsl");
    writeFile(root / "passes/main.json", R"json({
  "render_targets":[],
  "rendering_passes":[{"name":"main","passes":[{
    "name":"known_value","type":"fullscreen",
    "output":{"color":"swapchain","depth":null},
    "shader":{"vertex":"shaders/fullscreen","fragment":"shaders/half"}
  }]}]
})json");

    const nlohmann::json project{
        {"schema", "pelican.project"}, {"version", 1}, {"name", "RPC color"},
        {"engine_min_version", "0.1.0"},
        {"basic_config", {{"window_size", {{"width", 16}, {"height", 16}}},
                          {"framerate", 60}, {"default_scene_id", "default_scene"},
                          {"scene_data_json", "scene.json"}, {"asset_data_json", "assets.json"},
                          {"rendering_config_json", "passes/main.json"},
                          {"ui_config_json", "ui/ui.json"}, {"default_rendering_pass", "main"}}},
    };
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setProjectData(project.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    launch.shader_hot_reload = false;
    launch.force_unorm_color_path_for_testing = fallback;
    GET_MODULE(EngineTime).setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);

    const auto capture_string = capture.generic_string();
    std::istringstream input{
        nlohmann::json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "render_frame"},
                       {"params", nlohmann::json::object()}}.dump() + "\n" +
        nlohmann::json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "capture"},
                       {"params", {{"path", capture_string}}}}.dump() + "\n" +
        nlohmann::json{{"jsonrpc", "2.0"}, {"id", 3}, {"method", "get_status"},
                       {"params", nlohmann::json::object()}}.dump() + "\n"};
    std::ostringstream output;
    runEngineRpcServer(input, output);
    GET_MODULE(VulkanManageCore).waitIdle();

    std::string first_line;
    std::string second_line;
    std::string third_line;
    std::istringstream responses{output.str()};
    std::getline(responses, first_line);
    std::getline(responses, second_line);
    std::getline(responses, third_line);
    const auto response = nlohmann::json::parse(second_line).at("result");
    REQUIRE(response.at("contract") == 2);
    REQUIRE(response.at("encoding") == "srgb");
    const auto color_status = nlohmann::json::parse(third_line).at("result").at("color");
    REQUIRE(color_status.at("contract") == 2);
    REQUIRE(color_status.at("path") == (fallback ? "unorm_fallback" : "srgb"));
    const auto module_status = nlohmann::json::parse(third_line).at("result").at("modules");
    REQUIRE(module_status.at("phase") == "booting");
    REQUIRE_FALSE(module_status.at("creation_frozen").get<bool>());
    const auto xr_status = nlohmann::json::parse(third_line).at("result").at("xr");
    REQUIRE_FALSE(xr_status.at("active").get<bool>());
    REQUIRE(xr_status.at("reference_space").is_null());
    REQUIRE(xr_status.at("floor_semantics") == "not_applicable");
    REQUIRE(xr_status.at("applied_floor_offset_m").is_null());

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc *pixels = stbi_load(capture.string().c_str(), &width, &height, &channels, 4);
    REQUIRE(pixels != nullptr);
    REQUIRE(width == 16);
    REQUIRE(height == 16);
    const auto center = static_cast<size_t>((height / 2) * width + width / 2) * 4;
    const int tolerance = fallback ? 1 : 0;
    REQUIRE(std::abs(static_cast<int>(pixels[center + 0]) - 188) <= tolerance);
    REQUIRE(std::abs(static_cast<int>(pixels[center + 1]) - 188) <= tolerance);
    REQUIRE(std::abs(static_cast<int>(pixels[center + 2]) - 188) <= tolerance);
    REQUIRE(pixels[center + 3] == 64);
    stbi_image_free(pixels);
    std::filesystem::remove_all(root);
}

TEST_CASE("RPC load_gltf publishes once and preserves inventory on preflight and GPU failure",
          "[wp144][rpc][gltf][transaction][gpu]") {
    setupLogger(true);
    FastModuleContainer modules;
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const TempProject project_dir{
        std::filesystem::temp_directory_path() /
        ("pelican_wp144_rpc_" + std::to_string(suffix))};

    std::filesystem::create_directories(project_dir.root);
    TestGltfFragmentFixture::writeGlb(project_dir.root / "asset.glb");
    writeFile(project_dir.root / "scene.json", R"json({
  "schema":"pelican.scene","version":1,
  "scenes":{"default_scene":{"objects":[]}}
})json");
    writeFile(project_dir.root / "assets.json", R"json({"models":[]})json");
    writeFile(project_dir.root / "ui/ui.json",
              R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
    writeFile(project_dir.root / "shaders/fullscreen.vert", R"glsl(
#version 450
layout(location=0) out vec2 uv;
void main(){ uv=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2); gl_Position=vec4(uv*2.0-1.0,0,1); }
)glsl");
    writeFile(project_dir.root / "shaders/half.frag", R"glsl(
#version 450
layout(location=0) out vec4 outColor;
void main(){ outColor=vec4(0.5,0.5,0.5,1.0); }
)glsl");
    writeFile(project_dir.root / "passes/main.json", R"json({
  "render_targets":[],
  "rendering_passes":[{"name":"main","passes":[{
    "name":"known_value","type":"fullscreen",
    "output":{"color":"swapchain","depth":null},
    "shader":{"vertex":"shaders/fullscreen","fragment":"shaders/half"}
  }]}]
})json");

    const nlohmann::json project{
        {"schema", "pelican.project"}, {"version", 1}, {"name", "WP144 RPC"},
        {"engine_min_version", "0.1.0"},
        {"basic_config", {{"window_size", {{"width", 16}, {"height", 16}}},
                          {"framerate", 60}, {"default_scene_id", "default_scene"},
                          {"scene_data_json", "scene.json"}, {"asset_data_json", "assets.json"},
                          {"rendering_config_json", "passes/main.json"},
                          {"ui_config_json", "ui/ui.json"}, {"default_rendering_pass", "main"}}},
    };
    GET_MODULE(PathResolver).setup(project_dir.root, false);
    GET_MODULE(ProjectSource).setProjectData(project.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    launch.shader_hot_reload = false;
    GET_MODULE(EngineTime).setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    GET_MODULE(ECSPredefinedRegistration).reg();

    try {
        (void)GET_MODULE(StandardMaterialResource);
        std::istringstream empty_input;
        std::ostringstream empty_output;
        runEngineRpcServer(empty_input, empty_output);
    } catch (const std::exception &error) {
        SKIP(std::string{"Vulkan headless rendering unavailable: "} + error.what());
    }

    const auto success = runRpcRequest(
        1, "load_gltf", {{"path", "asset.glb"}, {"name", "anchor"}});
    REQUIRE(success.at("result").at("name") == "anchor");
    REQUIRE(success.at("result").at("path").get<std::string>().ends_with("asset.glb"));
    const auto published = transientInventory("anchor");
    REQUIRE(published.anchor_object);
    REQUIRE(published.anchor_instance);
    REQUIRE(published.entities == 1);
    REQUIRE(published.instances == 1);
    REQUIRE(published.commands.size() == 2);

    const auto duplicate = runRpcRequest(
        2, "load_gltf", {{"path", "missing.glb"}, {"name", "anchor"}});
    REQUIRE(duplicate.at("error").at("code") == JsonRpcErrorCodes::applicationError);
    REQUIRE(duplicate.at("error").at("message").get<std::string>().find("anchor") !=
            std::string::npos);
    REQUIRE(transientInventory("anchor") == published);

    const auto invalid_fragment = runRpcRequest(
        3, "load_gltf", {{"path", "asset.glb#camera/Main"}, {"name", "invalid_fragment"}});
    REQUIRE(invalid_fragment.at("error").at("code") == JsonRpcErrorCodes::applicationError);
    REQUIRE(invalid_fragment.at("error").at("message").get<std::string>().find("camera") !=
            std::string::npos);
    REQUIRE(transientInventory("anchor") == published);

    auto &materials = GET_MODULE(MaterialContainer);
    const auto &standard = GET_MODULE(StandardMaterialResource);
    const MaterialInfo filler{
        .vert_shader = standard.standardVertShader(),
        .frag_shader = standard.standardFragShader(),
        .base_color_texture = standard.whiteTexture(),
        .metallic_roughness_texture = standard.metallicRoughnessDefaultTexture(),
        .normal_texture = standard.normalDefaultTexture(),
        .emissive_texture = standard.emissiveDefaultTexture(),
    };
    while (materials.materialCountForTesting() + 1 < materials.materialCapacityForTesting()) {
        (void)materials.registerMaterial(filler);
    }
    const auto before_gpu_failure = transientInventory("anchor");
    const auto gpu_failure = runRpcRequest(
        4, "load_gltf", {{"path", "asset.glb"}, {"name", "gpu_failure"}});
    REQUIRE(gpu_failure.at("error").at("code") == JsonRpcErrorCodes::applicationError);
    REQUIRE(gpu_failure.at("error").at("message").get<std::string>().find("Material capacity exceeded") !=
            std::string::npos);
    REQUIRE(transientInventory("anchor") == before_gpu_failure);

    GET_MODULE(VulkanManageCore).waitIdle();
}

} // namespace Pelican
