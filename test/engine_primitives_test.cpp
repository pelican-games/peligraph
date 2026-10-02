#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/asset/model.hpp"
#include "../src/core/communication/engineauthoringlists.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/core/loader/ktx2.hpp"
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
#include "../src/project/audioanalysisformat.hpp"
#include "../src/project/assetsmanifest.hpp"
#include "../src/project/materialpassdecompose.hpp"
#include "../src/project/projecttemplate.hpp"
#include "../src/project/renderconfigsection.hpp"
#include "lp3a_decompose_support.hpp"
#include "render_graph_root_test_support.hpp"
#include "renderer_testaccess.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <stb_image.h>
// IA-1: the test writes a PNG asset.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <string>
#include <thread>
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
TEST_CASE("PO-1 the frame plan names a binding the shader never reads and an output nobody reads, through the running renderer",
          "[po1][frame-plan][diagnostics][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("po1_diagnostics");
    const auto root = dir.root / "project";
    // present supplies q14e_a and q14e_b; its shader declares both and reads
    // only q14e_b. draw_q14e_c writes q14e_c, which nothing reads.
    writeQ14eProject(root, Json::array({"q14e_a", "q14e_b"}), q14f_reads_second_frag);

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");
    (void)rpc.result("render_frame");
    const auto plan = rpc.result("get_frame_plan");
    auto found = Json::array();
    for (const auto &entry : plan.at("diagnostics")) {
        if (entry.at("kind") == "unused_binding" || entry.at("kind") == "unread_output")
            found.push_back(entry);
    }
    INFO(plan.at("diagnostics").dump(2));
    // Nothing else: draw_q14e_a / draw_q14e_b are read, present writes
    // display, and q14e_b's binding is read.
    CHECK(found ==
          Json::array({{{"kind", "unread_output"},
                        {"subject", "draw_q14e_c"},
                        {"detail", {{"resource", "q14e_c"}}}},
                       {{"kind", "unused_binding"},
                        {"subject", "present"},
                        {"detail", {{"binding", 0}, {"resource", "q14e_a"}}}}}));
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("PO-1 get_status's GPU time starts a new window when an edit publishes a new generation",
          "[po1][gpu-timing][status][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("po1_gpu_timing");
    const auto root = dir.root / "project";
    writeQ14eProject(root, Json::array({"q14e_a"}), q14f_reads_one_frag);
    const auto rendering_path = root / "passes/main_rendering_config.json";
    {
        auto authored = Json::parse(readFile(rendering_path));
        renderingSectionOf(authored)["features"] =
            Json::array({"engine://features/gpu_timing.json"});
        writeFile(rendering_path, authored.dump(2));
    }

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    for (int frame = 0; frame < 6; ++frame) (void)rpc.result("step_frame");
    const auto before = rpc.result("get_status").at("gpu_timing");
    INFO(before.dump(2));
    REQUIRE(before.at("enabled") == true);
    REQUIRE(before.at("supported") == true);
    REQUIRE(before.at("generation").is_number_unsigned());
    REQUIRE(before.at("samples").get<std::size_t>() >= 2);
    CHECK(before.at("samples") == before.at("history_count"));
    std::uint64_t last_before = 0;
    for (const auto &frame : before.at("logical_frame_history"))
        last_before = std::max(last_before, frame.at("logical_frame").get<std::uint64_t>());

    const auto committed = editPresentInputs(rpc, Json::array({"q14e_b"}));
    INFO(committed.dump(2));
    REQUIRE(committed.at("committed") == true);
    for (int frame = 0; frame < 5; ++frame) (void)rpc.result("step_frame");
    const auto generation = rpc.result("get_frame_plan").at("runtime_generation");
    REQUIRE(generation != before.at("generation"));

    const auto after = rpc.result("get_status").at("gpu_timing");
    INFO(after.dump(2));
    CHECK(after.at("generation") == generation);
    const auto samples = after.at("samples").get<std::size_t>();
    CHECK(samples >= 1);
    CHECK(after.at("history_count") == samples);
    REQUIRE(after.at("logical_frame_history").size() == samples);
    // The window holds none of the frames measured before the edit.
    for (const auto &frame : after.at("logical_frame_history"))
        CHECK(frame.at("logical_frame").get<std::uint64_t>() > last_before);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("ST-1 get_status carries the last frame's GPU samples and get_gpu_timing's node averages",
          "[st1][gpu-timing][status][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("st1_gpu_timing");
    const auto root = dir.root / "project";
    writeQ14eProject(root, Json::array({"q14e_a"}), q14f_reads_one_frag);
    const auto rendering_path = root / "passes/main_rendering_config.json";
    {
        auto authored = Json::parse(readFile(rendering_path));
        renderingSectionOf(authored)["features"] =
            Json::array({"engine://features/gpu_timing.json"});
        writeFile(rendering_path, authored.dump(2));
    }

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    // More frames than the 120-frame history holds, so the window is full.
    for (int frame = 0; frame < 130; ++frame) (void)rpc.result("step_frame");
    const auto status = rpc.result("get_status");
    const auto timing = status.at("gpu_timing");
    const auto averages = rpc.result("get_gpu_timing");
    WARN("ST-1 size: get_status " << status.dump().size() << " bytes, gpu_timing "
                                  << timing.dump().size() << " bytes, nodes "
                                  << timing.at("nodes").size() << " rows, history "
                                  << timing.at("history_count") << " frames");
    INFO(timing.dump(2));
    REQUIRE(timing.at("enabled") == true);
    REQUIRE(timing.at("supported") == true);
    REQUIRE(timing.at("history_count").get<std::size_t>() >= 2);
    const auto last_frame = timing.at("logical_frame_history").back().at("logical_frame");

    // nodes: the last frame's samples only (barriers and body per node).
    const auto &rows = averages.at("nodes");
    REQUIRE_FALSE(rows.empty());
    REQUIRE(timing.at("nodes").size() == rows.size() * 2);
    for (const auto &sample : timing.at("nodes")) {
        CHECK(sample.at("logical_frame") == last_frame);
        CHECK(sample.contains("subrange"));
    }

    // node_averages: the rows get_gpu_timing answers, value for value.
    REQUIRE(timing.contains("node_averages"));
    CHECK(timing.at("node_averages") == rows);
    for (const auto &row : timing.at("node_averages"))
        CHECK(row.at("sample_count") == averages.at("frame_count"));
    GET_MODULE(VulkanManageCore).waitIdle();
}

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


// EL-4: the formats get_graph_catalog lists for a material pass's sockets are
// the ones the running engine takes: an image of each listed combination is
// committed (RS-1's restart for the live materials), a format off the list
// is refused as today, and every listed format has the aspect its socket
// needs. An engine shader get_engine_shaders lists (EL-4b) runs in a graph's
// fullscreen pass. EL-4c: the engine reflects them on a worker thread it
// starts with RPC; while it builds, get_engine_shaders answers building and
// the other requests answer.
TEST_CASE("EL-4 the material pass formats and engine shaders get_graph_catalog lists are the ones the engine takes",
          "[el4][primitives][material-routing][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("el4_lists");
    const Json color_type{{"semantic", "pelican.render.color_signal@1"},
                          {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}};
    const Json depth_type{{"semantic", "pelican.render.depth@1"},
                          {"arguments", {{"representation", "device"}, {"space", "projection"}}}};
    const auto image_parameters = [&](const std::string &socket, const std::string &format) {
        const bool depth = socket == "depth";
        return Json{{"type", depth ? depth_type : color_type},
                    {"format", format},
                    {"extent_scale", 1.0},
                    {"usage", Json::array({depth ? "DEPTH_STENCIL_ATTACHMENT" : "COLOR_ATTACHMENT", "SAMPLED"})}};
    };
    const auto image_id = [](const std::string &socket) { return "el4_pass__" + socket; };
    // The pass and one image per socket, the formats as given.
    const auto material_pass_ops = [&](const std::string &contract, const Json &formats) {
        Json ops = Json::array();
        Json inputs = Json::object();
        for (const auto &[socket, format] : formats.items()) {
            ops.push_back({{"op", "add_node"},
                           {"scope", "root"},
                           {"node", {{"id", image_id(socket)},
                                     {"op", "render.image"},
                                     {"parameters", image_parameters(socket, format.get<std::string>())},
                                     {"outputs", Json::array({"resource"})}}}});
            inputs[socket] = {{"node", image_id(socket)}, {"port", "resource"}};
        }
        ops.push_back({{"op", "add_node"},
                       {"scope", "root"},
                       {"node", {{"id", "el4_pass"},
                                 {"op", "render.material_pass"},
                                 {"parameters", {{"material_contract", contract}, {"pass_set", "main_render"}}},
                                 {"inputs", inputs},
                                 {"outputs", Json::array()}}}});
        return ops;
    };
    // The final answer, refused when asked for or when the renderer judges it.
    const auto edit = [](Rpc &rpc, const Json &ops, std::optional<std::string> route_default = std::nullopt) {
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
    const auto contract_of = [](const Json &catalog, const char *name) {
        for (const auto &entry : catalog.at("material_pass_contracts"))
            if (entry.at("contract") == name) return entry;
        FAIL("get_graph_catalog lists no contract " << name);
        return Json{};
    };
    // A format of the aspect that `listed` does not hold.
    const auto off_list = [](const Json &aspects, const Json &listed, const std::string &aspect) {
        for (const auto &[name, format_aspect] : aspects.items())
            if (format_aspect == aspect && std::find(listed.begin(), listed.end(), name) == listed.end()) return name;
        FAIL("every " << aspect << " format is listed");
        return std::string{};
    };
    const auto refused_for = [](const Json &result, const char *message) {
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "rejected");
        CHECK_THAT(result.at("error").dump(), Catch::Matchers::ContainsSubstring(message));
    };
    const auto check_aspects = [](const Json &contract, const Json &aspects) {
        for (const auto &socket : contract.at("sockets")) {
            INFO(socket.dump());
            CHECK(socket.at("aspect") == (socket.at("socket") == "depth" ? "depth" : "color"));
            for (const auto &format : socket.at("formats"))
                CHECK(aspects.at(format.get<std::string>()) == socket.at("aspect"));
        }
    };

    // --- The G-buffer contract on a blank project.
    {
        const auto root = dir.root / "deferred";
        initializeProjectTemplate(root, ProjectTemplate::Blank);
        FastModuleContainer modules;
        configureHeadless(root);
        Rpc rpc;
        (void)rpc.result("load_scene", {{"name", "default_scene"}});
        GET_MODULE(Renderer).prepareRuntimeModules();
        (void)rpc.result("step_frame");
        const auto catalog = rpc.result("get_graph_catalog");
        const auto aspects =
            rpc.result("get_render_authoring_context").at("render_target_vocabulary").at("format_aspects");
        const auto deferred = contract_of(catalog, "deferred_geometry_v1");
        INFO(deferred.dump(2));
        check_aspects(deferred, aspects);
        const auto &combinations = deferred.at("combinations");
        REQUIRE(combinations.size() == 2);
        const auto rendering_path = root / "passes/main_rendering_config.json";
        const auto root_before = readFile(rendering_path);

        // Off the list: a depth format the depth socket does not take, then a
        // color format the normal socket does not take.
        auto formats = combinations.at(0);
        formats["depth"] = off_list(aspects, Json::array({formats.at("depth")}), "depth");
        refused_for(edit(rpc, material_pass_ops("deferred_geometry_v1", formats)), "depth output format mismatch");
        formats = combinations.at(0);
        Json normal_formats;
        for (const auto &socket : deferred.at("sockets"))
            if (socket.at("socket") == "normal") normal_formats = socket.at("formats");
        formats["normal"] = off_list(aspects, normal_formats, "color");
        refused_for(edit(rpc, material_pass_ops("deferred_geometry_v1", formats)), "color output format mismatch");
        CHECK(readFile(rendering_path) == root_before);

        // Each listed combination is committed; the second rewrites the
        // images the first added.
        const auto first = edit(rpc, material_pass_ops("deferred_geometry_v1", combinations.at(0)));
        INFO(first.dump(2));
        REQUIRE(first.at("status") == "committed");
        Json rewrite = Json::array();
        for (const auto &[socket, format] : combinations.at(1).items())
            rewrite.push_back({{"op", "set_node_parameters"},
                               {"scope", "root"},
                               {"node", image_id(socket)},
                               {"parameters", image_parameters(socket, format.get<std::string>())}});
        const auto second = edit(rpc, rewrite);
        INFO(second.dump(2));
        REQUIRE(second.at("status") == "committed");
        GET_MODULE(VulkanManageCore).waitIdle();
    }

    // --- A fullscreen pass of listed engine shaders, then the forward contract.
    const auto root = dir.root / "forward";
    initializeProjectTemplate(root, ProjectTemplate::Blank);
    FastModuleContainer modules;
    configureHeadless(root);
    // The worker the endpoint starts waits before its first shader until
    // released, so the requests below meet it building.
    resetEngineShadersReflectionForTesting();
    holdEngineShadersReflectionForTesting(true);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    GET_MODULE(Renderer).prepareRuntimeModules();
    (void)rpc.result("step_frame");
    const auto catalog = rpc.result("get_graph_catalog");
    const auto aspects =
        rpc.result("get_render_authoring_context").at("render_target_vocabulary").at("format_aspects");
    // EL-4b: the catalog reflects no shader; get_engine_shaders lists them.
    CHECK_FALSE(catalog.contains("engine_shaders"));
    const Json building{{"status", "building"}};
    CHECK(rpc.result("get_engine_shaders") == building);
    CHECK(rpc.result("get_status").contains("instance_id"));
    CHECK(rpc.result("get_engine_shaders") == building);
    holdEngineShadersReflectionForTesting(false);
    auto answered = rpc.result("get_engine_shaders");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes{2};
    while (answered == building && std::chrono::steady_clock::now() < deadline) {
        (void)rpc.result("step_frame");
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        answered = rpc.result("get_engine_shaders");
    }
    REQUIRE(answered.value("status", "") == "ready");
    const auto engine_shaders = answered.at("engine_shaders");
    const auto shader_of = [&](const char *reference, const char *stage) {
        for (const auto &entry : engine_shaders)
            if (entry.at("reference") == reference && entry.at("stage") == stage) return entry;
        FAIL("get_engine_shaders lists no " << stage << " shader " << reference);
        return Json{};
    };
    const auto vertex = shader_of("engine://fullscreen", "vertex");
    const auto fragment = shader_of("engine://passthrough", "fragment");
    INFO(vertex.dump(2) << "\n" << fragment.dump(2));
    REQUIRE(fragment.contains("reflected_from"));
    CHECK(fragment.at("fragment_outputs").at(0).at("location") == 0);
    // EL-5: get_shader_interface answers an engine stem with these entries.
    const auto shader_interface = rpc.result("get_shader_interface", {{"reference", "engine://passthrough"}});
    REQUIRE(shader_interface.value("status", "") == "ready");
    CHECK(std::find(shader_interface.at("stages").begin(), shader_interface.at("stages").end(), fragment) !=
          shader_interface.at("stages").end());
    const auto fullscreen = edit(rpc, Json::array({
        {{"op", "add_node"},
         {"scope", "root"},
         {"node", {{"id", "el4_canvas"},
                   {"op", "render.image"},
                   {"parameters", image_parameters("color", "R16G16B16A16_SFLOAT")},
                   {"outputs", Json::array({"resource"})}}}},
        {{"op", "add_node"},
         {"scope", "root"},
         {"node", {{"id", "el4_fill"},
                   {"op", "render.fullscreen"},
                   {"parameters",
                    {{"pass_set", "main_render"},
                     {"shader", {{"vertex", vertex.at("reference")}, {"fragment", fragment.at("reference")}}},
                     {"ports", {{"color", {{"kind", "image"}, {"type", color_type}, {"access", "write"},
                                           {"attachment", "color"}, {"location", 0}}}}}}},
                   {"inputs", {{"color", {{"node", "el4_canvas"}, {"port", "resource"}}}}},
                   {"outputs", Json::array({"color"})}}}},
    }));
    INFO(fullscreen.dump(2));
    REQUIRE(fullscreen.at("status") == "committed");
    CHECK(fullscreen.value("restart_required", false) == false);
    (void)rpc.result("render_frame");
    CHECK(rpc.result("get_frame_plan").dump().find("el4_fill") != std::string::npos);

    const auto forward = contract_of(catalog, "forward_opaque_v1");
    INFO(forward.dump(2));
    check_aspects(forward, aspects);
    REQUIRE(forward.at("combinations").size() == 1);
    auto formats = forward.at("combinations").at(0);
    REQUIRE(formats.size() == 2);
    formats["color"] = off_list(aspects, Json::array({formats.at("color")}), "color");
    refused_for(edit(rpc, material_pass_ops("forward_opaque_v1", formats), "forward"),
                "color output format mismatch");
    const auto committed =
        edit(rpc, material_pass_ops("forward_opaque_v1", forward.at("combinations").at(0)), "forward");
    INFO(committed.dump(2));
    REQUIRE(committed.at("status") == "committed");
    GET_MODULE(VulkanManageCore).waitIdle();
}

// EL-2: the planner's decisions the editor writes into the rendering root
// reach the plan the running renderer compiles. The blank project's
// main_render is a chain el2_a -> el2_b -> el2_c -> present through three
// targets of one shape; el2_a's and el2_c's targets never live at once, so the
// planner aliases them until el2_a is isolated or its target says no_alias.
// (A rendering scope is fused only for a tile backend candidate, which a
// desktop device does not select, so the scopes cannot show isolate here.)
TEST_CASE("EL-2 target planning, plan pins and physical fragments written over RPC reach the frame plan",
          "[el2][primitives][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("el2_planning");
    const auto root = dir.root / "project";
    initializeProjectTemplate(root, ProjectTemplate::Blank);
    const auto rendering_path = root / "passes/main_rendering_config.json";
    {
        auto authored = Json::parse(readFile(rendering_path));
        auto &section = renderingSectionOf(authored);
        const auto target = [](const char *name) {
            return Json{{"name", name},
                        {"extent_scale", 1.0},
                        {"format", "R8G8B8A8_UNORM"},
                        {"format_class", "data"},
                        {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}};
        };
        section["render_targets"] =
            Json::array({target("el2_color_a"), target("el2_color_b"), target("el2_color_c")});
        const Json shader{{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}};
        const auto draw = [&](const char *name, Json input, const char *color) {
            return Json{{"name", name},
                        {"type", "fullscreen"},
                        {"input", std::move(input)},
                        {"output", {{"color", Json::array({color})}, {"depth", nullptr}}},
                        {"shader", shader}};
        };
        auto &passes = section.at("rendering_passes").at(0).at("passes");
        passes.at(0)["input"] = Json::array({"el2_color_c"});
        passes.insert(passes.begin(), draw("el2_c", Json::array({"el2_color_b"}), "el2_color_c"));
        passes.insert(passes.begin(), draw("el2_b", Json::array({"el2_color_a"}), "el2_color_b"));
        passes.insert(passes.begin(), draw("el2_a", Json::array(), "el2_color_a"));
        writeFile(rendering_path, authored.dump(2));
    }

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    GET_MODULE(Renderer).prepareRuntimeModules();
    (void)rpc.result("step_frame");
    const auto send = [&](std::string_view method, Json params) {
        params["base_source_digest"] =
            rpc.result("get_render_authoring_context").at("source_digest").at("hex");
        return rpc.result(method, std::move(params));
    };
    const auto settle = [&](const Json &accepted) {
        INFO(accepted.dump(2));
        REQUIRE(accepted.at("status") == "accepted");
        for (int attempt = 0; attempt < 8; ++attempt) {
            (void)rpc.result("step_frame");
            auto result = rpc.result("get_edit_result", {{"ticket", accepted.at("ticket")}});
            if (result.value("status", std::string{}) != "pending") return result;
        }
        FAIL("the planning ticket stayed pending");
        return Json{};
    };
    const auto committed = [&](std::string_view method, Json params) {
        auto result = settle(send(method, std::move(params)));
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "committed");
        return result;
    };
    const auto physical = [&] {
        (void)rpc.result("render_frame");
        const auto plan = rpc.result("get_frame_plan");
        REQUIRE(plan.contains("physical_target_plan"));
        return plan.at("physical_target_plan");
    };
    const auto aliased = [](const Json &plan) {
        for (const auto &group : plan.at("alias_groups")) {
            const auto &members = group.at("resources");
            if (std::find(members.begin(), members.end(), "el2_color_a") != members.end() &&
                std::find(members.begin(), members.end(), "el2_color_c") != members.end())
                return true;
        }
        return false;
    };
    const auto documents = rpc.result("get_render_documents").at("documents");
    REQUIRE(documents.size() == 1);
    const auto root_before = readFile(rendering_path);

    // 1. isolate keeps el2_a's target out of every alias group, as no_alias
    // on the target does; null undoes each.
    const auto automatic = physical();
    INFO(automatic.at("alias_groups").dump(2));
    INFO(automatic.at("resources").dump(2));
    REQUIRE(aliased(automatic));
    const auto isolated = committed(
        "edit_target_planning", {{"graph", "main_render"}, {"nodes", {{"el2_a", {{"isolate", true}}}}}});
    CHECK(isolated.at("operation") == "edit_target_planning");
    CHECK_FALSE(aliased(physical()));
    committed("edit_target_planning", {{"graph", "main_render"},
                                       {"nodes", {{"el2_a", nullptr}}},
                                       {"resources", {{"el2_color_a", {{"no_alias", true}}}}}});
    CHECK_FALSE(aliased(physical()));
    const auto unknown = send("edit_target_planning",
                              {{"graph", "main_render"}, {"nodes", {{"el2_nowhere", {{"isolate", true}}}}}});
    INFO(unknown.dump(2));
    REQUIRE(unknown.at("status") == "rejected");
    CHECK(unknown.at("error").at("code") == "target_planning_name_unknown");
    CHECK(unknown.at("error").at("kind") == "node");
    CHECK(unknown.at("error").at("name") == "el2_nowhere");
    committed("edit_target_planning", {{"graph", "main_render"}, {"resources", {{"el2_color_a", nullptr}}}});
    CHECK(aliased(physical()));

    // 2. The plan's own pin package and physical fragment, written back.
    const auto pin = physical().at("ejectable_pin_package");
    committed("edit_vulkan_plan_pins", {{"family", "flat"}, {"pin", pin}});
    {
        const auto plan = physical();
        CHECK(plan.at("applied_pin_package") == pin);
        CHECK(plan.at("backend_selection").at("selected_candidate") == pin.at("pins").at("backend_candidate"));
    }
    const auto fragment = physical().at("ejectable_physical_fragment");
    committed("put_vulkan_physical_fragment", {{"family", "flat"}, {"fragment", fragment}});
    CHECK(physical().at("applied_physical_fragment") == fragment);
    const auto applied_root = readFile(rendering_path);

    // A fragment whose base plan is not the current one: the linker says so.
    auto broken = fragment;
    auto fingerprint = broken.at("automatic_plan_fingerprint").get<std::string>();
    fingerprint.back() = fingerprint.back() == '0' ? '1' : '0';
    broken["automatic_plan_fingerprint"] = fingerprint;
    const auto stale = settle(send("put_vulkan_physical_fragment", {{"family", "flat"}, {"fragment", broken}}));
    INFO(stale.dump(2));
    REQUIRE(stale.at("status") == "rejected");
    const auto &error = stale.at("error");
    CHECK(error.at("code") == "physical_fragment_stale");
    CHECK(error.at("family") == "flat");
    CHECK(error.at("graph") == "main_render");
    CHECK(error.at("fingerprint") == "automatic_plan");
    CHECK(error.at("expected") == fragment.at("automatic_plan_fingerprint"));
    CHECK(error.at("actual") == fingerprint);
    CHECK(readFile(rendering_path) == applied_root);
    CHECK(physical().at("applied_physical_fragment") == fragment);

    // 3. RD-1 takes the root back, and the plan with it.
    const auto current = rpc.result("get_render_documents").at("documents").at(0);
    const auto restored = settle(rpc.result(
        "restore_render_documents",
        {{"documents", Json::array({{{"reference", documents.at(0).at("reference")},
                                     {"base_digest", current.at("digest")},
                                     {"content", documents.at(0).at("content")}}})}}));
    INFO(restored.dump(2));
    REQUIRE(restored.at("status") == "committed");
    CHECK(readFile(rendering_path) == root_before);
    {
        const auto plan = physical();
        CHECK_FALSE(plan.contains("applied_pin_package"));
        CHECK_FALSE(plan.contains("applied_physical_fragment"));
        CHECK(aliased(plan));
    }
    GET_MODULE(VulkanManageCore).waitIdle();
}

// RL-1: a rendering root rewritten on disk outside the editor is taken in by
// reload_render_documents (Studio's engine runs with the file watcher's gate
// closed), and the edits after it are not external_modification.
TEST_CASE("RL-1 reload_render_documents takes in a root rewritten on disk, and refuses what cannot compose",
          "[rl1][primitives][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("rl1_reload");
    const auto root = dir.root / "project";
    initializeProjectTemplate(root, ProjectTemplate::Blank);
    const auto rendering_path = root / "passes/main_rendering_config.json";
    {
        // EL-2's frame: three chained fullscreen passes before the template's
        // own, so el2_color_a and el2_color_c alias unless planning says not.
        auto authored = Json::parse(readFile(rendering_path));
        auto &section = renderingSectionOf(authored);
        const auto target = [](const char *name) {
            return Json{{"name", name},
                        {"extent_scale", 1.0},
                        {"format", "R8G8B8A8_UNORM"},
                        {"format_class", "data"},
                        {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}};
        };
        section["render_targets"] =
            Json::array({target("el2_color_a"), target("el2_color_b"), target("el2_color_c")});
        const Json shader{{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}};
        const auto draw = [&](const char *name, Json input, const char *color) {
            return Json{{"name", name},
                        {"type", "fullscreen"},
                        {"input", std::move(input)},
                        {"output", {{"color", Json::array({color})}, {"depth", nullptr}}},
                        {"shader", shader}};
        };
        auto &passes = section.at("rendering_passes").at(0).at("passes");
        passes.at(0)["input"] = Json::array({"el2_color_c"});
        passes.insert(passes.begin(), draw("el2_c", Json::array({"el2_color_b"}), "el2_color_c"));
        passes.insert(passes.begin(), draw("el2_b", Json::array({"el2_color_a"}), "el2_color_b"));
        passes.insert(passes.begin(), draw("el2_a", Json::array(), "el2_color_a"));
        writeFile(rendering_path, authored.dump(2));
    }

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    GET_MODULE(Renderer).prepareRuntimeModules();
    (void)rpc.result("step_frame");
    const auto digest = [&] {
        return rpc.result("get_render_authoring_context").at("source_digest").at("hex").get<std::string>();
    };
    const auto settle = [&](const Json &accepted) {
        INFO(accepted.dump(2));
        if (accepted.value("status", std::string{}) == "rejected") return accepted;
        REQUIRE(accepted.at("status") == "accepted");
        for (int attempt = 0; attempt < 8; ++attempt) {
            (void)rpc.result("step_frame");
            auto result = rpc.result("get_edit_result", {{"ticket", accepted.at("ticket")}});
            if (result.value("status", std::string{}) != "pending") return result;
        }
        FAIL("the ticket stayed pending");
        return Json{};
    };
    const auto reload = [&] { return settle(rpc.result("reload_render_documents")); };
    const auto physical = [&] {
        (void)rpc.result("render_frame");
        const auto plan = rpc.result("get_frame_plan");
        REQUIRE(plan.contains("physical_target_plan"));
        return plan.at("physical_target_plan");
    };
    const auto aliased = [](const Json &plan) {
        for (const auto &group : plan.at("alias_groups")) {
            const auto &members = group.at("resources");
            if (std::find(members.begin(), members.end(), "el2_color_a") != members.end() &&
                std::find(members.begin(), members.end(), "el2_color_c") != members.end())
                return true;
        }
        return false;
    };
    const auto reference =
        rpc.result("get_render_documents").at("documents").at(0).at("reference").get<std::string>();
    const auto original = readFile(rendering_path);
    const auto original_digest = digest();
    REQUIRE(aliased(physical()));

    // 2. Nothing changed: no_change, and every document is listed.
    {
        const auto result = reload();
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "committed");
        CHECK(result.at("no_change") == true);
        CHECK(result.at("operation") == "reload_render_documents");
        CHECK(result.at("changed") == Json::array());
        CHECK(result.at("documents").at(reference) == original_digest);
        CHECK(result.at("source_digest").at("hex") == original_digest);
    }

    // 1. The root rewritten outside the editor: isolate on el2_a.
    auto isolated = Json::parse(original);
    renderingSectionOf(isolated)["target_planning"] = {
        {"graphs", {{"main_render", {{"nodes", {{"el2_a", {{"isolate", true}}}}}}}}}};
    const auto isolated_bytes = isolated.dump(2);
    writeFile(rendering_path, isolated_bytes);
    std::string isolated_digest;
    {
        // Before the reload, an edit on the old digest is the external change.
        const auto refused = rpc.result(
            "edit_target_planning", {{"base_source_digest", original_digest},
                                     {"graph", "main_render"},
                                     {"resources", {{"el2_color_a", {{"no_alias", true}}}}}});
        CHECK(refused.at("error").at("code") == "external_modification");

        const auto result = reload();
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "committed");
        CHECK_FALSE(result.contains("no_change"));
        CHECK_FALSE(result.contains("restart_required"));
        CHECK(result.at("changed") == Json::array({reference}));
        isolated_digest = result.at("source_digest").at("hex").get<std::string>();
        CHECK(isolated_digest != original_digest);
        CHECK(result.at("documents").at(reference) == isolated_digest);
        CHECK(digest() == isolated_digest);
        CHECK(readFile(rendering_path) == isolated_bytes); // nothing was written
        CHECK_FALSE(aliased(physical()));
    }
    {
        // The GUI's next edit goes through on the reloaded digest.
        const auto result = settle(rpc.result(
            "edit_target_planning", {{"base_source_digest", isolated_digest},
                                     {"graph", "main_render"},
                                     {"nodes", {{"el2_a", nullptr}}}}));
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "committed");
        CHECK(aliased(physical()));
    }

    // 4. A ticket not yet committed: reload_busy, and the ticket still lands.
    {
        const auto pending = rpc.result(
            "edit_target_planning", {{"base_source_digest", digest()},
                                     {"graph", "main_render"},
                                     {"nodes", {{"el2_a", {{"isolate", true}}}}}});
        REQUIRE(pending.at("status") == "accepted");
        const auto busy = rpc.result("reload_render_documents");
        INFO(busy.dump(2));
        REQUIRE(busy.at("status") == "rejected");
        CHECK(busy.at("error").at("code") == "reload_busy");
        CHECK(busy.at("error").at("reason") == "ticket_pending");
        CHECK(busy.at("error").at("tickets") == Json::array({pending.at("ticket")}));
        CHECK(settle(pending).at("status") == "committed");
        CHECK_FALSE(aliased(physical()));
    }

    // 3. What cannot compose is refused by name; the live frame and the
    // editor's document stay, the disk keeps the bytes, and once the file is
    // put back an edit on the previous digest goes through.
    const auto before_refusals = digest();
    const auto before_bytes = readFile(rendering_path);
    const auto refused = [&](const std::string &bytes) {
        writeFile(rendering_path, bytes);
        const auto result = reload();
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "rejected");
        CHECK_FALSE(result.at("error").value("code", std::string{}).empty());
        CHECK(digest() == before_refusals);
        CHECK(readFile(rendering_path) == bytes);
        CHECK_FALSE(aliased(physical()));
        return result.at("error");
    };
    {
        const auto error = refused("{ \"schema\": \"pelican.graph\", ");
        CHECK(error.at("code") == "render_pipeline_preflight_failed");
        CHECK(error.at("method") == "reload_render_documents");
    }
    {
        auto missing = Json::parse(before_bytes);
        renderingSectionOf(missing)
            .at("rendering_passes").at(0).at("passes").at(0).at("shader")["fragment"] =
            "project://shaders/rl1_nowhere";
        // Composed, then refused by the runtime when it builds the pass.
        CHECK(refused(missing.dump(2)).at("code") == "render_pipeline_preflight_failed");
    }
    writeFile(rendering_path, before_bytes);
    {
        const auto result = settle(rpc.result(
            "edit_target_planning", {{"base_source_digest", before_refusals},
                                     {"graph", "main_render"},
                                     {"nodes", {{"el2_a", nullptr}}}}));
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "committed");
        CHECK(aliased(physical()));
    }
    GET_MODULE(VulkanManageCore).waitIdle();
}

