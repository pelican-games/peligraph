#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/asset/model.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/log.hpp"
#include "../src/core/material/projectmaterialasset.hpp"
#include "../src/core/material/materialcontainer.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/engineprimitives.hpp"
#include "../src/core/fullscreenpass/fullscreenpasscontainer.hpp"
#include "../src/core/renderingpass/renderingpasscontainer.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/project/materialpassdecompose.hpp"
#include "../src/project/projecttemplate.hpp"
#include "../src/project/renderconfigsection.hpp"
#include "lp3a_decompose_support.hpp"
#include "render_graph_root_test_support.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stb_image.h>
#include <string>
#include <vector>

// GP-5. engine://primitives/{cube,plane,sphere} resolve as model references
// with no asset_data entry. The shapes are generated in code, deterministically,
// and reach the renderer through the ordinary glTF load path.

namespace Pelican {
namespace {

using Json = nlohmann::json;

std::uint64_t fnv1a(std::uint64_t hash, const void *data, std::size_t bytes) {
    const auto *begin = static_cast<const unsigned char *>(data);
    for (std::size_t index = 0; index < bytes; ++index) {
        hash ^= begin[index];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

// Every stream, in upload order, byte for byte.
std::uint64_t meshDigest(const EnginePrimitiveMesh &mesh) {
    auto hash = 0xcbf29ce484222325ULL;
    const auto add = [&](const auto &values) {
        const std::uint64_t count = values.size();
        hash = fnv1a(hash, &count, sizeof(count));
        hash = fnv1a(hash, values.data(), values.size() * sizeof(values[0]));
    };
    add(mesh.positions);
    add(mesh.normals);
    add(mesh.texcoords);
    add(mesh.tangents);
    add(mesh.indices);
    return hash;
}

struct V3 {
    double x, y, z;
};
V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 at(const std::vector<float> &values, std::size_t vertex) {
    return {values[vertex * 3], values[vertex * 3 + 1], values[vertex * 3 + 2]};
}

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
                   ("pelican_gp5_" + std::string{name} + "_" + std::to_string(suffix))};
}

// A project made by the blank template: asset_data has no models. The blank
// frame is one fullscreen gradient pass that draws no geometry, so the
// rendering section is taken from the standard template (the only other file
// the two templates differ in) to have something that draws meshes.
std::filesystem::path makeBlankProject(const std::filesystem::path &dir) {
    const auto root = dir / "project";
    initializeProjectTemplate(root, ProjectTemplate::Blank);
    const auto standard = dir / "standard";
    initializeProjectTemplate(standard, ProjectTemplate::Standard);
    std::filesystem::copy_file(standard / "passes/main_rendering_config.json",
                               root / "passes/main_rendering_config.json",
                               std::filesystem::copy_options::overwrite_existing);
    const auto assets = Json::parse(readFile(root / "assets/asset_data.json"));
    REQUIRE(assets.at("models").empty());
    return root;
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
    TestSupport::requireVulkanDevice("GP-5 engine primitives");
    GET_MODULE(ECSPredefinedRegistration).reg();
}

struct Rpc {
    std::istringstream input;
    std::ostringstream output;
    EngineRpcEndpoint endpoint{input, output};
    std::uint64_t next_id = 5000;

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
        std::vector<unsigned char> rgba(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
        stbi_image_free(pixels);
        return rgba;
    }
    // Submits one edit and steps the frame that commits it.
    Json edit(std::uint64_t actor, Json operations) {
        const auto revision = result("get_scene_revision").at("scene_revision");
        const auto accepted = result("edit", {{"actor_id", actor},
                                              {"base_revision", revision},
                                              {"operations", std::move(operations)}});
        REQUIRE(accepted.at("status") == "accepted");
        (void)result("step_frame");
        return result("get_edit_result", {{"ticket", accepted.at("ticket")}});
    }
};

std::size_t differingPixels(const std::vector<unsigned char> &a, const std::vector<unsigned char> &b) {
    REQUIRE(a.size() == b.size());
    std::size_t count = 0;
    for (std::size_t pixel = 0; pixel + 3 < a.size(); pixel += 4) {
        int delta = 0;
        for (std::size_t channel = 0; channel < 3; ++channel)
            delta = std::max(delta, std::abs(int{a[pixel + channel]} - int{b[pixel + channel]}));
        if (delta > 8) ++count;
    }
    return count;
}

Json spawnCube(std::string_view model, std::string_view name = "PrimitiveCube") {
    return Json::array({{{"op", "spawn"},
                         {"scene_id", "default_scene"},
                         {"object",
                          {{"name", name},
                           {"components",
                            Json::array({{{"name", "transform"},
                                          {"pos", {0.0, 0.0, 0.0}},
                                          {"rotation", {0.0, 0.0, 0.0, 1.0}},
                                          {"scale", {1.0, 1.0, 1.0}}},
                                         {{"name", "simplemodelview"}, {"model", model}}})}}}}});
}

std::size_t indexCount(const ModelTemplate &model) {
    std::size_t count = 0;
    for (const auto &group : model.material_primitives)
        for (const auto &primitive : group.primitives) count += primitive.index_count;
    return count;
}

} // namespace

TEST_CASE("GP-5 engine primitives are generated deterministically, closed and outward-facing",
          "[gp5][primitives]") {
    struct Expected {
        std::string_view reference;
        std::size_t vertices;
        std::size_t indices;
        std::uint64_t digest;
    };
    constexpr auto segments = enginePrimitiveSphereSegments;
    constexpr auto rings = enginePrimitiveSphereRings;
    const std::array<Expected, 3> expected{{
        {"engine://primitives/cube", 24, 36, 0x11de26020ec19ef1ULL},
        {"engine://primitives/plane", 4, 6, 0xb4b5892b1b278759ULL},
        {"engine://primitives/sphere", std::size_t{rings + 1} * (segments + 1),
         std::size_t{segments} * (rings - 1) * 2 * 3, 0x6d51ad58aaee0ae7ULL},
    }};
    for (const auto &shape : expected) {
        INFO(std::string{shape.reference});
        const auto first = generateEnginePrimitive(shape.reference);
        const auto second = generateEnginePrimitive(shape.reference);
        const auto digest = meshDigest(first);
        UNSCOPED_INFO("digest " << shape.reference << " = 0x" << std::hex << digest << std::dec);
        CHECK(digest == meshDigest(second));
        CHECK(digest == shape.digest);

        const auto vertices = first.vertexCount();
        CHECK(vertices == shape.vertices);
        CHECK(first.normals.size() == vertices * 3);
        CHECK(first.texcoords.size() == vertices * 2);
        CHECK(first.tangents.size() == vertices * 4);
        CHECK(first.indices.size() == shape.indices);
        CHECK(std::all_of(first.indices.begin(), first.indices.end(),
                          [&](std::uint32_t index) { return index < vertices; }));
        for (std::size_t vertex = 0; vertex < vertices; ++vertex) {
            const auto normal = at(first.normals, vertex);
            const V3 tangent{first.tangents[vertex * 4], first.tangents[vertex * 4 + 1],
                             first.tangents[vertex * 4 + 2]};
            CHECK(std::abs(dot(normal, normal) - 1.0) < 1e-5);
            CHECK(std::abs(dot(tangent, tangent) - 1.0) < 1e-5);
            CHECK(std::abs(dot(normal, tangent)) < 1e-5);
            CHECK(first.tangents[vertex * 4 + 3] == 1.0F);
            CHECK(first.texcoords[vertex * 2] >= 0.0F);
            CHECK(first.texcoords[vertex * 2] <= 1.0F);
            CHECK(first.texcoords[vertex * 2 + 1] >= 0.0F);
            CHECK(first.texcoords[vertex * 2 + 1] <= 1.0F);
        }
        // Counter-clockwise seen from where the normals point (glTF front faces).
        for (std::size_t triangle = 0; triangle < first.indices.size(); triangle += 3) {
            const auto a = at(first.positions, first.indices[triangle]);
            const auto b = at(first.positions, first.indices[triangle + 1]);
            const auto c = at(first.positions, first.indices[triangle + 2]);
            const auto face = cross(sub(b, a), sub(c, a));
            CHECK(dot(face, at(first.normals, first.indices[triangle])) > 0.0);
        }
    }

    const auto cube = generateEnginePrimitive("engine://primitives/cube");
    CHECK(*std::min_element(cube.positions.begin(), cube.positions.end()) == -0.5F);
    CHECK(*std::max_element(cube.positions.begin(), cube.positions.end()) == 0.5F);
    const auto plane = generateEnginePrimitive("engine://primitives/plane");
    for (std::size_t vertex = 0; vertex < plane.vertexCount(); ++vertex) {
        CHECK(plane.positions[vertex * 3 + 1] == 0.0F);
        CHECK(std::abs(plane.positions[vertex * 3]) == 0.5F);
        CHECK(std::abs(plane.positions[vertex * 3 + 2]) == 0.5F);
        CHECK(plane.normals[vertex * 3 + 1] == 1.0F);
    }
    const auto sphere = generateEnginePrimitive("engine://primitives/sphere");
    for (std::size_t vertex = 0; vertex < sphere.vertexCount(); ++vertex) {
        const auto position = at(sphere.positions, vertex);
        CHECK(std::abs(std::sqrt(dot(position, position)) - 0.5) < 1e-6);
    }

    for (const auto *unknown : {"engine://primitives/torus", "engine://primitives/", "engine://primitives/Cube",
                                "engine://primitives/cube#mesh/cube"}) {
        INFO(unknown);
        CHECK(isEnginePrimitiveReference(unknown));
        try {
            (void)generateEnginePrimitive(unknown);
            FAIL("an unknown primitive was generated");
        } catch (const std::runtime_error &error) {
            CHECK(std::string{error.what()}.starts_with("model.primitive_unknown@1: "));
        }
    }
    CHECK_FALSE(isEnginePrimitiveReference("assets/models/cube.glb"));
    CHECK_FALSE(isEnginePrimitiveReference("engine://surfaces/openpbr/opaque_single.surface"));
}

TEST_CASE("GP-5 a cube spawned over RPC into a blank project is drawn, saved and reopened",
          "[gp5][primitives][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("spawn");
    const auto root = makeBlankProject(dir.root);
    const auto capture = dir.root / "capture.png";
    std::vector<unsigned char> background;

    {
        FastModuleContainer modules;
        configureHeadless(root);
        Rpc rpc;
        (void)rpc.result("load_scene", {{"name", "default_scene"}});
        (void)rpc.result("step_frame");
        background = rpc.frame(capture);

        const auto actor = rpc.result("open_editor_session", {{"display_name", "GP-5"}}).at("actor_id")
                               .get<std::uint64_t>();
        const auto spawned = rpc.edit(actor, spawnCube("engine://primitives/cube"));
        INFO(spawned.dump(2));
        REQUIRE(spawned.at("status") == "committed");
        const auto drawn = rpc.frame(capture);
        CHECK(differingPixels(background, drawn) > 64);

        // Every shape resolves with nothing declared, and they are distinct models.
        auto &models = GET_MODULE(ModelAssetContainer);
        const auto &cube = models.getModelTemplateByName("engine://primitives/cube");
        const auto &plane = models.getModelTemplateByName("engine://primitives/plane");
        const auto &sphere = models.getModelTemplateByName("engine://primitives/sphere");
        CHECK(indexCount(cube) == 36);
        CHECK(indexCount(plane) == 6);
        CHECK(indexCount(sphere) ==
              std::size_t{enginePrimitiveSphereSegments} * (enginePrimitiveSphereRings - 1) * 6);
        CHECK(cube.asset_id != plane.asset_id);
        CHECK(cube.asset_id != sphere.asset_id);
        CHECK(cube.authored_asset_reference == "engine://primitives/cube");
        // The default material: one grey OpenPBR base material, named so a
        // material binding can address it.
        REQUIRE(cube.material_primitives.size() == 1);
        REQUIRE(cube.named_materials.size() == 1);
        CHECK(cube.named_materials.front().name == std::string{enginePrimitiveDefaultMaterialName});
        CHECK(cube.named_materials.front().routing.has_value());
        CHECK(cube.material_primitives.front().material == cube.named_materials.front().material);
        // Asking again returns the same template.
        CHECK(&models.getModelTemplateByName("engine://primitives/cube") == &cube);

        // An unknown name is refused by name, directly and through spawn.
        try {
            (void)models.getModelTemplateByName("engine://primitives/torus");
            FAIL("an unknown primitive resolved");
        } catch (const std::runtime_error &error) {
            CHECK(std::string{error.what()}.starts_with("model.primitive_unknown@1: "));
        }
        const auto refused = rpc.edit(actor, spawnCube("engine://primitives/torus", "PrimitiveTorus"));
        INFO(refused.dump(2));
        CHECK(refused.at("status") == "failed");
        CHECK(refused.dump().find("model.primitive_unknown@1") != std::string::npos);

        const auto saved = rpc.result("save_scene");
        CHECK(saved.at("byte_count").get<std::uint64_t>() != 0);
        GET_MODULE(VulkanManageCore).waitIdle();
    }

    const auto scene_text = readFile(root / "scenes/main.scene.json");
    CHECK(scene_text.find("\"engine://primitives/cube\"") != std::string::npos);
    CHECK(scene_text.find("\"PrimitiveCube\"") != std::string::npos);
    CHECK(scene_text.find("PrimitiveTorus") == std::string::npos);

    {
        FastModuleContainer modules;
        configureHeadless(root);
        Rpc rpc;
        (void)rpc.result("load_scene", {{"name", "default_scene"}});
        const auto tree = rpc.result("scene_tree").dump();
        CHECK(tree.find("PrimitiveCube") != std::string::npos);
        // One game frame, as the player would run, before drawing.
        (void)rpc.result("step_frame");
        const auto reopened = rpc.frame(capture);
        CHECK(differingPixels(background, reopened) > 64);
        GET_MODULE(VulkanManageCore).waitIdle();
    }
}

// OL-1 acceptance 1-3 through the running engine: duplicate over RPC places a
// copy of Crate between Crate and Hero, the copy is drawn and goes away and
// comes back with undo and redo, save_scene writes the file, and a fresh engine
// reopens it with the graph owner that named Hero still on Hero. The scene
// starts without ids, so Hero's id is its position until the first save.
TEST_CASE("OL-1 a copy duplicated before a graph owner over RPC survives save and reopen without moving the owner",
          "[ol1][duplicate][rpc][graph-owner][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("ol1_duplicate");
    const auto root = makeBlankProject(dir.root);
    auto scene = Json::parse(readFile(root / "scenes/main.scene.json"));
    auto &objects = scene.at("scenes").at("default_scene").at("objects");
    REQUIRE(objects.size() == 2);
    const auto primitive = [](std::string name, std::string model, double x) {
        return Json{{"name", std::move(name)},
                    {"components",
                     Json::array({{{"name", "transform"},
                                   {"pos", {x, 0.0, 0.0}},
                                   {"rotation", {0.0, 0.0, 0.0, 1.0}},
                                   {"scale", {0.5, 0.5, 0.5}}},
                                  {{"name", "simplemodelview"}, {"model", std::move(model)}}})}};
    };
    objects.push_back(primitive("Crate", "engine://primitives/cube", -0.5));
    objects.push_back(primitive("Hero", "engine://primitives/sphere", 0.5));
    writeFile(root / "scenes/main.scene.json", scene.dump(2));
    const GraphInstanceOwner hero_owner{"default_scene", 4};
    const auto capture = dir.root / "capture.png";

    const auto treeObject = [](const Json &tree, std::string_view name) {
        for (const auto &object : tree.at("objects"))
            if (object.value("name", std::string{}) == name) return object;
        return Json{};
    };

    std::uint64_t copy_id = 0;
    {
        FastModuleContainer modules;
        configureHeadless(root);
        Rpc rpc;
        (void)rpc.result("load_scene", {{"name", "default_scene"}});
        (void)rpc.result("step_frame");
        const auto initial_tree = rpc.result("scene_tree");
        REQUIRE(treeObject(initial_tree, "Hero").at("authoring_object_id") == hero_owner.object);
        REQUIRE(treeObject(initial_tree, "Crate").at("authoring_object_id") == 3);
        auto &loader = GET_MODULE(SceneLoader);
        REQUIRE(loader.graphOwnerObject(hero_owner) == loader.objectId("Hero"));
        (void)rpc.frame(capture);

        const auto actor = rpc.result("open_editor_session", {{"display_name", "OL-1"}}).at("actor_id")
                               .get<std::uint64_t>();
        const auto duplicated =
            rpc.edit(actor, Json::array({{{"op", "duplicate"}, {"object_id", 3}}}));
        INFO(duplicated.dump(2));
        REQUIRE(duplicated.at("status") == "committed");
        REQUIRE(duplicated.at("created_objects").size() == 1);
        const auto &created = duplicated.at("created_objects").at(0);
        CHECK(created.at("name") == "Crate_copy1");
        CHECK(created.at("declaration_index") == 3);
        CHECK(created.at("source_object_id") == 3);
        copy_id = created.at("object_id").get<std::uint64_t>();
        CHECK(copy_id == 5);
        std::cout << "OL1_DUPLICATE_RESULT " << duplicated.dump() << '\n';

        // scene_tree carries the ids; Hero moved to index 4 and kept id 4.
        const auto tree = rpc.result("scene_tree");
        CHECK(treeObject(tree, "Crate_copy1").at("authoring_object_id") == copy_id);
        CHECK(treeObject(tree, "Crate_copy1").at("declaration_index") == 3);
        CHECK(treeObject(tree, "Hero").at("authoring_object_id") == hero_owner.object);
        CHECK(treeObject(tree, "Hero").at("declaration_index") == 4);
        // The name table (objectId) is filled by scene load only, for spawn as
        // for duplicate; the live copy is reached through its authoring id.
        const GraphInstanceOwner copy_owner{"default_scene", copy_id};
        const auto live_copy = loader.graphOwnerObject(copy_owner);
        REQUIRE(live_copy);
        CHECK(live_copy != loader.objectId("Crate"));
        CHECK(loader.graphOwnerObject(hero_owner) == loader.objectId("Hero"));
        // The copy is drawn by the same lifecycle route a spawn takes.
        (void)rpc.frame(capture);

        const auto revert = [&](std::string_view method) {
            const auto revision = rpc.result("get_scene_revision").at("scene_revision");
            const auto accepted =
                rpc.result(method, {{"actor_id", actor}, {"base_revision", revision}});
            REQUIRE(accepted.at("status") == "accepted");
            (void)rpc.result("step_frame");
            const auto result = rpc.result("get_edit_result", {{"ticket", accepted.at("ticket")}});
            INFO(result.dump(2));
            REQUIRE(result.at("status") == "committed");
            return result;
        };
        (void)revert("undo");
        CHECK_FALSE(loader.graphOwnerObject(copy_owner));
        CHECK(treeObject(rpc.result("scene_tree"), "Crate_copy1").is_null());
        (void)rpc.frame(capture);
        const auto redone = revert("redo");
        CHECK(redone.at("created_objects") == duplicated.at("created_objects"));
        CHECK(loader.graphOwnerObject(copy_owner));
        CHECK(treeObject(rpc.result("scene_tree"), "Crate_copy1").at("authoring_object_id") ==
              copy_id);
        (void)rpc.frame(capture);

        const auto saved = rpc.result("save_scene");
        CHECK(saved.at("byte_count").get<std::uint64_t>() != 0);
        GET_MODULE(VulkanManageCore).waitIdle();
    }

    const auto saved_scene = Json::parse(readFile(root / "scenes/main.scene.json"));
    const auto &saved_objects = saved_scene.at("scenes").at("default_scene").at("objects");
    REQUIRE(saved_objects.size() == 5);
    CHECK(saved_objects.at(3).at("name") == "Crate_copy1");
    CHECK(saved_objects.at(4).at("name") == "Hero");

    {
        FastModuleContainer modules;
        configureHeadless(root);
        Rpc rpc;
        (void)rpc.result("load_scene", {{"name", "default_scene"}});
        (void)rpc.result("step_frame");
        const auto tree = rpc.result("scene_tree");
        CHECK(treeObject(tree, "Hero").at("authoring_object_id") == hero_owner.object);
        CHECK(treeObject(tree, "Crate_copy1").at("authoring_object_id") == copy_id);
        auto &loader = GET_MODULE(SceneLoader);
        REQUIRE(loader.objectId("Hero"));
        CHECK(loader.graphOwnerObject(hero_owner) == loader.objectId("Hero"));
        CHECK(loader.graphOwnerObject(GraphInstanceOwner{"default_scene", copy_id}) ==
              loader.objectId("Crate_copy1"));
        GET_MODULE(VulkanManageCore).waitIdle();
    }
}

TEST_CASE("GP-5 a declared primitive takes its material from the existing material binding route",
          "[gp5][primitives][material-binding][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("binding");
    const auto root = makeBlankProject(dir.root);
    writeFile(root / "materials/red.material.json",
              R"json({"schema":"pelican.material","version":1,"materials":[{"name":"ProjectRed","surface":"engine://surfaces/openpbr/opaque_single.surface","base":{"baseColorFactor":[0.9,0.1,0.1,1.0]},"routing":{"alpha_mode":"opaque","double_sided":false}}]})json");
    writeFile(root / "materials/floor.bindings.json",
              R"json({"schema":"pelican.material_bindings","version":1,"model":"engine://primitives/plane","bindings":[{"usd_path":"/Floor","mesh":0,"primitive":0,"material":"ProjectRed","routing":{"alpha_mode":"opaque","double_sided":false}}]})json");
    writeFile(root / "assets/asset_data.json",
              R"json({"schema":"pelican.asset_data","version":2,"models":[{"name":"red_floor","path":"engine://primitives/plane","material_bindings":"materials/floor.bindings.json"}],"materials":[{"path":"materials/red.material.json"}]})json");

    FastModuleContainer modules;
    configureHeadless(root);
    auto &models = GET_MODULE(ModelAssetContainer);
    const auto &floor = models.getModelTemplateByName("red_floor");
    const auto &project = GET_MODULE(ProjectMaterialAssetContainer).namedMaterials();
    const auto red = std::find_if(project.begin(), project.end(),
                                  [](const auto &material) { return material.name == "ProjectRed"; });
    REQUIRE(red != project.end());
    REQUIRE(floor.material_primitives.size() == 1);
    CHECK(floor.material_primitives.front().material == red->material);
    CHECK(indexCount(floor) == 6);
    // The undeclared reference keeps the default material.
    const auto &plain = models.getModelTemplateByName("engine://primitives/plane");
    REQUIRE(plain.material_primitives.size() == 1);
    CHECK_FALSE(plain.material_primitives.front().material == red->material);
    GET_MODULE(VulkanManageCore).waitIdle();
}

namespace {

// G-4: a direct root with one forward_opaque material pass and `present`,
// which passes the pass's colour through. The forward pass clears to white,
// so an unlit cube is a dark shape on white.
void writeForwardOnlyProject(const std::filesystem::path &root, bool forward_default) {
    Json rendering{
        {"render_targets",
         Json::array({{{"name", "scene_color"},
                       {"extent_scale", 1.0},
                       {"format", "R16G16B16A16_SFLOAT"},
                       {"format_class", "explicit(R16G16B16A16_SFLOAT)"},
                       {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}},
                      {{"name", "scene_depth"},
                       {"extent_scale", 1.0},
                       {"format", "D32_SFLOAT"},
                       {"format_class", "data"},
                       {"usage", {"DEPTH_STENCIL_ATTACHMENT", "SAMPLED"}}}})},
        {"rendering_passes",
         Json::array({{{"name", "main_render"},
                       {"passes",
                        Json::array({{{"name", "forward"},
                                      {"type", "material"},
                                      {"material_contract", "forward_opaque_v1"},
                                      {"clear_color", {1.0, 1.0, 1.0, 1.0}},
                                      {"output", {{"color", {"scene_color"}}, {"depth", "scene_depth"}}}},
                                     {{"name", "present"},
                                      {"type", "fullscreen"},
                                      {"input", {"scene_color"}},
                                      {"output", {{"color", {"display"}}, {"depth", nullptr}}},
                                      {"shader",
                                       {{"vertex", "engine://fullscreen"},
                                        {"fragment", "project://shaders/present"}}}}})}}})},
    };
    if (forward_default) rendering["material_route_default"] = "forward";
    writeFile(root / "passes/main_rendering_config.json",
              Pelican::TestSupport::renderGraphRoot(rendering).dump(2));
    writeFile(root / "shaders/present.frag",
              "#version 460\n#extension GL_GOOGLE_include_directive : require\n"
              "#include \"pelican_sets.glsl\"\n#include \"pelican_view.glsl\"\n\n"
              "layout(location = 0) in vec2 inUV;\nlayout(location = 0) out vec4 outColor;\n"
              "PELICAN_DECLARE_INPUT_0(in_scene_color);\n\n"
              "void main() {\n    outColor = PELICAN_TEXTURE_2D_0(in_scene_color, inUV);\n}\n");
}

} // namespace

TEST_CASE("G-4 a forward-only direct root draws the basic cube once material_route_default is forward",
          "[g4][primitives][material-routing][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("g4_forward");
    const auto root = makeBlankProject(dir.root);
    writeForwardOnlyProject(root, true);
    const auto capture = dir.root / "capture.png";

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");
    const auto background = rpc.frame(capture);

    const auto actor =
        rpc.result("open_editor_session", {{"display_name", "G-4"}}).at("actor_id").get<std::uint64_t>();
    const auto spawned = rpc.edit(actor, spawnCube("engine://primitives/cube"));
    INFO(spawned.dump(2));
    REQUIRE(spawned.at("status") == "committed");
    const auto drawn = rpc.frame(capture);
    CHECK(differingPixels(background, drawn) > 64);

    // The cube's default material and every built-in follow the default.
    auto &materials = GET_MODULE(MaterialContainer);
    const auto &cube = GET_MODULE(ModelAssetContainer).getModelTemplateByName("engine://primitives/cube");
    REQUIRE(cube.material_primitives.size() == 1);
    CHECK(materials.routeForMaterial(cube.material_primitives.front().material) ==
          MaterialRouteClass::forward_opaque);
    auto &standard = GET_MODULE(StandardMaterialResource);
    CHECK(materials.routeForMaterial(standard.standardTransparentMaterial()) ==
          MaterialRouteClass::forward_opaque);
    for (const auto *suffix : {"sphere", "skinned_fallback", "default"}) {
        CAPTURE(suffix);
        const auto info = standard.builtinMaterialInfo(suffix, standard.whiteTexture());
        CHECK(info.route == MaterialRouteClass::forward_opaque);
        CHECK(info.shader_contract == MaterialShaderContract::forward_scene_color_v1);
        CHECK(info.name == std::string{"pelican.builtin."} + suffix);
        CHECK(info.frag_shader != standard.standardFragShader());
    }
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("G-4 a forward-only direct root without the forward default stops loading by name",
          "[g4][primitives][material-routing][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("g4_auto");
    const auto root = makeBlankProject(dir.root);
    writeForwardOnlyProject(root, false);

    std::string failure;
    try {
        FastModuleContainer modules;
        configureHeadless(root);
        Rpc rpc;
        const auto loaded = rpc.call("load_scene", {{"name", "default_scene"}});
        if (loaded.contains("error")) {
            failure = loaded.dump();
        } else {
            // The composition root's boot step (appflow/loop.cpp) makes the
            // engine's built-in materials before the first frame.
            GET_MODULE(Renderer).prepareRuntimeModules();
        }
        if (auto *core = FastModuleContainer::tryGet<VulkanManageCore>()) core->waitIdle();
    } catch (const std::exception &error) {
        failure = error.what();
    }
    INFO(failure);
    CHECK(failure.find("material_route_unserved@1") != std::string::npos);
    CHECK(failure.find("'pelican.builtin.transparent' (engine built-in)") != std::string::npos);
    CHECK(failure.find("route 'deferred_geometry'") != std::string::npos);
    CHECK(failure.find("Hint: set material_route_default to ") != std::string::npos);
}

TEST_CASE("G-4 an edit that leaves the engine's materials without a pass is refused through the running renderer",
          "[g4][primitives][material-routing][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("g4_edit");
    const auto root = makeBlankProject(dir.root);
    // The blank project's own rendering section: `present` only, so no
    // material pass and nothing for the previous generation to consume. The
    // two targets a forward pass needs are declared already.
    writeForwardOnlyProject(root, false);
    const auto rendering_path = root / "passes/main_rendering_config.json";
    const auto present_reads = [&](bool scene_color) {
        auto authored = Json::parse(readFile(rendering_path));
        auto &passes = renderingSectionOf(authored).at("rendering_passes").at(0).at("passes");
        if (passes.at(0).at("name") == "forward" && !scene_color) passes.erase(passes.begin());
        for (auto &pass : passes)
            if (pass.at("name") == "present")
                pass["input"] = scene_color ? Json::array({"scene_color"}) : Json::array();
        writeFile(rendering_path, authored.dump(2));
    };
    const auto present_frag = readFile(root / "shaders/present.frag");
    present_reads(false);
    writeFile(root / "shaders/present.frag",
              "#version 460\nlayout(location = 0) in vec2 inUV;\n"
              "layout(location = 0) out vec4 outColor;\n"
              "void main() { outColor = vec4(inUV.x, inUV.y, 0.5, 1.0); }\n");
    const auto capture = dir.root / "capture.png";

    const auto forward_pass = Json{{"name", "forward"},
                                   {"type", "material"},
                                   {"material_contract", "forward_opaque_v1"},
                                   {"clear_color", {1.0, 1.0, 1.0, 1.0}},
                                   {"output", {{"color", {"scene_color"}}, {"depth", "scene_depth"}}}};
    {
        FastModuleContainer modules;
        configureHeadless(root);
        Rpc rpc;
        (void)rpc.result("load_scene", {{"name", "default_scene"}});
        GET_MODULE(Renderer).prepareRuntimeModules();
        (void)rpc.result("step_frame");
        const auto add = [&](std::optional<std::string> route_default) {
            const auto context = rpc.result("get_render_authoring_context");
            Json params{{"base_source_digest", context.at("source_digest").at("hex")},
                        {"graph", "main_render"},
                        {"insert", "before:present"},
                        {"pass", forward_pass}};
            if (route_default) params["material_route_default"] = *route_default;
            const auto accepted = rpc.result("add_authored_pass", params);
            INFO(accepted.dump(2));
            REQUIRE(accepted.at("status") == "accepted");
            for (int attempt = 0; attempt < 8; ++attempt) {
                (void)rpc.result("step_frame");
                auto result = rpc.result("get_edit_result", {{"ticket", accepted.at("ticket")}});
                if (result.value("status", std::string{}) != "pending") return result;
            }
            FAIL("the authored pass ticket stayed pending");
            return Json{};
        };

        // G-2's order: a cube is spawned while nothing draws materials, so
        // its default grey is lowered deferred and registered unchecked.
        const auto actor = rpc.result("open_editor_session", {{"display_name", "G-4"}})
                               .at("actor_id")
                               .get<std::uint64_t>();
        const auto spawned = rpc.edit(actor, spawnCube("engine://primitives/cube"));
        INFO(spawned.dump(2));
        REQUIRE(spawned.at("status") == "committed");

        // The previous generation drew no material at all. A forward-only
        // pipeline still leaves the built-in material, and the cube's, without
        // a pass: the edit is refused instead of leaving them undrawn.
        const auto root_before = readFile(rendering_path);
        const auto refused = add(std::nullopt);
        INFO(refused.dump(2));
        REQUIRE(refused.at("status") == "rejected");
        const auto &error = refused.at("error");
        CHECK(error.at("code") == "material_route_unserved@1");
        REQUIRE(error.at("unserved").size() == 1);
        CHECK(error.at("unserved").at(0).at("route") == "deferred_geometry");
        const auto names = error.at("unserved").at(0).at("materials").dump();
        CHECK(names.find("pelican.builtin.transparent") != std::string::npos);
        CHECK(names.find(std::string{enginePrimitiveDefaultMaterialName}) != std::string::npos);
        CHECK(names.find("engine://primitives/cube") != std::string::npos);
        CHECK_THAT(error.at("hint").get<std::string>(),
                   Catch::Matchers::ContainsSubstring("material_route_default"));
        CHECK(readFile(rendering_path) == root_before);
        CHECK_FALSE(std::filesystem::exists(root / "passes/authoring"));

        // With the forward default in the same ticket, the candidate is
        // judged by the routes the restart will give every material.
        const auto committed = add(std::string{"forward"});
        INFO(committed.dump(2));
        REQUIRE(committed.at("committed") == true);
        CHECK(committed.at("restart_required") == true);
        GET_MODULE(VulkanManageCore).waitIdle();
    }
    CHECK(readMaterialRouteDefault(renderingSectionOrEmpty(
              Json::parse(readFile(rendering_path)))) == MaterialRouteDefault::forward);
    // present reads what the new pass draws, as the scenario's drag makes it.
    present_reads(true);
    writeFile(root / "shaders/present.frag", present_frag);

    // The restart: the same project, lowered forward, draws the cube.
    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");
    const auto background = rpc.frame(capture);
    const auto actor =
        rpc.result("open_editor_session", {{"display_name", "G-4"}}).at("actor_id").get<std::uint64_t>();
    const auto spawned = rpc.edit(actor, spawnCube("engine://primitives/cube"));
    INFO(spawned.dump(2));
    REQUIRE(spawned.at("status") == "committed");
    CHECK(differingPixels(background, rpc.frame(capture)) > 64);
    GET_MODULE(VulkanManageCore).waitIdle();
}

// RS-1: the Graph palette's first material pass on a blank project (the
// template's own rendering section: `present` only). The live engine cannot
// rebuild its materials' pipelines for the G-buffer, so the edit is committed
// for the next start and answers restart_required; the next start takes it.
TEST_CASE("RS-1 a graph edit the live materials cannot take is committed and asks for a restart",
          "[rs1][primitives][material-routing][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("rs1_restart");
    const auto root = dir.root / "project";
    initializeProjectTemplate(root, ProjectTemplate::Blank);
    const auto rendering_path = root / "passes/main_rendering_config.json";

    // What the palette's render.material_pass (deferred_geometry_v1) sends:
    // the pass and the six G-buffer images it writes, one op list.
    const Json color_type{{"semantic", "pelican.render.color_signal@1"},
                          {"arguments", {{"reference", "scene"}, {"transfer", "linear"},
                                         {"range", "extended"}}}};
    const auto image_op = [](const std::string &id, const Json &type, const char *format,
                             const char *usage) {
        return Json{{"op", "add_node"},
                    {"scope", "root"},
                    {"node", {{"id", id},
                              {"op", "render.image"},
                              {"parameters", {{"type", type},
                                              {"format", format},
                                              {"extent_scale", 1.0},
                                              {"usage", Json::array({usage, "SAMPLED"})}}},
                              {"outputs", Json::array({"resource"})}}}};
    };
    Json ops = Json::array();
    Json inputs = Json::object();
    for (const auto *socket : {"albedo", "normal", "material", "worldpos", "emissive"}) {
        const auto id = std::string{"render_material_pass__"} + socket;
        ops.push_back(image_op(id, color_type,
                               std::string_view{socket} == "material" ? "R8G8B8A8_UNORM"
                                                                      : "R16G16B16A16_SFLOAT",
                               "COLOR_ATTACHMENT"));
        inputs[socket] = {{"node", id}, {"port", "resource"}};
    }
    ops.push_back(image_op("render_material_pass__depth",
                           Json{{"semantic", "pelican.render.depth@1"},
                                {"arguments", {{"representation", "device"}, {"space", "projection"}}}},
                           "D32_SFLOAT", "DEPTH_STENCIL_ATTACHMENT"));
    inputs["depth"] = {{"node", "render_material_pass__depth"}, {"port", "resource"}};
    ops.push_back({{"op", "add_node"},
                   {"scope", "root"},
                   {"node", {{"id", "render_material_pass"},
                             {"op", "render.material_pass"},
                             {"parameters", {{"material_contract", "deferred_geometry_v1"},
                                             {"pass_set", "main_render"}}},
                             {"inputs", inputs},
                             {"outputs", Json::array()}}}});

    {
        FastModuleContainer modules;
        configureHeadless(root);
        Rpc rpc;
        (void)rpc.result("load_scene", {{"name", "default_scene"}});
        GET_MODULE(Renderer).prepareRuntimeModules();
        (void)rpc.result("step_frame");
        const auto edit = [&](const Json &edit_ops) {
            const auto context = rpc.result("get_render_authoring_context");
            const auto accepted = rpc.result(
                "edit_graph_document",
                {{"base_source_digest", context.at("source_digest").at("hex")}, {"ops", edit_ops}});
            INFO(accepted.dump(2));
            REQUIRE(accepted.at("status") == "accepted");
            for (int attempt = 0; attempt < 8; ++attempt) {
                (void)rpc.result("step_frame");
                auto result = rpc.result("get_edit_result", {{"ticket", accepted.at("ticket")}});
                if (result.value("status", std::string{}) != "pending") return result;
            }
            FAIL("the graph edit ticket stayed pending");
            return Json{};
        };

        const auto committed = edit(ops);
        INFO(committed.dump(2));
        REQUIRE(committed.at("status") == "committed");
        CHECK(committed.at("restart_required") == true);
        const auto &reason = committed.at("restart_reason");
        CHECK(reason.at("code") == "material_physical_contract_changed");
        CHECK(reason.at("pass") == "render_material_pass");
        CHECK(reason.at("material").is_string());
        CHECK_FALSE(reason.at("detail").empty());
        UNSCOPED_INFO("restart_reason = " << reason.dump());
        // The document holds the pass; the live frame does not.
        CHECK(readFile(rendering_path).find("render.material_pass") != std::string::npos);
        CHECK(rpc.result("get_frame_plan").dump().find("render_material_pass") == std::string::npos);

        // Before the restart the next edit meets the same live materials.
        const auto again = edit(Json::array({{{"op", "set_node_label"}, {"scope", "root"},
                                              {"node", "render_material_pass"}, {"label", "G-buffer"}}}));
        INFO(again.dump(2));
        REQUIRE(again.at("status") == "committed");
        CHECK(again.at("restart_required") == true);
        CHECK(again.at("restart_reason").at("code") == "material_physical_contract_changed");
        (void)rpc.result("render_frame");
        GET_MODULE(VulkanManageCore).waitIdle();
    }

    // The restart: the same documents compose, and the pass is drawn.
    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    GET_MODULE(Renderer).prepareRuntimeModules();
    (void)rpc.result("step_frame");
    (void)rpc.result("render_frame");
    CHECK(rpc.result("get_frame_plan").dump().find("render_material_pass") != std::string::npos);
    GET_MODULE(VulkanManageCore).waitIdle();
}

// RS-2: the Graph palette's Forward opaque material pass on a blank project,
// with the draw_loop that supplies its draws (as decompose_material_pass
// writes one). Nothing in the blank pipeline draws deferred, so the live
// materials are unserved unless the same ticket makes the forward default the
// project's: edit_graph_document carries material_route_default as
// add_authored_pass does.
TEST_CASE("RS-2 edit_graph_document carries material_route_default for a forward material pass",
          "[rs2][primitives][material-routing][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("rs2_route_default");
    const auto root = dir.root / "project";
    initializeProjectTemplate(root, ProjectTemplate::Blank);
    const auto rendering_path = root / "passes/main_rendering_config.json";

    const auto image_op = [](const std::string &id, const Json &type, const char *format,
                             const char *usage) {
        return Json{{"op", "add_node"},
                    {"scope", "root"},
                    {"node", {{"id", id},
                              {"op", "render.image"},
                              {"parameters", {{"type", type},
                                              {"format", format},
                                              {"extent_scale", 1.0},
                                              {"usage", Json::array({usage, "SAMPLED"})}}},
                              {"outputs", Json::array({"resource"})}}}};
    };
    Json ops = Json::array();
    ops.push_back(image_op("render_material_pass__color",
                           Json{{"semantic", "pelican.render.color_signal@1"},
                                {"arguments", {{"reference", "scene"}, {"transfer", "linear"},
                                               {"range", "extended"}}}},
                           "R16G16B16A16_SFLOAT", "COLOR_ATTACHMENT"));
    ops.push_back(image_op("render_material_pass__depth",
                           Json{{"semantic", "pelican.render.depth@1"},
                                {"arguments", {{"representation", "device"}, {"space", "projection"}}}},
                           "D32_SFLOAT", "DEPTH_STENCIL_ATTACHMENT"));
    // The draw_loop: over the contract's draw items, one else branch whose
    // body draws each material (materialpassdecompose.cpp's shape).
    ops.push_back({{"op", "add_definition"},
                   {"name", "render_material_pass_draw"},
                   {"definition",
                    {{"nodes", Json::array({{{"id", "step"},
                                             {"op", "draw.material"},
                                             {"parameters", Json::object()},
                                             {"outputs", Json::array({"draws"})}}})},
                     {"outputs", {{"draws", {{"node", "step"}, {"port", "draws"}}}}}}}});
    ops.push_back({{"op", "add_node"},
                   {"scope", "root"},
                   {"node", {{"id", "render_material_pass_draws"},
                             {"op", "graph.loop"},
                             {"parameters",
                              {{"over", {{"draw_items", {{"contract", "forward_opaque_v1"}}}}},
                               {"order", {{"by", "phase_default"}}},
                               {"branches", Json::array({{{"body", "render_material_pass_draw"}}})}}},
                             {"outputs", Json::array({"draws"})}}}});
    ops.push_back({{"op", "add_node"},
                   {"scope", "root"},
                   {"node", {{"id", "render_material_pass"},
                             {"op", "render.material_pass"},
                             {"parameters", {{"material_contract", "forward_opaque_v1"},
                                             {"pass_set", "main_render"}}},
                             {"inputs",
                              {{"color", {{"node", "render_material_pass__color"}, {"port", "resource"}}},
                               {"depth", {{"node", "render_material_pass__depth"}, {"port", "resource"}}},
                               {"draws", {{"node", "render_material_pass_draws"}, {"port", "draws"}}}}},
                             {"outputs", Json::array()}}}});

    {
        FastModuleContainer modules;
        configureHeadless(root);
        Rpc rpc;
        (void)rpc.result("load_scene", {{"name", "default_scene"}});
        GET_MODULE(Renderer).prepareRuntimeModules();
        (void)rpc.result("step_frame");
        // The final answer, whether the ticket is refused when it is asked
        // for or when the running renderer judges it.
        const auto edit = [&](std::optional<std::string> route_default) {
            const auto context = rpc.result("get_render_authoring_context");
            Json params{{"base_source_digest", context.at("source_digest").at("hex")}, {"ops", ops}};
            if (route_default) params["material_route_default"] = *route_default;
            const auto accepted = rpc.result("edit_graph_document", params);
            if (accepted.value("status", std::string{}) != "accepted") return accepted;
            for (int attempt = 0; attempt < 8; ++attempt) {
                (void)rpc.result("step_frame");
                auto result = rpc.result("get_edit_result", {{"ticket", accepted.at("ticket")}});
                if (result.value("status", std::string{}) != "pending") return result;
            }
            FAIL("the graph edit ticket stayed pending");
            return Json{};
        };

        // Without the forward default: refused by name, as today, and the
        // document is what it was.
        const auto root_before = readFile(rendering_path);
        const auto refused = edit(std::nullopt);
        INFO(refused.dump(2));
        REQUIRE(refused.at("status") == "rejected");
        CHECK(refused.at("error").at("code") == "material_route_unserved@1");
        CHECK(readFile(rendering_path) == root_before);

        // With it, in the same ticket: committed with the default written
        // and a restart asked for.
        const auto committed = edit(std::string{"forward"});
        INFO(committed.dump(2));
        REQUIRE(committed.at("status") == "committed");
        CHECK(committed.at("restart_required") == true);
        const auto code = committed.at("restart_reason").at("code").get<std::string>();
        CHECK((code == "material_physical_contract_changed" || code == "material_route_default_changed"));
        std::cout << "RS-2 restart_reason = " << committed.at("restart_reason").dump() << "\n";
        const auto written = Json::parse(readFile(rendering_path));
        CHECK(readMaterialRouteDefault(renderingSectionOrEmpty(written)) ==
              MaterialRouteDefault::forward);
        CHECK(written.dump().find("render_material_pass_draws") != std::string::npos);
        (void)rpc.result("render_frame");
        GET_MODULE(VulkanManageCore).waitIdle();
    }

    // The restart: the same documents compose, and the pass is in the plan.
    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    GET_MODULE(Renderer).prepareRuntimeModules();
    (void)rpc.result("step_frame");
    (void)rpc.result("render_frame");
    CHECK(rpc.result("get_frame_plan").dump().find("render_material_pass") != std::string::npos);
    GET_MODULE(VulkanManageCore).waitIdle();
}

namespace {

// Q-14e: the blank template's direct root with three targets `present` can
// read, each drawn by its own fullscreen pass before it.
constexpr std::string_view q14e_pattern_frag =
    "#version 460\nlayout(location = 0) in vec2 inUV;\n"
    "layout(location = 0) out vec4 outColor;\n"
    "void main() { outColor = vec4(inUV.x, inUV.y, 0.5, 1.0); }\n";

// Reads two pass inputs, at bindings 0 and 1.
constexpr std::string_view q14e_two_inputs_frag =
    "#version 460\n#extension GL_GOOGLE_include_directive : require\n"
    "#include \"pelican_sets.glsl\"\n#include \"pelican_view.glsl\"\n\n"
    "layout(location = 0) in vec2 inUV;\nlayout(location = 0) out vec4 outColor;\n"
    "PELICAN_DECLARE_INPUT_0(in_a);\nPELICAN_DECLARE_INPUT_1(in_b);\n\n"
    "void main() {\n"
    "    outColor = PELICAN_TEXTURE_2D_0(in_a, inUV) + PELICAN_TEXTURE_2D_1(in_b, inUV);\n"
    "}\n";

const Json q14e_present_output{{"color", {"display"}}, {"depth", nullptr}};
const Json q14e_present_shader{{"vertex", "engine://fullscreen"},
                               {"fragment", "project://shaders/present"}};

void writeQ14eProject(const std::filesystem::path &root, const Json &present_input,
                      std::string_view present_frag) {
    initializeProjectTemplate(root, ProjectTemplate::Blank);
    auto targets = Json::array();
    auto passes = Json::array();
    for (const auto *name : {"q14e_a", "q14e_b", "q14e_c"}) {
        targets.push_back({{"name", name},
                           {"extent_scale", 1.0},
                           {"format", "R16G16B16A16_SFLOAT"},
                           {"format_class", "explicit(R16G16B16A16_SFLOAT)"},
                           {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}});
        passes.push_back({{"name", std::string{"draw_"} + name},
                          {"type", "fullscreen"},
                          {"input", Json::array()},
                          {"output", {{"color", {name}}, {"depth", nullptr}}},
                          {"shader",
                           {{"vertex", "engine://fullscreen"},
                            {"fragment", "project://shaders/q14e_pattern"}}}});
    }
    passes.push_back({{"name", "present"},
                      {"type", "fullscreen"},
                      {"input", present_input},
                      {"output", q14e_present_output},
                      {"shader", q14e_present_shader}});
    const Json rendering{{"render_targets", std::move(targets)},
                         {"rendering_passes",
                          Json::array({{{"name", "main_render"}, {"passes", std::move(passes)}}})}};
    writeFile(root / "passes/main_rendering_config.json",
              Pelican::TestSupport::renderGraphRoot(rendering).dump(2));
    writeFile(root / "shaders/q14e_pattern.frag", q14e_pattern_frag);
    writeFile(root / "shaders/present.frag", present_frag);
}

// Studio's edit of `present`'s inputs (and, Q-14f, its shader), and the
// ticket's terminal result.
Json editPresent(Rpc &rpc, const Json &inputs, const Json &shader) {
    const auto context = rpc.result("get_render_authoring_context");
    const auto sent = rpc.result("edit_preset_pass",
                                 {{"base_source_digest", context.at("source_digest").at("hex")},
                                  {"graph", "main_render"},
                                  {"pass_name", "present"},
                                  {"input", inputs},
                                  {"output", q14e_present_output},
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

Json editPresentInputs(Rpc &rpc, const Json &inputs) {
    return editPresent(rpc, inputs, q14e_present_shader);
}

constexpr std::string_view q14e_texture_lead =
    "Fullscreen pass input texture/local-read contract does not match shader reflection";

// Q-14f: shaders that declare more pass inputs than present supplies. Each
// writes what it reads with alpha 1, so a default (zero) input reads black.
// Declares bindings 0 and 1 and reads 1.
constexpr std::string_view q14f_reads_second_frag =
    "#version 460\n#extension GL_GOOGLE_include_directive : require\n"
    "#include \"pelican_sets.glsl\"\n#include \"pelican_view.glsl\"\n\n"
    "layout(location = 0) in vec2 inUV;\nlayout(location = 0) out vec4 outColor;\n"
    "PELICAN_DECLARE_INPUT_0(in_a);\nPELICAN_DECLARE_INPUT_1(in_b);\n\n"
    "void main() {\n"
    "    outColor = vec4(PELICAN_TEXTURE_2D_1(in_b, inUV).rgb, 1.0);\n"
    "}\n";

// Declares and reads binding 0.
constexpr std::string_view q14f_reads_one_frag =
    "#version 460\n#extension GL_GOOGLE_include_directive : require\n"
    "#include \"pelican_sets.glsl\"\n#include \"pelican_view.glsl\"\n\n"
    "layout(location = 0) in vec2 inUV;\nlayout(location = 0) out vec4 outColor;\n"
    "PELICAN_DECLARE_INPUT_0(in_a);\n\n"
    "void main() {\n"
    "    outColor = vec4(PELICAN_TEXTURE_2D_0(in_a, inUV).rgb, 1.0);\n"
    "}\n";

// Reads binding 0 and declares a uniform buffer at binding 1: a type the
// engine does not fill.
constexpr std::string_view q14f_uniform_second_frag =
    "#version 460\n#extension GL_GOOGLE_include_directive : require\n"
    "#include \"pelican_sets.glsl\"\n#include \"pelican_view.glsl\"\n\n"
    "layout(location = 0) in vec2 inUV;\nlayout(location = 0) out vec4 outColor;\n"
    "PELICAN_DECLARE_INPUT_0(in_a);\n"
    "layout(set = PELICAN_SET_PASS_INPUT, binding = 1) uniform Q14fTint { vec4 tint; } q14f_tint;\n\n"
    "void main() {\n"
    "    outColor = vec4(PELICAN_TEXTURE_2D_0(in_a, inUV).rgb * q14f_tint.tint.rgb, 1.0);\n"
    "}\n";

Json q14fPresentShader(std::string_view stem) {
    return Json{{"vertex", "engine://fullscreen"},
                {"fragment", "project://shaders/" + std::string{stem}}};
}

// How many pixels of a capture are not black.
std::size_t q14fLitPixels(const std::vector<unsigned char> &rgba) {
    std::size_t lit = 0;
    for (std::size_t pixel = 0; pixel + 3 < rgba.size(); pixel += 4) {
        if (rgba[pixel] > 2 || rgba[pixel + 1] > 2 || rgba[pixel + 2] > 2) ++lit;
    }
    return lit;
}

PassId q14fPresentPassId() {
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

// The running present pass's default-filled bindings.
std::vector<std::uint32_t> q14fPresentFilledBindings() {
    std::vector<std::uint32_t> bindings;
    for (const auto &entry :
         GET_MODULE(FullscreenPassContainer).filledPassInputs(q14fPresentPassId()))
        bindings.push_back(entry.binding);
    return bindings;
}

// The default descriptors written into present's sets: one per filled
// binding in each of the two history-parity sets of its one variant.
std::size_t q14fPresentFilledDescriptorWrites() {
    return GET_MODULE(FullscreenPassContainer)
        .filledDescriptorWritesForTesting(q14fPresentPassId());
}

} // namespace

TEST_CASE("Q-14e a present that reads nothing refuses an added input by binding, through the running renderer",
          "[q14e][render-config-editor][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("q14e_none");
    const auto root = dir.root / "project";
    // The blank template's present.frag: it reads nothing.
    writeQ14eProject(root, Json::array(), q14e_pattern_frag);
    const auto rendering_path = root / "passes/main_rendering_config.json";

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");
    const auto root_before = readFile(rendering_path);

    const auto refused = editPresentInputs(rpc, Json::array({"q14e_a"}));
    INFO(refused.dump(2));
    REQUIRE(refused.at("status") == "rejected");
    CHECK(refused.at("committed") == false);
    const auto &error = refused.at("error");
    CHECK(error.at("code") == "render_pipeline_preflight_failed");
    const auto message = error.at("message").get<std::string>();
    CHECK(message.rfind(q14e_texture_lead, 0) == 0);
    CHECK_THAT(message, Catch::Matchers::ContainsSubstring("pass 'present'"));
    REQUIRE(error.contains("shader_input_mismatch"));
    CHECK(error.at("shader_input_mismatch") ==
          Json{{"pass", "present"},
               {"fragment", "project://shaders/present"},
               {"direction", "pass_input_undeclared"},
               {"kind", "texture"},
               {"binding", 0},
               {"resource", "q14e_a"},
               {"expected", "sampled_image"},
               {"pass_texture_inputs", 1},
               {"pass_buffer_inputs", 0},
               {"shader_pass_inputs", Json::array()},
               // VI-1: no variadic group.
               {"variadic_range", nullptr}});
    CHECK(readFile(rendering_path) == root_before);

    // A refusal for another reason carries no shader_input_mismatch.
    const auto unknown = editPresentInputs(rpc, Json::array({"q14e_nobody_declared"}));
    INFO(unknown.dump(2));
    REQUIRE(unknown.at("status") == "rejected");
    CHECK(unknown.at("error").at("code") == "render_pipeline_preflight_failed");
    CHECK_FALSE(unknown.at("error").contains("shader_input_mismatch"));
    CHECK(readFile(rendering_path) == root_before);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("Q-14e a present that reads two inputs refuses a third at binding 2, through the running renderer",
          "[q14e][render-config-editor][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("q14e_two");
    const auto root = dir.root / "project";
    writeQ14eProject(root, Json::array({"q14e_a", "q14e_b"}), q14e_two_inputs_frag);

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");

    const auto refused = editPresentInputs(rpc, Json::array({"q14e_a", "q14e_b", "q14e_c"}));
    INFO(refused.dump(2));
    REQUIRE(refused.at("status") == "rejected");
    const auto &error = refused.at("error");
    CHECK(error.at("code") == "render_pipeline_preflight_failed");
    CHECK(error.at("message").get<std::string>().rfind(q14e_texture_lead, 0) == 0);
    REQUIRE(error.contains("shader_input_mismatch"));
    const auto &mismatch = error.at("shader_input_mismatch");
    CHECK(mismatch.at("direction") == "pass_input_undeclared");
    CHECK(mismatch.at("binding") == 2);
    CHECK(mismatch.at("resource") == "q14e_c");
    CHECK(mismatch.at("pass_texture_inputs") == 3);
    // VI-1: each declaration carries its variable name and whether it is
    // variadic.
    CHECK(mismatch.at("shader_pass_inputs") ==
          Json::array({{{"binding", 0}, {"type", "sampled_image"}, {"name", "in_a"}, {"variadic", false}},
                       {{"binding", 1}, {"type", "sampled_image"}, {"name", "in_b"}, {"variadic", false}}}));
    CHECK_THAT(error.at("message").get<std::string>(),
               Catch::Matchers::ContainsSubstring(
                   "input 2 ('q14e_c') expects a sampled image at set 1 binding 2") &&
                   Catch::Matchers::ContainsSubstring("(shader declares 2 pass inputs)"));

    // The two it reads are still accepted.
    const auto kept = editPresentInputs(rpc, Json::array({"q14e_b", "q14e_a"}));
    INFO(kept.dump(2));
    CHECK(kept.at("committed") == true);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("Q-14f a present that supplies one input fills a second binding its shader reads with black, through the running renderer",
          "[q14f][render-config-editor][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("q14f_one");
    const auto root = dir.root / "project";
    writeQ14eProject(root, Json::array({"q14e_a"}), q14f_reads_one_frag);
    writeFile(root / "shaders/q14f_reads_second.frag", q14f_reads_second_frag);
    const auto capture = dir.root / "capture.png";

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");
    // The control: present reads q14e_a's pattern, which is lit everywhere.
    const auto pattern = rpc.frame(capture);
    REQUIRE(q14fLitPixels(pattern) == pattern.size() / 4);
    CHECK(q14fPresentFilledBindings().empty());

    // Binding 1 is read and no input supplies it: the edit commits and the
    // engine binds its default (zeros) there.
    const auto edited =
        editPresent(rpc, Json::array({"q14e_a"}), q14fPresentShader("q14f_reads_second"));
    INFO(edited.dump(2));
    REQUIRE(edited.at("status") == "committed");
    CHECK(edited.at("committed") == true);
    CHECK(q14fPresentFilledBindings() == std::vector<std::uint32_t>{1});
    CHECK(q14fPresentFilledDescriptorWrites() == 2);
    const auto filled = rpc.frame(capture);
    CHECK(q14fLitPixels(filled) == 0);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("Q-14f a present with no inputs fills the binding its shader reads with black, through the running renderer",
          "[q14f][render-config-editor][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("q14f_none");
    const auto root = dir.root / "project";
    writeQ14eProject(root, Json::array(), q14e_pattern_frag);
    writeFile(root / "shaders/q14f_reads_one.frag", q14f_reads_one_frag);
    const auto capture = dir.root / "capture.png";

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");
    // The control: the blank present's gradient is lit everywhere.
    const auto gradient = rpc.frame(capture);
    REQUIRE(q14fLitPixels(gradient) == gradient.size() / 4);

    const auto edited =
        editPresent(rpc, Json::array(), q14fPresentShader("q14f_reads_one"));
    INFO(edited.dump(2));
    REQUIRE(edited.at("status") == "committed");
    CHECK(edited.at("committed") == true);
    CHECK(q14fPresentFilledBindings() == std::vector<std::uint32_t>{0});
    CHECK(q14fPresentFilledDescriptorWrites() == 2);
    const auto filled = rpc.frame(capture);
    CHECK(q14fLitPixels(filled) == 0);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("Q-14f a present that supplies one input refuses a uniform buffer its shader declares past it, through the running renderer",
          "[q14f][render-config-editor][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("q14f_uniform");
    const auto root = dir.root / "project";
    writeQ14eProject(root, Json::array({"q14e_a"}), q14f_reads_one_frag);
    writeFile(root / "shaders/q14f_uniform_second.frag", q14f_uniform_second_frag);
    const auto rendering_path = root / "passes/main_rendering_config.json";

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");
    const auto root_before = readFile(rendering_path);

    const auto refused =
        editPresent(rpc, Json::array({"q14e_a"}), q14fPresentShader("q14f_uniform_second"));
    INFO(refused.dump(2));
    REQUIRE(refused.at("status") == "rejected");
    CHECK(refused.at("committed") == false);
    const auto &error = refused.at("error");
    CHECK(error.at("code") == "render_pipeline_preflight_failed");
    CHECK(error.at("message") ==
          "Fullscreen shader declares a pass input the pass does not supply: pass 'present' "
          "supplies 1 pass input, but fragment 'project://shaders/q14f_uniform_second' declares "
          "a uniform buffer at set 1 binding 1 (shader declares 2 pass inputs)");
    REQUIRE(error.contains("shader_input_mismatch"));
    CHECK(error.at("shader_input_mismatch") ==
          Json{{"pass", "present"},
               {"fragment", "project://shaders/q14f_uniform_second"},
               {"direction", "shader_input_unsupplied"},
               {"kind", "buffer"},
               {"binding", 1},
               {"expected", "none"},
               {"pass_texture_inputs", 1},
               {"pass_buffer_inputs", 0},
               {"shader_pass_inputs",
                Json::array({{{"binding", 0}, {"type", "sampled_image"}, {"name", "in_a"}, {"variadic", false}},
                             {{"binding", 1}, {"type", "uniform_buffer"}, {"name", "q14f_tint"}, {"variadic", false}}})},
               {"variadic_range", nullptr}});
    CHECK(readFile(rendering_path) == root_before);
    GET_MODULE(VulkanManageCore).waitIdle();
}

// EL-1: Studio picks a draw_loop branch's material, tags and variant, and a
// region replacement's provider, from get_render_authoring_context instead of
// typing them. The names listed are the ones the loop judges by: the loop
// rewritten with them through edit_graph_document sends each material down
// the branch its name chose, in the running frame plan.
TEST_CASE("EL-1 get_render_authoring_context lists the materials a draw_loop judges, by the names it takes",
          "[el1][primitives][draw-loop][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("el1_materials");
    const auto root = makeBlankProject(dir.root);
    constexpr auto surface = "engine://surfaces/openpbr/opaque_single.surface";
    const auto material = [&](const char *name, Json tags) {
        return Json{{"name", name},
                    {"surface", surface},
                    {"tags", std::move(tags)},
                    {"base", {{"baseColorFactor", {0.5, 0.5, 0.5, 1.0}}}},
                    {"routing", {{"alpha_mode", "opaque"}, {"double_sided", false}}}};
    };
    auto outlined = material("el1_outlined", Json::array({"el1_hero"}));
    outlined["variants"] = {{"outline", {{"surface", surface}}}};
    writeFile(root / "materials/el1.material.json",
              Json{{"schema", "pelican.material"},
                   {"version", 1},
                   {"materials", Json::array({outlined,
                                              material("el1_tagged", Json::array({"el1_crowd", "el1_hero"})),
                                              material("el1_named", Json::array())})}}
                  .dump(2));
    auto assets = Json::parse(readFile(root / "assets/asset_data.json"));
    assets["materials"] = Json::array({{{"path", "materials/el1.material.json"}}});
    writeFile(root / "assets/asset_data.json", assets.dump(2));
    // The hybrid preset forked into the root, its deferred_geometry written as
    // a draw_loop the way decompose_material_pass writes it.
    const auto rendering_path = root / "passes/main_rendering_config.json";
    auto authored = Json::parse(readFile(rendering_path));
    authored["rendering"] = TestSupport::lp3aForkPreset(
        authored.at("rendering"),
        Json::parse(engineResourceOrThrow("render_pipelines/hybrid_v1.json")).at("config"));
    auto decomposed = nlohmann::ordered_json::parse(authored.dump());
    (void)decomposeMaterialPass(decomposed.at("rendering"), decomposed, "deferred_geometry");
    writeFile(rendering_path, decomposed.dump(2));

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    GET_MODULE(Renderer).prepareRuntimeModules();
    (void)GET_MODULE(ProjectMaterialAssetContainer);
    (void)rpc.result("step_frame");

    const auto context = rpc.result("get_render_authoring_context");
    INFO(context.at("materials").dump(2));
    const auto listed = [&](const char *name) {
        for (const auto &entry : context.at("materials"))
            if (entry.at("name") == name) return entry;
        FAIL("get_render_authoring_context lists no material " << name);
        return Json{};
    };
    const auto outlined_entry = listed("el1_outlined");
    const auto tagged_entry = listed("el1_tagged");
    const auto named_entry = listed("el1_named");
    CHECK(outlined_entry.at("id") == "materials/el1.material.json#el1_outlined");
    CHECK(outlined_entry.at("tags") == Json::array({"el1_hero"}));
    CHECK(outlined_entry.at("variants") == Json::array({"outline"}));
    CHECK(outlined_entry.at("builtin") == false);
    CHECK(tagged_entry.at("variants") == Json::array());
    // The engine's own materials are listed too, marked.
    CHECK(std::any_of(context.at("materials").begin(), context.at("materials").end(),
                      [](const Json &entry) { return entry.at("builtin") == true; }));
    // Every tag once, sorted.
    const auto &tags = context.at("material_tags");
    CHECK(std::find(tags.begin(), tags.end(), "el1_crowd") != tags.end());
    CHECK(std::find(tags.begin(), tags.end(), "el1_hero") != tags.end());
    CHECK(std::is_sorted(tags.begin(), tags.end()));
    CHECK(std::adjacent_find(tags.begin(), tags.end()) == tags.end());
    // The engine's identity provider is the one any project can name.
    CHECK(std::any_of(context.at("region_providers").begin(), context.at("region_providers").end(),
                      [](const Json &provider) {
                          return provider.at("name") == "builtin.identity_v1" && provider.at("builtin") == true;
                      }));

    // A loop whose branches are chosen by those names, and nothing typed.
    Json parameters;
    const auto written = Json::parse(readFile(rendering_path));
    for (const auto &node : written.at("nodes"))
        if (node.at("id") == "deferred_geometry_draws") parameters = node.at("parameters");
    REQUIRE(parameters.is_object());
    const auto body = parameters.at("branches").back().at("body");
    parameters["branches"] = Json::array({
        {{"when", {{"has_variant", outlined_entry.at("variants").at(0)}}}, {"body", body}},
        {{"when", {{"tags", {{"include", Json::array({tagged_entry.at("tags").at(0)})}}}}}, {"body", body}},
        {{"when", {{"material", named_entry.at("id")}}}, {"body", body}},
        {{"body", body}},
    });
    const auto accepted = rpc.result(
        "edit_graph_document",
        {{"base_source_digest", context.at("source_digest").at("hex")},
         {"ops", Json::array({{{"op", "set_node_parameters"},
                               {"scope", "root"},
                               {"node", "deferred_geometry_draws"},
                               {"parameters", parameters}}})}});
    INFO(accepted.dump(2));
    REQUIRE(accepted.at("status") == "accepted");
    Json committed;
    for (int attempt = 0; attempt < 8 && committed.is_null(); ++attempt) {
        (void)rpc.result("step_frame");
        auto result = rpc.result("get_edit_result", {{"ticket", accepted.at("ticket")}});
        if (result.value("status", std::string{}) != "pending") committed = std::move(result);
    }
    INFO(committed.dump(2));
    REQUIRE(committed.at("status") == "committed");
    CHECK(committed.value("restart_required", false) == false);

    // The running frame plan sends each material down the branch its name chose.
    (void)rpc.result("render_frame");
    const auto plan = rpc.result("get_frame_plan");
    Json loop;
    for (const auto &entry : plan.at("loops"))
        if (entry.at("node") == "deferred_geometry_draws") loop = entry;
    INFO(loop.dump(2));
    REQUIRE(loop.is_object());
    const auto branch_of = [&](const Json &material_entry) {
        for (const auto &element : loop.at("elements"))
            if (element.at("element") == material_entry.at("id")) return element.at("branch").get<int>();
        FAIL("the draw_loop has no element " << material_entry.at("id"));
        return -1;
    };
    CHECK(branch_of(outlined_entry) == 0);
    CHECK(branch_of(tagged_entry) == 1);
    CHECK(branch_of(named_entry) == 2);
    GET_MODULE(VulkanManageCore).waitIdle();
}

} // namespace Pelican
