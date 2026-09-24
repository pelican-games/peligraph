#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/asset/model.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/material/projectmaterialasset.hpp"
#include "../src/core/material/materialcontainer.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/engineprimitives.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/project/projecttemplate.hpp"
#include "../src/project/renderconfigsection.hpp"
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
              R"json({"schema":"pelican.asset_data","version":1,"models":[{"name":"red_floor","path":"engine://primitives/plane","material_bindings":"materials/floor.bindings.json"}],"materials":[{"path":"materials/red.material.json"}]})json");

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
              wrapLegacyRenderingConfig(rendering).dump(2));
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

} // namespace Pelican