namespace {
// PS-1: h0 (once) writes (level, 0.25, 0.5) into the persistent h0_image,
// twiddle (once) doubles its green into the persistent twiddle_image, and
// show (every frame) copies that into show_image, which the test reads back.
constexpr std::string_view ps1_h0_frag =
    "#version 460\n#extension GL_GOOGLE_include_directive : require\n"
    "#include \"pelican_sets.glsl\"\n#include \"pelican_frame.glsl\"\n"
    "#include \"pelican_resource_ports.glsl\"\n\n"
    "layout(location = 0) in vec2 inUV;\nlayout(location = 0) out vec4 outColor;\n\n"
    "void main() {\n"
    "    outColor = vec4(pelican_load_level(0u), 0.25, 0.5, 1.0);\n"
    "}\n";

constexpr std::string_view ps1_twiddle_frag =
    "#version 460\n#extension GL_GOOGLE_include_directive : require\n"
    "#include \"pelican_sets.glsl\"\n#include \"pelican_view.glsl\"\n\n"
    "layout(location = 0) in vec2 inUV;\nlayout(location = 0) out vec4 outColor;\n"
    "PELICAN_DECLARE_INPUT_0(h0);\n\n"
    "void main() {\n"
    "    vec4 value = PELICAN_TEXTURE_2D_0(h0, inUV);\n"
    "    outColor = vec4(value.r, value.g * 2.0, value.b, 1.0);\n"
    "}\n";

constexpr std::string_view ps1_show_frag =
    "#version 460\n#extension GL_GOOGLE_include_directive : require\n"
    "#include \"pelican_sets.glsl\"\n#include \"pelican_view.glsl\"\n\n"
    "layout(location = 0) in vec2 inUV;\nlayout(location = 0) out vec4 outColor;\n"
    "PELICAN_DECLARE_INPUT_0(field);\n\n"
    "void main() {\n"
    "    outColor = vec4(PELICAN_TEXTURE_2D_0(field, inUV).rgb, 1.0);\n"
    "}\n";

Json ps1RenderingRoot(const Json &blank_root, double level) {
    const Json type{{"semantic", "pelican.render.color_signal@1"},
                    {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}};
    const Json color{{"kind", "image"}, {"type", type}, {"access", "write"}, {"attachment", "color"},
                     {"location", 0}};
    const Json input{{"kind", "image"}, {"type", type}, {"access", "read"}, {"binding", "input"},
                     {"input_index", 0}};
    const auto edge = [](const char *node, const char *port) { return Json{{"node", node}, {"port", port}}; };
    const auto image = [&](const char *id, bool persistent, bool read_back) {
        Json usage = Json::array({"COLOR_ATTACHMENT", "SAMPLED"});
        if (read_back) usage.push_back("TRANSFER_SRC");
        Json parameters{{"type", type}, {"format", "R8G8B8A8_UNORM"}, {"extent_scale", 1.0}, {"usage", usage}};
        if (persistent) parameters["lifetime"] = "persistent";
        return Json{{"id", id}, {"op", "render.image"}, {"parameters", parameters}, {"outputs", {"resource"}}};
    };
    const auto pass = [&](const char *id, const char *fragment, Json ports, Json inputs, bool once) {
        Json parameters{{"pass_set", "main_render"},
                        {"shader", {{"vertex", "engine://fullscreen"},
                                    {"fragment", std::string{"project://shaders/"} + fragment}}},
                        {"ports", std::move(ports)}};
        if (once) parameters["schedule"] = "once";
        return Json{{"id", id}, {"op", "render.fullscreen"}, {"parameters", parameters},
                    {"inputs", std::move(inputs)}, {"outputs", {"color"}}};
    };
    auto root = blank_root;
    root["nodes"] = Json::array({
        {{"id", "level_value"}, {"op", "value.constant"}, {"parameters", {{"value", level}}},
         {"outputs", {"value"}}},
        {{"id", "level"}, {"op", "render.buffer"}, {"parameters", {{"element", "float"}}},
         {"inputs", {{"value", edge("level_value", "value")}}}, {"outputs", {"resource"}}},
        image("h0_image", true, false),
        pass("h0", "ps1_h0", {{"level", {{"kind", "buffer"}, {"element", "float"}, {"access", "read"}}},
                              {"color", color}},
             {{"level", edge("level", "resource")}, {"color", edge("h0_image", "resource")}}, true),
        image("twiddle_image", true, false),
        pass("twiddle", "ps1_twiddle", {{"source", input}, {"color", color}},
             {{"source", edge("h0", "color")}, {"color", edge("twiddle_image", "resource")}}, true),
        image("show_image", false, true),
        pass("show", "ps1_show", {{"source", input}, {"color", color}},
             {{"source", edge("twiddle", "color")}, {"color", edge("show_image", "resource")}}, false),
    });
    return root;
}
} // namespace

// PS-1 acceptance 1-3 and Play / Stop, through the running renderer.
TEST_CASE("PS-1 once passes run on the first frame and again only when asked, edited or upstream ran",
          "[ps1][frame-plan][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("ps1_once");
    const auto root = dir.root / "project";
    initializeProjectTemplate(root, ProjectTemplate::Blank);
    const auto rendering_path = root / "passes/main_rendering_config.json";
    const auto blank_root = Json::parse(readFile(rendering_path));
    writeFile(rendering_path, ps1RenderingRoot(blank_root, 0.25).dump(2));
    writeFile(root / "shaders/ps1_h0.frag", ps1_h0_frag);
    writeFile(root / "shaders/ps1_twiddle.frag", ps1_twiddle_frag);
    writeFile(root / "shaders/ps1_show.frag", ps1_show_frag);

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    auto &renderer = GET_MODULE(Renderer);
    test_support::RendererTestAccess::setExecutionTracing(renderer, true);

    // The one value every texel of show_image holds.
    const auto shown = [&] {
        GET_MODULE(VulkanManageCore).waitIdle();
        const auto read =
            test_support::RendererTestAccess::readRenderTarget(renderer, "show_image", ImageSubresourceRange{});
        REQUIRE(read.format == vk::Format::eR8G8B8A8Unorm);
        std::set<std::array<std::uint8_t, 4>> values;
        for (std::size_t offset = 0; offset + 4 <= read.bytes.size(); offset += 4)
            values.insert({read.bytes.at(offset), read.bytes.at(offset + 1), read.bytes.at(offset + 2),
                           read.bytes.at(offset + 3)});
        REQUIRE(values.size() == 1);
        return *values.begin();
    };
    // The frame plan's once nodes: name -> {last_run_generation, last_run_frame}.
    const auto runs = [&] {
        const auto plan = rpc.result("get_frame_plan");
        std::map<std::string, std::pair<Json, Json>> result;
        for (const auto &node : plan.at("nodes")) {
            if (node.value("schedule", std::string{}) != "once") continue;
            result.emplace(node.at("name").get<std::string>(),
                           std::pair{node.at("last_run_generation"), node.at("last_run_frame")});
        }
        return std::pair{result, plan.at("runtime_generation")};
    };
    // Whether the last frame recorded each node (the execution trace).
    const auto recorded = [&](const std::string &name) {
        const auto &trace = test_support::RendererTestAccess::lastExecutionTrace(renderer);
        for (const auto &node : trace.at("nodes"))
            if (node.at("name") == name) return node.value("recorded", true);
        FAIL("no trace entry for " << name);
        return false;
    };
    const auto frame = [&] { (void)rpc.result("step_frame"); };

    // 1. Frame 1 runs both once passes; frames 2-5 record neither, and the
    // pattern stays.
    frame();
    CHECK(recorded("h0"));
    CHECK(recorded("twiddle"));
    CHECK(recorded("show"));
    const auto [first, generation] = runs();
    REQUIRE(first.size() == 2);
    CHECK(first.at("h0").first == generation);
    CHECK(first.at("twiddle").first == generation);
    REQUIRE(first.at("h0").second.is_number_unsigned());
    {
        // Acceptance 5 in the running frame plan: the persistent images are
        // stored and never alias candidates, and the reason says why.
        const auto plan = rpc.result("get_frame_plan");
        REQUIRE(plan.contains("physical_target_plan"));
        std::set<std::string> seen;
        for (const auto &resource : plan.at("physical_target_plan").at("resources")) {
            const auto name = resource.at("logical_resource").get<std::string>();
            if (name != "h0_image" && name != "twiddle_image") continue;
            INFO(resource.dump(2));
            seen.insert(name);
            CHECK(resource.at("aliasable") == false);
            CHECK(resource.at("stored") == true);
            CHECK_THAT(resource.at("reason").get<std::string>(),
                       Catch::Matchers::ContainsSubstring("persistent lifetime"));
        }
        CHECK(seen == std::set<std::string>{"h0_image", "twiddle_image"});
    }
    const auto pattern = shown();
    // level 0.25 -> 64; green 0.25 doubled -> 128; blue 0.5 -> 127 or 128.
    CHECK(int{pattern[0]} == 64);
    CHECK(int{pattern[1]} == 128);
    CHECK(std::abs(int{pattern[2]} - 128) <= 1);
    for (int repeat = 0; repeat < 4; ++repeat) {
        frame();
        CHECK_FALSE(recorded("h0"));
        CHECK_FALSE(recorded("twiddle"));
        CHECK(recorded("show"));
        CHECK(shown() == pattern);
        CHECK(runs().first == first);
    }

    // 2. rerun_once: h0 runs on the next frame, and twiddle with it (it reads
    // what h0 wrote); then neither again.
    CHECK(rpc.result("rerun_once", {{"node", "h0"}}) == Json{{"scheduled", Json::array({"h0"})}});
    frame();
    CHECK(recorded("h0"));
    CHECK(recorded("twiddle"));
    const auto rerun = runs().first;
    CHECK(rerun.at("h0").second != first.at("h0").second);
    CHECK(rerun.at("twiddle").second == rerun.at("h0").second);
    CHECK(shown() == pattern);
    frame();
    CHECK_FALSE(recorded("h0"));
    CHECK_FALSE(recorded("twiddle"));
    // Without a node: every once node, in plan order. A node that runs every
    // frame schedules nothing; an unknown node is invalid params.
    CHECK(rpc.result("rerun_once") == Json{{"scheduled", Json::array({"h0", "twiddle"})}});
    CHECK(rpc.result("rerun_once", {{"node", "show"}}) == Json{{"scheduled", Json::array()}});
    {
        const auto unknown = rpc.call("rerun_once", {{"node", "nowhere"}});
        INFO(unknown.dump(2));
        REQUIRE(unknown.contains("error"));
        CHECK(unknown.at("error").at("code") == -32602);
    }
    frame();
    CHECK(recorded("h0"));
    CHECK(recorded("twiddle"));
    frame();
    CHECK_FALSE(recorded("h0"));

    // 5. Play and Stop neither change the generation nor run them again.
    {
        const auto before = runs();
        CHECK(rpc.result("set_play_state", {{"state", "playing"}}).at("state") == "playing");
        (void)rpc.result("render_frame");
        CHECK(rpc.result("set_play_state", {{"state", "edit"}}).at("state") == "edit");
        frame();
        const auto after = runs();
        INFO(after.second.dump());
        CHECK(after.second == before.second);
        CHECK_FALSE(recorded("h0"));
        CHECK(after.first == before.first);
        CHECK(shown() == pattern);
    }

    // 2. The constant edited (the document rewritten and reloaded): the new
    // generation runs both once passes, and the pattern follows the value.
    writeFile(rendering_path, ps1RenderingRoot(blank_root, 0.75).dump(2));
    const auto reloaded = rpc.result("reload_render_documents");
    INFO(reloaded.dump(2));
    REQUIRE(reloaded.at("status") == "accepted");
    Json committed;
    for (int attempt = 0; attempt < 8; ++attempt) {
        frame();
        committed = rpc.result("get_edit_result", {{"ticket", reloaded.at("ticket")}});
        if (committed.value("status", std::string{}) != "pending") break;
    }
    INFO(committed.dump(2));
    REQUIRE(committed.at("status") == "committed");
    frame();
    const auto [edited, edited_generation] = runs();
    CHECK(edited_generation != generation);
    CHECK(edited.at("h0").first == edited_generation);
    CHECK(edited.at("twiddle").first == edited_generation);
    const auto changed = shown();
    CHECK(int{changed[0]} == 191);
    CHECK(int{changed[1]} == 128);
    frame();
    CHECK_FALSE(recorded("h0"));
    CHECK(shown() == changed);
    GET_MODULE(VulkanManageCore).waitIdle();
}

// FP-1: get_frame_plan builds what the published generation decides once,
// reads what a frame changed on every call, and answers the bytes a call
// that builds everything answers.
TEST_CASE("FP-1 get_frame_plan builds its generation's part once and reads a frame's part on every call",
          "[fp1][frame-plan][rpc][gpu]") {
    using Access = test_support::RendererTestAccess;
    setupLogger(true);
    const auto dir = makeTempDir("fp1_frame_plan");
    const auto root = dir.root / "project";
    initializeProjectTemplate(root, ProjectTemplate::Blank);
    const auto rendering_path = root / "passes/main_rendering_config.json";
    const auto blank_root = Json::parse(readFile(rendering_path));
    writeFile(rendering_path, ps1RenderingRoot(blank_root, 0.25).dump(2));
    writeFile(root / "shaders/ps1_h0.frag", ps1_h0_frag);
    writeFile(root / "shaders/ps1_twiddle.frag", ps1_twiddle_frag);
    writeFile(root / "shaders/ps1_show.frag", ps1_show_frag);

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    const auto &renderer = GET_MODULE(Renderer);
    // The whole response line, the same request every time.
    const auto answer = [&] {
        const auto line = rpc.endpoint.processLine(
            Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "get_frame_plan"}, {"params", Json::object()}}
                .dump());
        INFO(line.substr(0, 400));
        REQUIRE(Json::parse(line).contains("result"));
        return line;
    };
    // The answer of a call that builds the generation's part again.
    const auto built_afresh = [&] {
        Access::dropFramePlanGenerationPart(renderer);
        return answer();
    };
    const auto result_of = [](const std::string &line) { return Json::parse(line).at("result"); };
    const auto last_run_frame = [&](const std::string &line, std::string_view name) {
        const auto plan = result_of(line);
        for (const auto &node : plan.at("nodes"))
            if (node.at("name") == std::string{name}) return node.at("last_run_frame");
        FAIL("the frame plan has no node " << name);
        return Json{};
    };
    const auto step = [&] { (void)rpc.result("step_frame"); };

    // 1. Twice in one generation: built once, the same bytes both times,
    // and the bytes of a call that builds everything.
    step();
    const auto first = built_afresh();
    const auto builds = Access::framePlanGenerationPartBuilds(renderer);
    const auto second = answer();
    CHECK(Access::framePlanGenerationPartBuilds(renderer) == builds);
    CHECK(second == first);
    REQUIRE(last_run_frame(first, "h0").is_number_unsigned());

    // 2. A frame's part is read on every call: h0 runs again in the same
    // generation, and the next answer says so without building anything.
    CHECK(rpc.result("rerun_once", {{"node", "h0"}}) == Json{{"scheduled", Json::array({"h0"})}});
    step();
    const auto rerun = answer();
    CHECK(Access::framePlanGenerationPartBuilds(renderer) == builds);
    CHECK(result_of(rerun).at("runtime_generation") == result_of(first).at("runtime_generation"));
    CHECK(last_run_frame(rerun, "h0") != last_run_frame(first, "h0"));
    CHECK(last_run_frame(rerun, "twiddle") == last_run_frame(rerun, "h0"));
    CHECK(rerun == built_afresh());
    const auto before_reload = Access::framePlanGenerationPartBuilds(renderer);

    // 3. Another generation: the document rewritten and reloaded. The next
    // answer is built for it once, and is again what building it all gives.
    writeFile(rendering_path, ps1RenderingRoot(blank_root, 0.75).dump(2));
    const auto reloaded = rpc.result("reload_render_documents");
    INFO(reloaded.dump(2));
    REQUIRE(reloaded.at("status") == "accepted");
    Json committed;
    for (int attempt = 0; attempt < 8; ++attempt) {
        step();
        committed = rpc.result("get_edit_result", {{"ticket", reloaded.at("ticket")}});
        if (committed.value("status", std::string{}) != "pending") break;
    }
    INFO(committed.dump(2));
    REQUIRE(committed.at("status") == "committed");
    step();
    const auto edited = answer();
    const auto built_for_edit = Access::framePlanGenerationPartBuilds(renderer);
    CHECK(built_for_edit > before_reload);
    CHECK(result_of(edited).at("runtime_generation") != result_of(first).at("runtime_generation"));
    CHECK(last_run_frame(edited, "h0") != last_run_frame(rerun, "h0"));
    CHECK(answer() == edited);
    CHECK(Access::framePlanGenerationPartBuilds(renderer) == built_for_edit);
    CHECK(edited == built_afresh());
    GET_MODULE(VulkanManageCore).waitIdle();
}


namespace {
// IA-1: lut_float (RGBA32F, 64x64, from a KTX2 the writer made, extent taken
// from the asset), lut_unorm and lut_srgb (RGBA8, 4x4, both from one PNG)
// are persistent images that start from their assets; a fullscreen pass per
// image, run every frame, copies it texel for texel into an RGBA32F image
// the test reads back.
constexpr std::string_view ia1_show_frag =
    "#version 460\n#extension GL_GOOGLE_include_directive : require\n"
    "#include \"pelican_sets.glsl\"\n#include \"pelican_view.glsl\"\n\n"
    "layout(location = 0) in vec2 inUV;\nlayout(location = 0) out vec4 outColor;\n"
    "PELICAN_DECLARE_INPUT_0(field);\n\n"
    "void main() {\n"
    "    outColor = PELICAN_TEXTURE_2D_0(field, inUV);\n"
    "}\n";

Json ia1RenderingRoot(const Json &blank_root) {
    const Json type{{"semantic", "pelican.render.color_signal@1"},
                    {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}};
    const Json color{{"kind", "image"}, {"type", type}, {"access", "write"}, {"attachment", "color"},
                     {"location", 0}};
    const Json input{{"kind", "image"}, {"type", type}, {"access", "read"}, {"binding", "input"},
                     {"input_index", 0}};
    const auto edge = [](const std::string &node, const char *port) { return Json{{"node", node}, {"port", port}}; };
    const auto lut = [&](const char *id, const char *format, const char *asset, std::optional<int> extent) {
        Json parameters{{"type", type},
                        {"format", format},
                        {"lifetime", "persistent"},
                        {"usage", {"SAMPLED", "TRANSFER_SRC"}},
                        {"initial", {{"asset", asset}}}};
        if (extent) {
            parameters["width"] = *extent;
            parameters["height"] = *extent;
        }
        return Json{{"id", id}, {"op", "render.image"}, {"parameters", parameters}, {"outputs", {"resource"}}};
    };
    const auto shown = [&](const std::string &id, int extent) {
        return Json{{"id", id}, {"op", "render.image"},
                    {"parameters", {{"type", type}, {"format", "R32G32B32A32_SFLOAT"}, {"extent_scale", 1.0},
                                    {"width", extent}, {"height", extent},
                                    {"usage", {"COLOR_ATTACHMENT", "TRANSFER_SRC"}}}},
                    {"outputs", {"resource"}}};
    };
    const auto show = [&](const std::string &id, const std::string &source, const std::string &target) {
        return Json{{"id", id}, {"op", "render.fullscreen"},
                    {"parameters", {{"pass_set", "main_render"},
                                    {"shader", {{"vertex", "engine://fullscreen"},
                                                {"fragment", "project://shaders/ia1_show"}}},
                                    {"ports", {{"source", input}, {"color", color}}}}},
                    {"inputs", {{"source", edge(source, "resource")}, {"color", edge(target, "resource")}}},
                    {"outputs", {"color"}}};
    };
    auto root = blank_root;
    root["nodes"] = Json::array({
        lut("lut_float", "R32G32B32A32_SFLOAT", "assets/luts/ltc.ktx2", std::nullopt),
        lut("lut_unorm", "R8G8B8A8_UNORM", "assets/luts/ramp.png", 4),
        lut("lut_srgb", "R8G8B8A8_SRGB", "assets/luts/ramp.png", 4),
        shown("shown_float", 64),
        shown("shown_unorm", 4),
        shown("shown_srgb", 4),
        show("show_float", "lut_float", "shown_float"),
        show("show_unorm", "lut_unorm", "shown_unorm"),
        show("show_srgb", "lut_srgb", "shown_srgb"),
    });
    return root;
}

// 64x64 RGBA32F texels no 8- or 16-bit format holds: offset by `base`.
std::vector<float> ia1LutValues(float base) {
    std::vector<float> values(std::size_t{64} * 64 * 4);
    for (std::size_t texel = 0; texel < 64 * 64; ++texel) {
        const auto x = static_cast<float>(texel % 64);
        const auto y = static_cast<float>(texel / 64);
        values[texel * 4 + 0] = base + x * 0.0078125f + 1.0e-6f;
        values[texel * 4 + 1] = -3.5f + y * 12.25f;
        values[texel * 4 + 2] = 1234.5678f / (1.0f + x + y);
        values[texel * 4 + 3] = 0.333333343f;
    }
    return values;
}

void ia1WriteLut(const std::filesystem::path &path, const std::vector<float> &values) {
    std::vector<std::byte> texels(values.size() * sizeof(float));
    std::memcpy(texels.data(), values.data(), texels.size());
    const auto file = writeKtx2(Ktx2Format::R32G32B32A32Sfloat, 64, 64, texels);
    writeFile(path, std::string_view{reinterpret_cast<const char *>(file.data()), file.size()});
}

std::vector<float> ia1ReadFloats(Renderer &renderer, std::string_view name) {
    GET_MODULE(VulkanManageCore).waitIdle();
    const auto read = test_support::RendererTestAccess::readRenderTarget(renderer, name, ImageSubresourceRange{});
    REQUIRE(read.format == vk::Format::eR32G32B32A32Sfloat);
    std::vector<float> values(read.bytes.size() / sizeof(float));
    std::memcpy(values.data(), read.bytes.data(), values.size() * sizeof(float));
    return values;
}

float ia1SrgbToLinear(float encoded) {
    return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}
} // namespace

// IA-1 acceptance 1 and 2, through the running renderer: the float LUT's
// values come out bit for bit, the PNG's bytes are data in the UNORM image
// and sRGB in the SRGB one (the GPU decodes them), get_frame_plan names the
// assets, and a rewritten LUT reloaded is copied in on the new generation.
TEST_CASE("IA-1 persistent images start from their KTX2 and PNG assets and per-frame passes read them",
          "[ia1][frame-plan][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("ia1_initial");
    const auto root = dir.root / "project";
    initializeProjectTemplate(root, ProjectTemplate::Blank);
    // An asset store with a manifest, so the frame plan has the sha256.
    auto project = Json::parse(readFile(root / "project.json"));
    project["asset_stores"] = {{"main", {{"mount", "assets/"}, {"manifest", "assets.manifest.json"}}}};
    writeFile(root / "project.json", project.dump(2));
    ia1WriteLut(root / "assets/luts/ltc.ktx2", ia1LutValues(0.5f));
    // A 4x4 ramp whose bytes differ between the UNORM and the sRGB reading.
    std::vector<unsigned char> ramp(std::size_t{4} * 4 * 4);
    for (std::size_t texel = 0; texel < 16; ++texel) {
        ramp[texel * 4 + 0] = static_cast<unsigned char>(texel * 17);
        ramp[texel * 4 + 1] = static_cast<unsigned char>(255 - texel * 13);
        ramp[texel * 4 + 2] = static_cast<unsigned char>(64 + texel * 5);
        ramp[texel * 4 + 3] = static_cast<unsigned char>(200);
    }
    std::filesystem::create_directories(root / "assets/luts");
    REQUIRE(stbi_write_png((root / "assets/luts/ramp.png").string().c_str(), 4, 4, 4, ramp.data(), 16) != 0);
    (void)generateAssetsManifest(root / "assets", root / "assets.manifest.json", dir.root / "assets.cache.json",
                                 "main", true);
    const auto rendering_path = root / "passes/main_rendering_config.json";
    const auto blank_root = Json::parse(readFile(rendering_path));
    writeFile(rendering_path, ia1RenderingRoot(blank_root).dump(2));
    writeFile(root / "shaders/ia1_show.frag", ia1_show_frag);

    FastModuleContainer modules;
    configureHeadless(root);
    // The headless setup has no envelope; give the resolver the asset store.
    GET_MODULE(PathResolver).resetForTesting();
    GET_MODULE(PathResolver).setup(root, false, readFile(root / "project.json"));
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    auto &renderer = GET_MODULE(Renderer);
    const auto frame = [&] { (void)rpc.result("step_frame"); };

    frame();
    // 1. The float LUT, its 64x64 extent taken from the asset, reaches the
    // screen bit for bit.
    const auto expected = ia1LutValues(0.5f);
    CHECK(ia1ReadFloats(renderer, "shown_float") == expected);
    CHECK(ia1ReadFloats(renderer, "lut_float") == expected);
    frame();
    CHECK(ia1ReadFloats(renderer, "shown_float") == expected);

    // 2. One PNG: the UNORM image reads its bytes / 255, the SRGB image the
    // same bytes decoded from sRGB by the GPU.
    const auto unorm = ia1ReadFloats(renderer, "shown_unorm");
    const auto srgb = ia1ReadFloats(renderer, "shown_srgb");
    REQUIRE(unorm.size() == ramp.size());
    REQUIRE(srgb.size() == ramp.size());
    std::size_t differing = 0;
    for (std::size_t component = 0; component < ramp.size(); ++component) {
        INFO(component);
        const auto encoded = static_cast<float>(ramp[component]) / 255.0f;
        const bool alpha = component % 4 == 3;
        CHECK(std::abs(unorm[component] - encoded) < 1.0e-6f);
        CHECK(std::abs(srgb[component] - (alpha ? encoded : ia1SrgbToLinear(encoded))) < 2.0e-3f);
        if (std::abs(unorm[component] - srgb[component]) > 1.0e-2f) ++differing;
    }
    CHECK(differing > 20);

    // The frame plan names each image's asset and its manifest sha256.
    {
        const auto plan = rpc.result("get_frame_plan");
        std::map<std::string, Json> initial;
        for (const auto &resource : plan.at("physical_target_plan").at("resources"))
            if (resource.contains("initial_asset"))
                initial.emplace(resource.at("logical_resource").get<std::string>(), resource.at("initial_asset"));
        INFO(plan.at("physical_target_plan").dump(2));
        CHECK(initial == std::map<std::string, Json>{
                             {"lut_float", {{"asset", "assets/luts/ltc.ktx2"},
                                            {"sha256", assetFileSha256(root / "assets/luts/ltc.ktx2")}}},
                             {"lut_unorm", {{"asset", "assets/luts/ramp.png"},
                                            {"sha256", assetFileSha256(root / "assets/luts/ramp.png")}}},
                             {"lut_srgb", {{"asset", "assets/luts/ramp.png"},
                                           {"sha256", assetFileSha256(root / "assets/luts/ramp.png")}}}});
    }

    // 3. The LUT rewritten on disk and the documents reloaded: the new
    // generation reads the asset again and copies the new values in. The
    // root is rewritten too (same content, other layout); IA-2's test
    // reloads the asset alone.
    ia1WriteLut(root / "assets/luts/ltc.ktx2", ia1LutValues(7.0f));
    writeFile(rendering_path, ia1RenderingRoot(blank_root).dump(4));
    const auto reloaded = rpc.result("reload_render_documents");
    INFO(reloaded.dump(2));
    REQUIRE(reloaded.at("status") == "accepted");
    Json committed;
    for (int attempt = 0; attempt < 8; ++attempt) {
        frame();
        committed = rpc.result("get_edit_result", {{"ticket", reloaded.at("ticket")}});
        if (committed.value("status", std::string{}) != "pending") break;
    }
    INFO(committed.dump(2));
    REQUIRE(committed.at("status") == "committed");
    frame();
    CHECK(ia1ReadFloats(renderer, "shown_float") == ia1LutValues(7.0f));
    GET_MODULE(VulkanManageCore).waitIdle();
}

// IA-2: Studio's engine runs without the file watcher, so a LUT rewritten on
// disk reaches the frame through reload_render_documents. The documents stay
// byte for byte; the asset's bytes differing from what the live generation
// read is enough to publish a new one, and the result names the asset.
TEST_CASE("IA-2 reload_render_documents takes in an initial asset rewritten on disk with the documents unchanged",
          "[ia2][rl1][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("ia2_reload_asset");
    const auto root = dir.root / "project";
    initializeProjectTemplate(root, ProjectTemplate::Blank);
    auto project = Json::parse(readFile(root / "project.json"));
    project["asset_stores"] = {{"main", {{"mount", "assets/"}, {"manifest", "assets.manifest.json"}}}};
    writeFile(root / "project.json", project.dump(2));
    const auto lut_path = root / "assets/luts/ltc.ktx2";
    ia1WriteLut(lut_path, ia1LutValues(0.5f));
    (void)generateAssetsManifest(root / "assets", root / "assets.manifest.json", dir.root / "assets.cache.json",
                                 "main", true);
    // IA-1's frame with the float LUT only.
    const auto rendering_path = root / "passes/main_rendering_config.json";
    auto rendering = ia1RenderingRoot(Json::parse(readFile(rendering_path)));
    auto &nodes = rendering.at("nodes");
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                               [](const Json &node) {
                                   const auto id = node.at("id").get<std::string>();
                                   return id != "lut_float" && id != "shown_float" && id != "show_float";
                               }),
                nodes.end());
    REQUIRE(nodes.size() == 3);
    const auto rendering_bytes = rendering.dump(2);
    writeFile(rendering_path, rendering_bytes);
    writeFile(root / "shaders/ia1_show.frag", ia1_show_frag);

    FastModuleContainer modules;
    configureHeadless(root);
    GET_MODULE(PathResolver).resetForTesting();
    GET_MODULE(PathResolver).setup(root, false, readFile(root / "project.json"));
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    auto &renderer = GET_MODULE(Renderer);
    const auto frame = [&] { (void)rpc.result("step_frame"); };
    const auto reload = [&] {
        const auto accepted = rpc.result("reload_render_documents");
        INFO(accepted.dump(2));
        if (accepted.value("status", std::string{}) == "rejected") return accepted;
        REQUIRE(accepted.at("status") == "accepted");
        for (int attempt = 0; attempt < 8; ++attempt) {
            frame();
            auto result = rpc.result("get_edit_result", {{"ticket", accepted.at("ticket")}});
            if (result.value("status", std::string{}) != "pending") return result;
        }
        FAIL("the ticket stayed pending");
        return Json{};
    };
    const auto source_digest = [&] {
        return rpc.result("get_render_authoring_context").at("source_digest").at("hex").get<std::string>();
    };

    frame();
    REQUIRE(ia1ReadFloats(renderer, "shown_float") == ia1LutValues(0.5f));
    const auto digest_before = source_digest();

    // 2. Nothing changed: no_change, no asset named.
    std::uint64_t generation = 0;
    {
        const auto result = reload();
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "committed");
        CHECK(result.at("no_change") == true);
        CHECK(result.at("changed") == Json::array());
        CHECK(result.at("changed_assets") == Json::array());
        generation = result.at("published_generation").get<std::uint64_t>();
    }

    // 1. Only the LUT's bytes rewritten: committed as a new generation that
    // names the asset and no document, and the frame shows the new values.
    ia1WriteLut(lut_path, ia1LutValues(7.0f));
    REQUIRE(readFile(rendering_path) == rendering_bytes);
    {
        const auto result = reload();
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "committed");
        CHECK_FALSE(result.contains("no_change"));
        CHECK(result.at("changed") == Json::array());
        CHECK(result.at("changed_assets") == Json::array({"assets/luts/ltc.ktx2"}));
        CHECK(result.at("published_generation").get<std::uint64_t>() > generation);
        CHECK(result.at("source_digest").at("hex") == digest_before);
        generation = result.at("published_generation").get<std::uint64_t>();
    }
    frame();
    CHECK(ia1ReadFloats(renderer, "shown_float") == ia1LutValues(7.0f));
    CHECK(source_digest() == digest_before);

    // The new generation read what is on disk: the next reload is no_change.
    {
        const auto result = reload();
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "committed");
        CHECK(result.at("no_change") == true);
        CHECK(result.at("changed_assets") == Json::array());
        CHECK(result.at("published_generation").get<std::uint64_t>() == generation);
    }

    // An asset that no longer fits the image (one channel for an RGBA32F
    // image) is refused by name, and the live frame keeps the last LUT.
    {
        std::vector<std::byte> texels(std::size_t{64} * 64 * sizeof(float));
        const auto file = writeKtx2(Ktx2Format::R32Sfloat, 64, 64, texels);
        writeFile(lut_path, std::string_view{reinterpret_cast<const char *>(file.data()), file.size()});
        const auto result = reload();
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "rejected");
        CHECK(result.at("error").at("code") == "graph.image_initial_mismatch@1");
        CHECK(result.at("error").at("node") == "lut_float");
        CHECK(result.at("error").at("asset") == "assets/luts/ltc.ktx2");
        CHECK(result.at("error").at("what") == "format");
    }
    frame();
    CHECK(ia1ReadFloats(renderer, "shown_float") == ia1LutValues(7.0f));
    CHECK(source_digest() == digest_before);
    GET_MODULE(VulkanManageCore).waitIdle();
}

// IA-3: a table only value.sample_table reads is read when the value graph is
// built, so it is one of the assets reload_render_documents compares. The
// table rewritten with its write time put back (same size): the reload names
// it, publishes a new generation, and that generation's node reads the new
// values (the table cache judged by the bytes, not the write time and size).
// The manifest beside it is compared too. Nothing changed: no_change.
TEST_CASE("IA-3 reload_render_documents takes in a value.sample_table table rewritten on disk with the documents "
          "unchanged",
          "[ia3][ia2][ab1][rl1][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("ia3_reload_table");
    const auto root = dir.root / "project";
    initializeProjectTemplate(root, ProjectTemplate::Blank);
    auto project = Json::parse(readFile(root / "project.json"));
    project["asset_stores"] = {{"main", {{"mount", "assets/"}, {"manifest", "assets.manifest.json"}}}};
    writeFile(root / "project.json", project.dump(2));
    const std::string asset = "assets/tables/level.ktx2";
    const auto table_path = root / asset;
    const auto manifest_path = derivedAssetManifestPath(table_path);
    // One feature, three samples, every one `value`: the node reads it at any
    // scene time.
    const auto writeTable = [&](float value, double hop_seconds) {
        SampleTableLayout layout;
        layout.sample_count = 3;
        layout.hop_seconds = hop_seconds;
        layout.features = {"level"};
        std::vector<float> values(sampleTableWidth, 0.0f);
        std::fill_n(values.begin(), 3, value);
        const auto file = writeKtx2(Ktx2Format::R32Sfloat, sampleTableWidth, 1, std::as_bytes(std::span{values}));
        writeFile(table_path, std::string_view{reinterpret_cast<const char *>(file.data()), file.size()});
        writeFile(manifest_path, Json{{"source", {{"metadata", {{"layout", Json(sampleTableLayoutJson(layout))}}}}}}.dump());
    };
    writeTable(3.0f, 0.5);
    (void)generateAssetsManifest(root / "assets", root / "assets.manifest.json", dir.root / "assets.cache.json",
                                 "main", true);
    const auto rendering_path = root / "passes/main_rendering_config.json";
    auto rendering = Json::parse(readFile(rendering_path));
    rendering.at("nodes").push_back({{"id", "table_level"},
                                     {"op", "value.sample_table"},
                                     {"parameters", {{"asset", asset}, {"row", "level"}}},
                                     {"outputs", Json::array({"value"})}});
    const auto rendering_bytes = rendering.dump(2);
    writeFile(rendering_path, rendering_bytes);

    FastModuleContainer modules;
    configureHeadless(root);
    GET_MODULE(PathResolver).resetForTesting();
    GET_MODULE(PathResolver).setup(root, false, readFile(root / "project.json"));
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    const auto frame = [&] { (void)rpc.result("step_frame"); };
    const auto reload = [&] {
        const auto accepted = rpc.result("reload_render_documents");
        INFO(accepted.dump(2));
        if (accepted.value("status", std::string{}) == "rejected") return accepted;
        REQUIRE(accepted.at("status") == "accepted");
        for (int attempt = 0; attempt < 8; ++attempt) {
            frame();
            auto result = rpc.result("get_edit_result", {{"ticket", accepted.at("ticket")}});
            if (result.value("status", std::string{}) != "pending") return result;
        }
        FAIL("the ticket stayed pending");
        return Json{};
    };
    // The node's value in the published generation, and that generation.
    const auto level = [&] {
        const auto values = rpc.result("get_graph_node_values");
        INFO(values.dump(2));
        for (const auto &node : values.at("nodes"))
            if (node.at("node") == "table_level")
                return std::pair{node.at("outputs").at("value").get<double>(),
                                 values.at("generation").get<std::uint64_t>()};
        FAIL("table_level is not listed");
        return std::pair{0.0, std::uint64_t{0}};
    };

    frame();
    REQUIRE(level().first == 3.0);

    // Nothing changed: no_change, no asset named.
    std::uint64_t generation = 0;
    {
        const auto result = reload();
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "committed");
        CHECK(result.at("no_change") == true);
        CHECK(result.at("changed") == Json::array());
        CHECK(result.at("changed_assets") == Json::array());
        generation = result.at("published_generation").get<std::uint64_t>();
    }

    // The table baked again with other values, same size, the write times
    // put back: a new generation that names the table and no document, and
    // its node reads 9.
    const auto written = std::filesystem::last_write_time(table_path);
    const auto manifest_written = std::filesystem::last_write_time(manifest_path);
    writeTable(9.0f, 0.5);
    std::filesystem::last_write_time(table_path, written);
    std::filesystem::last_write_time(manifest_path, manifest_written);
    REQUIRE(readFile(rendering_path) == rendering_bytes);
    {
        const auto result = reload();
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "committed");
        CHECK_FALSE(result.contains("no_change"));
        CHECK(result.at("changed") == Json::array());
        CHECK(result.at("changed_assets") == Json::array({asset}));
        CHECK(result.at("published_generation").get<std::uint64_t>() > generation);
        generation = result.at("published_generation").get<std::uint64_t>();
    }
    frame();
    {
        const auto [value, published] = level();
        CHECK(published == generation);
        CHECK(value == 9.0);
    }

    // The new generation read what is on disk: the next reload is no_change.
    {
        const auto result = reload();
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "committed");
        CHECK(result.at("no_change") == true);
        CHECK(result.at("changed_assets") == Json::array());
        CHECK(result.at("published_generation").get<std::uint64_t>() == generation);
    }

    // Only the manifest beside it changed (another hop): the table is named.
    {
        const auto table_bytes = readFile(table_path);
        writeTable(9.0f, 0.25);
        REQUIRE(readFile(table_path) == table_bytes);
        const auto result = reload();
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "committed");
        CHECK_FALSE(result.contains("no_change"));
        CHECK(result.at("changed_assets") == Json::array({asset}));
        CHECK(result.at("published_generation").get<std::uint64_t>() > generation);
    }
    GET_MODULE(VulkanManageCore).waitIdle();
}

namespace {

// FR-3: the deferred pipeline the GUI session built from File > New Project >
// Blank on the Graph tab (gui_settings_choose's project, its second lighting
// pass left out): a deferred_geometry_v1 render.material_pass drawing its
// G-buffer and depth into render.image nodes of its own, the Deferred
// lighting template's fullscreen pass reading them and shadow_directional's
// shadow_map through shadow_map_ref, and present.
std::filesystem::path fr3BlankDeferredProject(const std::filesystem::path &dir) {
    const auto root = dir / "project";
    initializeProjectTemplate(root, ProjectTemplate::Blank);
    const auto fixtures = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" / "fr3_blank_graph";
    writeFile(root / "passes/main_rendering_config.json", readFile(fixtures / "main_rendering_config.graph"));
    writeFile(root / "shaders/deferred_lighting.frag", readFile(fixtures / "deferred_lighting.frag"));
    return root;
}

// The engine with that project open, answering RPC, a frame drawn.
struct Fr3Engine {
    FastModuleContainer modules;
    // The endpoint reads the configured project: configured first.
    bool configured;
    Rpc rpc;

    explicit Fr3Engine(const std::filesystem::path &root) : configured{(configureHeadless(root), true)} {
        (void)rpc.result("load_scene", {{"name", "default_scene"}});
        GET_MODULE(Renderer).prepareRuntimeModules();
        (void)rpc.result("step_frame");
        (void)rpc.result("render_frame");
    }
    ~Fr3Engine() { GET_MODULE(VulkanManageCore).waitIdle(); }

    std::string digest() {
        return rpc.result("get_render_authoring_context").at("source_digest").at("hex").get<std::string>();
    }
    // The final answer: a refusal when it is asked for, or the ticket's
    // result once the running renderer judged it.
    Json settle(const Json &accepted) {
        INFO(accepted.dump(2));
        if (accepted.value("status", std::string{}) != "accepted") return accepted;
        for (int attempt = 0; attempt < 8; ++attempt) {
            (void)rpc.result("step_frame");
            auto result = rpc.result("get_edit_result", {{"ticket", accepted.at("ticket")}});
            if (result.value("status", std::string{}) != "pending") return result;
        }
        FAIL("the ticket stayed pending");
        return Json{};
    }
    Json send(std::string_view method, Json params) {
        params["base_source_digest"] = digest();
        return settle(rpc.result(method, std::move(params)));
    }
    Json batch(Json edits) { return send("edit_batch", {{"edits", std::move(edits)}}); }
};

const Json fr3Depth{{"semantic", "pelican.render.depth@1"},
                    {"arguments", {{"representation", "device"}, {"space", "projection"}}}};
const Json fr3ArrayDepth{{"semantic", "pelican.render.depth@1"},
                         {"arguments", {{"representation", "device"}, {"space", "projection"},
                                        {"topology", "two_d_array"}}}};

Json fr3SkyParams() {
    return {{"operations", Json::array({{{"op", "add"}, {"feature", "engine://features/sky_ambient.json"}}})}};
}

Json fr3CascadeParams() {
    return {{"operations", Json::array({{{"op", "set_parameters"}, {"index", 0},
                                         {"parameters", {{"pcf_radius", 2}, {"strength", 1}, {"cascade_count", 3}}}}})}};
}

const Json *fr3PlanNode(const Json &plan, std::string_view name) {
    for (const auto &node : plan.at("nodes"))
        if (node.at("name").get<std::string>() == name) return &node;
    return nullptr;
}

bool fr3Lists(const Json &list, std::string_view name) {
    return std::find(list.begin(), list.end(), Json(name)) != list.end();
}

} // namespace

// FR-3 (UX-12b): sky_ambient on that pipeline, with the targets and
// target_refs Studio adds for it, was refused only by the frame planner's text
// ("Pass input target is not produced as an earlier output: scene_depth in
// pass: sky_background"). Over RPC, through the running renderer: the catalog
// says scene_depth needs a writer, the edit is refused by name with the
// material pass's depth port as the writer, and the one edit Studio makes of
// it -- that port connected to scene_depth_ref -- lets sky_ambient publish.
TEST_CASE("FR-3 sky_ambient on the Blank-built deferred pipeline names the material pass's depth as its writer, "
          "and connecting it publishes, through the running renderer",
          "[fr3][primitives][render-feature][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("fr3_unwritten");
    const auto root = fr3BlankDeferredProject(dir.root);
    const auto rendering_path = root / "passes/main_rendering_config.json";
    Fr3Engine engine{root};

    const auto catalog = engine.rpc.result("list_render_features").at("features");
    const auto sky = std::find_if(catalog.begin(), catalog.end(),
                                  [](const Json &feature) { return feature.at("name") == "sky_ambient"; });
    REQUIRE(sky != catalog.end());
    INFO(sky->dump(2));
    REQUIRE(sky->at("required_targets").size() == 2);
    CHECK(sky->at("required_targets").at(0).at("needs_writer") == false);
    CHECK(sky->at("required_targets").at(1).at("name") == "scene_depth");
    CHECK(sky->at("required_targets").at(1).at("needs_writer") == true);

    // What Studio's "Add with targets" adds (UX-12b), in one edit_batch.
    const auto declared = engine.batch(Json::array({
        {{"method", "edit_render_targets"},
         {"params", {{"ops", Json::array({
             {{"op", "add"}, {"target", {{"name", "lit_color"}, {"extent_scale", 1.0},
                                         {"format", "R16G16B16A16_SFLOAT"}, {"format_class", "scene"},
                                         {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}}}},
             {{"op", "add"}, {"target", {{"name", "scene_depth"}, {"extent_scale", 1.0},
                                         {"format", "D32_SFLOAT"}, {"format_class", "data"},
                                         {"usage", Json::array({"DEPTH_STENCIL_ATTACHMENT", "SAMPLED"})}}}}})}}}},
        {{"method", "edit_graph_document"},
         {"params", {{"ops", Json::array({
             {{"op", "add_node"}, {"node", {{"id", "scene_depth_ref"}, {"op", "render.target_ref"},
                                            {"outputs", Json::array({"resource"})},
                                            {"parameters", {{"name", "scene_depth"}, {"type", fr3Depth}}}}}},
             {{"op", "add_node"}, {"node", {{"id", "lit_color_ref"}, {"op", "render.target_ref"},
                                            {"outputs", Json::array({"resource"})},
                                            {"parameters", {{"name", "lit_color"},
                                                            {"type", {{"semantic", "pelican.render.legacy_opaque_image@1"},
                                                                      {"arguments", Json::object()}}}}}}}}})}}}}}));
    INFO(declared.dump(2));
    REQUIRE(declared.at("status") == "committed");
    CHECK(declared.at("operation") == "edit_batch");

    const auto before = readFile(rendering_path);
    const auto refused = engine.send("edit_render_features", fr3SkyParams());
    INFO(refused.dump(2));
    REQUIRE(refused.at("status") == "rejected");
    const auto &error = refused.at("error");
    CHECK(error.at("code") == "feature.required_target_unwritten@1");
    CHECK(error.at("feature") == "sky_ambient");
    CHECK(error.at("target") == "scene_depth");
    CHECK(error.at("type") == fr3Depth);
    CHECK(error.at("document") == "engine://features/sky_ambient.json");
    CHECK(error.at("writer_candidates") == Json::parse(R"([{"node": "render_material_pass",
        "pass": "render_material_pass", "port": "depth", "resource": "render_material_pass__depth"}])"));
    CHECK(readFile(rendering_path) == before);

    const auto &candidate = error.at("writer_candidates").at(0);
    const auto connected = engine.send(
        "edit_graph_document",
        {{"ops", Json::array({{{"op", "set_input"}, {"node", candidate.at("node")}, {"port", candidate.at("port")},
                               {"source", {{"node", "scene_depth_ref"}, {"port", "resource"}}}}})}});
    INFO(connected.dump(2));
    REQUIRE(connected.at("status") == "committed");
    const auto added = engine.send("edit_render_features", fr3SkyParams());
    INFO(added.dump(2));
    REQUIRE(added.at("status") == "committed");

    (void)engine.rpc.result("render_frame");
    const auto plan = engine.rpc.result("get_frame_plan");
    const auto *material = fr3PlanNode(plan, "render_material_pass");
    const auto *background = fr3PlanNode(plan, "sky_background");
    INFO(plan.dump());
    REQUIRE(material != nullptr);
    REQUIRE(background != nullptr);
    CHECK(fr3Lists(material->at("writes"), "scene_depth"));
    CHECK(fr3Lists(background->at("reads"), "scene_depth"));
}

// FR-3 (UX-12b): cascade_count 3 and the 2D-array shadow_map_ref were each
// refused alone, in either order. Over RPC, through the running renderer:
// with the two in one edit_batch, what is left is the lighting pass's 2D
// depth port, named with its edit's index; with the port in the same graph
// edit the batch publishes as one ticket, and RD-1 takes it back whole.
TEST_CASE("FR-3 edit_batch takes cascade_count 3 with the 2D array shadow map types in one ticket, through the "
          "running renderer",
          "[fr3][primitives][edit-batch][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("fr3_batch");
    const auto root = fr3BlankDeferredProject(dir.root);
    const auto rendering_path = root / "passes/main_rendering_config.json";
    Fr3Engine engine{root};
    const auto before = readFile(rendering_path);

    const Json cascade{{"method", "edit_render_features"}, {"params", fr3CascadeParams()}};
    const Json ref_op{{"op", "set_node_parameters"}, {"node", "shadow_map_ref"},
                      {"parameters", {{"name", "shadow_map"}, {"type", fr3ArrayDepth}}}};
    Json lighting;
    const auto authored = Json::parse(before);
    for (const auto &node : authored.at("nodes"))
        if (node.at("id") == "deferred_lighting") lighting = node.at("parameters");
    REQUIRE(lighting.is_object());
    lighting["ports"]["shadow_map"]["type"] = fr3ArrayDepth;
    const Json port_op{{"op", "set_node_parameters"}, {"node", "deferred_lighting"}, {"parameters", lighting}};

    const auto alone = engine.send("edit_render_features", fr3CascadeParams());
    INFO(alone.dump(2));
    REQUIRE(alone.at("status") == "rejected");
    CHECK(alone.at("error").at("code") == "graph.target_ref_type_mismatch@1");

    const auto left = engine.batch(Json::array(
        {cascade, {{"method", "edit_graph_document"}, {"params", {{"ops", Json::array({ref_op})}}}}}));
    INFO(left.dump(2));
    REQUIRE(left.at("status") == "rejected");
    CHECK(left.at("error").at("batch_index") == 1);
    CHECK_THAT(left.at("error").at("message").get<std::string>(),
               Catch::Matchers::ContainsSubstring("deferred_lighting.shadow_map from shadow_map_ref.resource"));
    CHECK(readFile(rendering_path) == before);

    const auto documents = engine.rpc.result("get_render_documents").at("documents");
    REQUIRE(documents.size() == 1);
    const auto committed = engine.batch(Json::array(
        {cascade, {{"method", "edit_graph_document"}, {"params", {{"ops", Json::array({ref_op, port_op})}}}}}));
    INFO(committed.dump(2));
    REQUIRE(committed.at("status") == "committed");
    CHECK(committed.at("operation") == "edit_batch");
    CHECK(committed.at("methods") == Json::array({"edit_render_features", "edit_graph_document"}));
    REQUIRE(committed.at("documents").size() == 1);
    (void)engine.rpc.result("render_frame");
    const auto plan = engine.rpc.result("get_frame_plan");
    INFO(plan.dump());
    const auto *shadow = fr3PlanNode(plan, "shadow_depth");
    const auto *lighting_node = fr3PlanNode(plan, "deferred_lighting");
    REQUIRE(shadow != nullptr);
    REQUIRE(lighting_node != nullptr);
    CHECK(fr3Lists(lighting_node->at("reads"), "shadow_map"));

    const auto restored = engine.settle(engine.rpc.result(
        "restore_render_documents",
        {{"documents", Json::array({{{"reference", documents.at(0).at("reference")},
                                     {"base_digest", committed.at("documents").begin().value()},
                                     {"content", documents.at(0).at("content")}}})}}));
    INFO(restored.dump(2));
    REQUIRE(restored.at("status") == "committed");
    CHECK(readFile(rendering_path) == before);

    // A method edit_batch does not take is the caller's error.
    const auto outside = engine.rpc.call(
        "edit_batch", {{"base_source_digest", engine.digest()},
                       {"edits", Json::array({{{"method", "verify_fold"}, {"params", Json::object()}}})}});
    INFO(outside.dump(2));
    REQUIRE(outside.contains("error"));
    CHECK(outside.at("error").at("code") == -32602);
}

// FR-4 (UX-12d): on that pipeline with sky_ambient, the sky reached present
// only with two tickets and two undos -- the lighting pass's color into
// lit_color (edit_graph_document) and present reading lit_color
// (edit_preset_pass). Over RPC, through the running renderer: the catalog
// gives sky_ambient's lit_color the lighting pass's color port's type, so
// lit_color_ref connects to that port as the catalog declares it; the two
// edits in one edit_batch publish as one ticket, and one RD-1 restore takes
// both back.
TEST_CASE("FR-4 edit_batch puts the sky on present with edit_graph_document and edit_preset_pass in one ticket, "
          "through the running renderer",
          "[fr4][primitives][render-feature][edit-batch][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("fr4_sky_present");
    const auto root = fr3BlankDeferredProject(dir.root);
    const auto rendering_path = root / "passes/main_rendering_config.json";
    Fr3Engine engine{root};

    const auto catalog = engine.rpc.result("list_render_features").at("features");
    const auto sky = std::find_if(catalog.begin(), catalog.end(),
                                  [](const Json &feature) { return feature.at("name") == "sky_ambient"; });
    REQUIRE(sky != catalog.end());
    INFO(sky->dump(2));
    const auto &lit = sky->at("required_targets").at(0);
    REQUIRE(lit.at("name") == "lit_color");
    const auto lit_type = lit.at("type");
    CHECK(lit_type.at("semantic") == "pelican.render.color_signal@1");
    const auto authored = Json::parse(readFile(rendering_path));
    for (const auto &node : authored.at("nodes"))
        if (node.at("id") == "deferred_lighting")
            CHECK(node.at("parameters").at("ports").at("color").at("type") == lit_type);

    // sky_ambient as Studio adds it (FR-3), lit_color_ref of the catalog's type.
    const auto declared = engine.batch(Json::array({
        {{"method", "edit_render_targets"},
         {"params", {{"ops", Json::array({
             {{"op", "add"}, {"target", {{"name", "lit_color"}, {"extent_scale", 1.0},
                                         {"format", "R16G16B16A16_SFLOAT"}, {"format_class", "scene"},
                                         {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}}}},
             {{"op", "add"}, {"target", {{"name", "scene_depth"}, {"extent_scale", 1.0},
                                         {"format", "D32_SFLOAT"}, {"format_class", "data"},
                                         {"usage", Json::array({"DEPTH_STENCIL_ATTACHMENT", "SAMPLED"})}}}}})}}}},
        {{"method", "edit_graph_document"},
         {"params", {{"ops", Json::array({
             {{"op", "add_node"}, {"node", {{"id", "scene_depth_ref"}, {"op", "render.target_ref"},
                                            {"outputs", Json::array({"resource"})},
                                            {"parameters", {{"name", "scene_depth"}, {"type", fr3Depth}}}}}},
             {{"op", "add_node"}, {"node", {{"id", "lit_color_ref"}, {"op", "render.target_ref"},
                                            {"outputs", Json::array({"resource"})},
                                            {"parameters", {{"name", "lit_color"}, {"type", lit_type}}}}}}})}}}}}));
    INFO(declared.dump(2));
    REQUIRE(declared.at("status") == "committed");
    const auto connected = engine.send(
        "edit_graph_document",
        {{"ops", Json::array({{{"op", "set_input"}, {"node", "render_material_pass"}, {"port", "depth"},
                               {"source", {{"node", "scene_depth_ref"}, {"port", "resource"}}}}})}});
    INFO(connected.dump(2));
    REQUIRE(connected.at("status") == "committed");
    const auto added = engine.send("edit_render_features", fr3SkyParams());
    INFO(added.dump(2));
    REQUIRE(added.at("status") == "committed");

    const auto before = readFile(rendering_path);
    // The root and the render-target fragment edit_render_targets wrote.
    const auto documents = engine.rpc.result("get_render_documents").at("documents");
    const auto committed = engine.batch(Json::array({
        {{"method", "edit_graph_document"},
         {"params", {{"ops", Json::array({{{"op", "set_input"}, {"node", "deferred_lighting"}, {"port", "color"},
                                           {"source", {{"node", "lit_color_ref"}, {"port", "resource"}}}}})}}}},
        {{"method", "edit_preset_pass"},
         {"params", {{"graph", "main_render"}, {"pass_name", "present"}, {"input", Json::array({"lit_color"})},
                     {"output", {{"color", Json::array({"display"})}, {"depth", nullptr}}},
                     {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}}}}}}));
    INFO(committed.dump(2));
    REQUIRE(committed.at("status") == "committed");
    CHECK(committed.at("operation") == "edit_batch");
    CHECK(committed.at("methods") == Json::array({"edit_graph_document", "edit_preset_pass"}));
    REQUIRE(committed.at("documents").size() == 1);
    (void)engine.rpc.result("render_frame");
    {
        const auto plan = engine.rpc.result("get_frame_plan");
        INFO(plan.dump());
        const auto *lighting = fr3PlanNode(plan, "deferred_lighting");
        const auto *background = fr3PlanNode(plan, "sky_background");
        const auto *present = fr3PlanNode(plan, "present");
        REQUIRE(lighting != nullptr);
        REQUIRE(background != nullptr);
        REQUIRE(present != nullptr);
        CHECK(fr3Lists(lighting->at("writes"), "lit_color"));
        CHECK(fr3Lists(background->at("writes"), "lit_color"));
        CHECK(present->at("reads") == Json::array({"lit_color"}));
    }

    // RD-1: the two edits are one undo -- every document the batch wrote (the
    // root alone), back as it was read before it.
    auto restore = Json::array();
    for (const auto &written : committed.at("documents").items()) {
        const auto reference = written.key();
        const auto read = std::find_if(documents.begin(), documents.end(),
                                       [&](const Json &document) { return document.at("reference") == reference; });
        REQUIRE(read != documents.end());
        restore.push_back(
            {{"reference", reference}, {"base_digest", written.value()}, {"content", read->at("content")}});
    }
    const auto restored = engine.settle(engine.rpc.result("restore_render_documents", {{"documents", restore}}));
    INFO(restored.dump(2));
    REQUIRE(restored.at("status") == "committed");
    CHECK(readFile(rendering_path) == before);
    (void)engine.rpc.result("render_frame");
    const auto plan = engine.rpc.result("get_frame_plan");
    const auto *present = fr3PlanNode(plan, "present");
    REQUIRE(present != nullptr);
    CHECK(present->at("reads") == Json::array({"deferred_lighting__color"}));
}


namespace {

// PV-1: test/fixtures/pv1_history_accum copied out -- a direct root whose
// authored target `accum` keeps history, added to every frame.
std::filesystem::path pv1HistoryProject(const std::filesystem::path &dir) {
    const auto fixtures = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" / "pv1_history_accum";
    const auto root = dir / "project";
    std::filesystem::create_directories(root);
    std::filesystem::copy(fixtures, root, std::filesystem::copy_options::recursive);
    return root;
}

// render_preview of a small PNG from an explicit camera, the response as the
// wire carries it (a result or an error).
Json pv1RenderPreview(Rpc &rpc, std::uint64_t graph_generation) {
    return rpc.call("render_preview",
                    {{"capture",
                      {{"width", 16},
                       {"height", 16},
                       {"pixel_encoding", "png"},
                       {"camera",
                        {{"position", {0.0, 0.0, -3.0}},
                         {"target", {0.0, 0.0, 0.0}},
                         {"up", {0.0, 1.0, 0.0}},
                         {"projection",
                          {{"kind", "perspective"}, {"yfov", 0.8}, {"znear", 0.1}, {"zfar", 100.0}}}}},
                       {"graph_generation", graph_generation},
                       {"max_bytes", 1 << 20}}}});
}

} // namespace

// PV-1: a project authoring a history target starts and plays; only the
// one-shot preview is unavailable, said in get_status / get_frame_plan, and
// render_preview refuses by name. An edit that removes the history (an RL-1
// reload of the rewritten root) makes preview available and render_preview
// renders; edits that add a history target and read it (edit_batch, then
// edit_preset_pass) are committed, not refused, and preview is unavailable
// again with the new reason.
TEST_CASE("PV-1 an authored history target plays with the one-shot preview unavailable, and edits move preview "
          "both ways without refusing",
          "[pv1][preview][render-preview][rl1][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("pv1_history");
    const auto root = pv1HistoryProject(dir.root);
    const auto rendering_path = root / "passes/main.json";
    const auto capture = dir.root / "capture.png";
    Fr3Engine engine{root};
    auto &rpc = engine.rpc;

    // History works: each frame adds to the last.
    const auto first = rpc.frame(capture);
    (void)rpc.result("step_frame");
    const auto second = rpc.frame(capture);
    CHECK(differingPixels(first, second) > 0);

    const auto history_unavailable = [](const Json &preview) {
        INFO(preview.dump(2));
        REQUIRE(preview.at("available") == false);
        const auto &reason = preview.at("reason");
        CHECK(reason.at("code") == "preview.unsupported_authoring@1");
        CHECK(reason.at("feature") == "history");
        CHECK(reason.at("target") == "accum");
        CHECK(reason.at("message") == "preview graph '<config>' rejects authored history target 'accum'");
    };
    history_unavailable(rpc.result("get_status").at("preview"));
    history_unavailable(rpc.result("get_frame_plan").at("preview"));
    {
        const auto refused = pv1RenderPreview(rpc, 0);
        INFO(refused.dump(2));
        REQUIRE(refused.contains("error"));
        const auto &data = refused.at("error").at("data");
        CHECK(data.at("code") == "preview.unavailable@1");
        CHECK(data.at("method") == "render_preview");
        CHECK(data.at("reason").at("feature") == "history");
        CHECK(data.at("reason").at("target") == "accum");
    }

    // The root rewritten without history and reloaded (RL-1): committed with
    // preview available, and render_preview renders.
    {
        auto rendering = Json::parse(readFile(rendering_path));
        auto &section = rendering.at("rendering");
        section.at("render_targets").at(0).erase("history");
        section.at("rendering_passes").at(0).at("passes").at(0).at("input") = Json::array();
        writeFile(rendering_path, rendering.dump(2));
    }
    const Json available{{"available", true}};
    {
        const auto reloaded = engine.settle(rpc.result("reload_render_documents"));
        INFO(reloaded.dump(2));
        REQUIRE(reloaded.at("status") == "committed");
        CHECK(reloaded.at("preview") == available);
    }
    CHECK(rpc.result("get_status").at("preview") == available);
    CHECK(rpc.result("get_frame_plan").at("preview") == available);
    {
        const auto generation = rpc.result("eval_preview").at("graph_generation").get<std::uint64_t>();
        CHECK(generation != 0);
        const auto rendered = pv1RenderPreview(rpc, generation);
        if (rendered.contains("error")) FAIL(rendered.at("error").dump(2));
        CHECK(rendered.at("result").at("status") == "rendered");
    }

    // A history target added through edit_batch: it is declared in the
    // editor's render target fragment, which preview leaves out, so preview
    // stays available.
    {
        const auto added = engine.batch(Json::array({
            {{"method", "edit_render_targets"},
             {"params",
              {{"ops", Json::array({{{"op", "add"},
                                     {"target",
                                      {{"name", "trail"},
                                       {"extent_scale", 1.0},
                                       {"format", "R16G16B16A16_SFLOAT"},
                                       {"format_class", "explicit(R16G16B16A16_SFLOAT)"},
                                       {"role", "color"},
                                       {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
                                       {"history", true},
                                       {"clear_color", {0.0, 0.0, 0.0, 0.0}}}}}})}}}},
        }));
        INFO(added.dump(2));
        REQUIRE(added.at("status") == "committed");
        CHECK(added.at("preview") == available);
    }
    // present reading last frame's trail is what preview cannot take: the
    // edit is committed (it was refused before PV-1) and preview is
    // unavailable, named by that pass.
    {
        const auto read = engine.send("edit_preset_pass",
                                      {{"graph", "main_render"},
                                       {"pass_name", "present"},
                                       {"input", Json::array({"accum", "trail@history"})},
                                       {"output", q14e_present_output},
                                       {"shader",
                                        {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}}});
        INFO(read.dump(2));
        REQUIRE(read.at("status") == "committed");
        const auto &preview = read.at("preview");
        REQUIRE(preview.at("available") == false);
        CHECK(preview.at("reason").at("code") == "preview.unsupported_authoring@1");
        CHECK(preview.at("reason").at("feature") == "history_read");
        CHECK(preview.at("reason").at("graph") == "main_render");
        CHECK(preview.at("reason").at("pass") == "present");
        CHECK(rpc.result("get_status").at("preview") == preview);
    }
    CHECK(rpc.result("get_frame_plan").at("preview").at("available") == false);
    {
        const auto refused = pv1RenderPreview(rpc, 0);
        REQUIRE(refused.contains("error"));
        CHECK(refused.at("error").at("data").at("code") == "preview.unavailable@1");
        CHECK(refused.at("error").at("data").at("reason").at("feature") == "history_read");
    }
    (void)rpc.frame(capture);
}

} // namespace Pelican
