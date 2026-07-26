#include "../src/core/container.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/ecs/predefined/modelview.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/material/materialcontainer.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/gltf.hpp"
#include "../src/core/asset/model.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/project/importmanifest.hpp"
#include "../src/project/materialformat.hpp"
#include "../src/project/sceneformat.hpp"
#include "gltf_fragment_fixture.hpp"
#include "vat_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {

namespace {

AssetFragmentRef fragment(std::string kind, std::string path) {
    return AssetFragmentRef{
        .kind = std::move(kind),
        .path = path,
        .address_kind = path.find('/') == std::string::npos
                            ? AssetFragmentAddressKind::name
                            : AssetFragmentAddressKind::full_path,
    };
}

size_t primitiveCount(const ModelTemplate &model) {
    size_t count = 0;
    for (const auto &group : model.material_primitives) {
        count += group.primitives.size();
    }
    return count;
}

std::filesystem::path usdFixtureRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" / "usd0b";
}

std::filesystem::path usdMaterialFixtureRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" / "usd0c" /
           "root_materials_coat";
}

nlohmann::json readJson(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    if (!file.is_open()) throw std::runtime_error("failed to open fixture: " + path.string());
    return nlohmann::json::parse(file);
}

void writeText(const std::filesystem::path &path, std::string_view text) {
    std::ofstream file{path, std::ios::binary};
    file << text;
}

struct TempDirGuard {
    std::filesystem::path path;
    ~TempDirGuard() { std::filesystem::remove_all(path); }
};

} // namespace

TEST_CASE("glTF fragments load only the selected object and dependencies", "[gltf][fragment]") {
    setupLogger();
    const auto temp_dir = std::filesystem::temp_directory_path() /
                          ("pelican_wp77_" + std::to_string(
                               std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(temp_dir);
    const TempDirGuard temp_guard{temp_dir};
    const auto glb_path = temp_dir / "fragment.glb";
    const auto duplicate_path = temp_dir / "duplicate.glb";
    const auto normalized_path =
        temp_dir / "normalized.glb";
    TestGltfFragmentFixture::writeGlb(glb_path);
    TestGltfFragmentFixture::writeGlb(duplicate_path, true);
    TestGltfFragmentFixture::
        writeNormalizedPositionGlb(normalized_path);
    writeText(temp_dir / "scene.json", R"json({
        "schema":"pelican.scene",
        "version":1,
        "scenes":{"default_scene":{"objects":[{
            "name":"fragment_object",
            "components":[
                {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
                {"name":"simplemodelview","model":"selected"}
            ]
        }]}}
    })json");
    writeText(temp_dir / "assets.json",
              R"json({"models":[{"name":"selected","path":"fragment.glb#mesh/MeshA"}]})json");

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(temp_dir, false);
    GET_MODULE(ProjectSource).setSourceByData(nlohmann::json{
        {"basic_config",
         {
             {"scene_data_json", "scene.json"},
             {"asset_data_json", "assets.json"},
         }},
    }.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};

    try {
        (void)GET_MODULE(StandardMaterialResource);
    } catch (const std::exception &ex) {
        SKIP(std::string{"Vulkan headless rendering unavailable: "} + ex.what());
    }

    auto &materials = GET_MODULE(MaterialContainer);
    auto &loader = GET_MODULE(GltfLoader);
    const auto texture_count = materials.textureCountForTesting();
    const auto material_count = materials.materialCountForTesting();

    const auto selected = loader.loadGltfBinary(glb_path.string(), fragment("mesh", "MeshA"));
    REQUIRE(primitiveCount(selected) == 1);
    REQUIRE(selected.material_primitives.size() == 1);
    REQUIRE(selected.material_primitives.front().source_material_index == 0);
    REQUIRE(selected.material_initial_values != nullptr);
    REQUIRE(selected.material_initial_values->values.size() == 2);
    REQUIRE(selected.material_initial_values->values.at(0).base_color_factor ==
            glm::vec4{1.0f, 0.0f, 0.0f, 1.0f});
    REQUIRE(selected.material_initial_values->values.at(0).emissive_factor ==
            glm::vec4{0.0f, 0.0f, 0.0f, 1.0f});
    REQUIRE(selected.material_initial_values->values.at(1).base_color_factor ==
            glm::vec4{0.0f, 1.0f, 0.0f, 1.0f});
    REQUIRE(selected.material_initial_values->values.at(0).uv_offset ==
            glm::vec2{0.125f, -0.25f});
    REQUIRE(selected.material_initial_values->values.at(0).uv_scale ==
            glm::vec2{2.0f, 0.5f});
    REQUIRE(selected.material_initial_values->values.at(0).uv_rotation ==
            0.25f);
    REQUIRE(materials.textureCountForTesting() == texture_count + 2);
    REQUIRE(materials.materialCountForTesting() == material_count + 1);

    const auto full_path =
        loader.loadGltfBinary(glb_path.string(), fragment("mesh", "RootB/Cube"));
    REQUIRE(primitiveCount(full_path) == 1);

    const auto subtree = loader.loadGltfBinary(glb_path.string(), fragment("node", "RootA"));
    REQUIRE(primitiveCount(subtree) == 1);

    const auto material_only =
        loader.loadGltfBinary(glb_path.string(), fragment("material", "MatB"));
    REQUIRE(material_only.material_primitives.size() == 1);
    REQUIRE(material_only.material_primitives.front().primitives.empty());

    const auto animation_only =
        loader.loadGltfBinary(glb_path.string(), fragment("animation", "Walk"));
    REQUIRE(animation_only.material_primitives.empty());

    const auto whole = loader.loadGltfBinary(glb_path.string());
    REQUIRE(primitiveCount(whole) == 2);
    REQUIRE(std::any_of(whole.material_primitives.begin(),
                        whole.material_primitives.end(), [](const auto &group) {
                            return group.source_material_index == 0;
                        }));
    REQUIRE(std::any_of(whole.material_primitives.begin(),
                        whole.material_primitives.end(), [](const auto &group) {
                            return group.source_material_index == 1;
                        }));

    const auto normalized =
        loader.loadGltfBinary(
            normalized_path.string());
    REQUIRE(primitiveCount(normalized) == 1);
    const auto &normalized_bounds =
        normalized.material_primitives.front()
            .primitives.front()
            .bounds_source->base;
    REQUIRE(
        normalized_bounds.minimum ==
        glm::vec3{
            -1.0F, -64.0F / 127.0F, 0.0F});
    REQUIRE(
        normalized_bounds.maximum ==
        glm::vec3{1.0F, 1.0F, 0.0F});

#if PELICAN_WITH_VAT
    const auto vat_path = temp_dir / "bounds_vat.glb";
    TestVatFixture::writeTinyVatGlb(vat_path);
    const auto vat = loader.loadGltfBinary(vat_path.string());
    REQUIRE(primitiveCount(vat) == 1);
    REQUIRE(vat.material_primitives.size() == 1);
    REQUIRE(vat.material_primitives.front().primitives.size() == 1);
    const auto &vat_primitive =
        vat.material_primitives.front().primitives.front();
    REQUIRE(vat_primitive.bounds_source != nullptr);
    REQUIRE(vat_primitive.bounds_source->base.minimum ==
            glm::vec3{-0.8F, -0.4F, -0.1F});
    REQUIRE(vat_primitive.bounds_source->base.maximum ==
            glm::vec3{0.8F, 0.4F, 0.1F});
    REQUIRE(vat_primitive.bounds_source->morph_position_deltas.empty());
#endif

    REQUIRE(primitiveCount(GET_MODULE(ModelAssetContainer).getModelTemplateByName("selected")) == 1);
    GET_MODULE(ECSPredefinedRegistration).reg();
    GET_MODULE(SceneLoader).load("default_scene");
    GET_MODULE(ECSCore).update();
    const auto scene_object = GET_MODULE(SceneLoader).objectId("fragment_object");
    REQUIRE(scene_object);
    const auto *scene_model = GET_MODULE(ECSCore)
                                  .getTemplatePublicModule()
                                  .tryComponent<SimpleModelViewComponent>(*scene_object);
    REQUIRE(scene_model != nullptr);
    REQUIRE(scene_model->model_instance_id);

    REQUIRE_THROWS_WITH(loader.loadGltfBinary(glb_path.string(), fragment("mesh", "Cube")),
                        Catch::Matchers::ContainsSubstring("RootA/Cube") &&
                            Catch::Matchers::ContainsSubstring("RootB/Cube"));
    REQUIRE_THROWS_WITH(loader.loadGltfBinary(glb_path.string(), fragment("mesh", "Missing")),
                        Catch::Matchers::ContainsSubstring("Unknown GLB mesh fragment 'Missing'"));
    REQUIRE_THROWS_WITH(loader.loadGltfBinary(glb_path.string(), fragment("camera", "Main")),
                        Catch::Matchers::ContainsSubstring("Unknown GLB fragment kind 'camera'"));
    REQUIRE_THROWS_WITH(loader.loadGltfBinary(duplicate_path.string(), fragment("node", "RootA")),
                        Catch::Matchers::ContainsSubstring("Duplicate GLB node full path 'RootA'"));
    REQUIRE_THROWS_WITH(loader.loadGltfBinary((temp_dir / "not_glb.png").string(),
                                              fragment("mesh", "Cube")),
                        Catch::Matchers::ContainsSubstring("requires a .glb file"));

    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("U-USD0b corpus deliveries parse, load, and instantiate their scene fragment",
          "[gltf][fragment][usd0b]") {
    setupLogger();
    const auto fixture_root = usdFixtureRoot();
    const std::array fixture_names{
        "root_yup_m_usda",
        "root_zup_cm_usda",
        "root_zup_cm_usdz",
    };
    for (const auto *name : fixture_names) {
        CAPTURE(name);
        const auto delivery = fixture_root / name;
        const auto manifest = parseImportManifestJson(readJson(delivery / "manifest.json"));
        REQUIRE(manifest.outputs.size() == 2);
        REQUIRE(manifest.outputs[0].schema == "gltf");
        REQUIRE(manifest.outputs[1].schema == "pelican.scene");
        const auto normalized_scene = normalizeSceneDataJson(readJson(delivery / "scene.json"));
        REQUIRE(normalized_scene.scenes.at("default_scene").at("objects").is_array());
        REQUIRE_FALSE(normalized_scene.scenes.at("default_scene").at("objects").empty());
    }

    const auto zup_manifest = readJson(fixture_root / "root_zup_cm_usda" / "manifest.json");
    const auto &mapping = zup_manifest.at("source").at("geometry_mapping");
    REQUIRE(mapping.size() == 3);
    REQUIRE(mapping.at(0).at("prim_path") == "/World/Hierarchy/FeatureMesh");
    REQUIRE(mapping.at(0).at("primitive_mapping").size() == 2);
    REQUIRE(mapping.at(0).at("primitive_mapping").at(0).at("subset_path") ==
            "/World/Hierarchy/FeatureMesh/RedFaces");
    REQUIRE(mapping.at(0).at("primitive_mapping").at(0).at("glb_primitive_index") == 0);
    REQUIRE(mapping.at(0).at("primitive_mapping").at(1).at("subset_path") ==
            "/World/Hierarchy/FeatureMesh/TexturedFaces");
    REQUIRE(mapping.at(0).at("primitive_mapping").at(1).at("glb_primitive_index") == 1);

    const auto temp_dir = std::filesystem::temp_directory_path() /
                          ("pelican_wp119_" + std::to_string(
                               std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(temp_dir);
    const TempDirGuard temp_guard{temp_dir};
    const auto yup = fixture_root / "root_yup_m_usda";
    std::filesystem::copy_file(yup / "model.glb", temp_dir / "model.glb");
    std::filesystem::copy_file(yup / "scene.json", temp_dir / "scene.json");
    writeText(temp_dir / "assets.json", R"json({"models":[]})json");

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(temp_dir, false);
    GET_MODULE(ProjectSource).setSourceByData(nlohmann::json{
        {"basic_config",
         {
             {"scene_data_json", "scene.json"},
             {"asset_data_json", "assets.json"},
             {"default_scene_id", "default_scene"},
         }},
    }.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};

    try {
        (void)GET_MODULE(StandardMaterialResource);
    } catch (const std::exception &ex) {
        SKIP(std::string{"Vulkan headless rendering unavailable: "} + ex.what());
    }

    auto &loader = GET_MODULE(GltfLoader);
    REQUIRE(primitiveCount(loader.loadGltfBinary((yup / "model.glb").string())) == 1);
    REQUIRE(primitiveCount(loader.loadGltfBinary(
                (fixture_root / "root_zup_cm_usda" / "model.glb").string())) == 4);
    REQUIRE(primitiveCount(loader.loadGltfBinary(
                (fixture_root / "root_zup_cm_usdz" / "model.glb").string())) == 4);

    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &scene = GET_MODULE(SceneLoader);
    scene.load("default_scene");
    GET_MODULE(ECSCore).update();
    const auto object = scene.objectId("YWorld_YUpMeterMesh");
    REQUIRE(object);
    const auto *model = GET_MODULE(ECSCore)
                            .getTemplatePublicModule()
                            .tryComponent<SimpleModelViewComponent>(*object);
    REQUIRE(model != nullptr);
    REQUIRE(model->model_instance_id);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("U-USD0c generated delivery parses material and binding then loads the GLB and scene",
          "[gltf][fragment][usd0c][openpbr]") {
    setupLogger();
    const auto delivery = usdMaterialFixtureRoot();
    const auto manifest = parseImportManifestJson(readJson(delivery / "manifest.json"));
    REQUIRE(manifest.outputs.size() == 4);
    REQUIRE(manifest.outputs.at(0).schema == "pelican.material");
    REQUIRE(manifest.outputs.at(1).schema == "gltf");
    REQUIRE(manifest.outputs.at(2).schema == "pelican.scene");
    REQUIRE(manifest.outputs.at(3).schema == "png");

    const auto surface_reference =
        std::string{"engine://surfaces/openpbr/opaque_double.surface"};
    MaterialSurfaceCatalog surfaces;
    surfaces.emplace(
        surface_reference,
        parseSurfaceFormat(engineResourceOrThrow("surfaces/openpbr/opaque_double.surface"),
                           surface_reference));
    const auto material =
        parseMaterialFormatJson(readJson(delivery / "materials.json"), surfaces);
    REQUIRE(material.materials.size() == 1);
    REQUIRE(material.materials.front().name == "World_Coat_opaque_double");
    REQUIRE(material.materials.front().routing.has_value());
    REQUIRE(material.materials.front().routing->double_sided);
    REQUIRE(material.materials.front().texture_overrides.size() == 1);
    REQUIRE(material.materials.front().texture_overrides.front().name ==
            "base_diffuse_roughness_map");

    const auto bindings =
        parsePrimitiveMaterialBindingJson(readJson(delivery / "material_bindings.json"));
    REQUIRE(bindings.model == "project://model.glb");
    REQUIRE(bindings.bindings.size() == 1);
    REQUIRE(bindings.bindings.front().usd_path == "/World/CoatTriangle");
    REQUIRE(bindings.bindings.front().material == material.materials.front().name);

    const auto normalized_scene = normalizeSceneDataJson(readJson(delivery / "scene.json"));
    REQUIRE(normalized_scene.scenes.at("default_scene").at("objects").size() == 1);

    const auto temp_dir = std::filesystem::temp_directory_path() /
                          ("pelican_wp124_" + std::to_string(
                               std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(temp_dir);
    const TempDirGuard temp_guard{temp_dir};
    std::filesystem::copy(delivery, temp_dir,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing);
    writeText(temp_dir / "assets.json", R"json({"models":[]})json");

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(temp_dir, false);
    GET_MODULE(ProjectSource).setSourceByData(nlohmann::json{
        {"basic_config",
         {
             {"scene_data_json", "scene.json"},
             {"asset_data_json", "assets.json"},
             {"default_scene_id", "default_scene"},
         }},
    }.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    try {
        (void)GET_MODULE(StandardMaterialResource);
    } catch (const std::exception &ex) {
        SKIP(std::string{"Vulkan headless rendering unavailable: "} + ex.what());
    }

    auto model = GET_MODULE(GltfLoader).loadGltfBinary((delivery / "model.glb").string());
    REQUIRE(primitiveCount(model) == 1);
    applyPrimitiveMaterialBindings(model, bindings, "wp124_generated_coat");
    REQUIRE(primitiveCount(model) == 1);

    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &scene = GET_MODULE(SceneLoader);
    scene.load("default_scene");
    GET_MODULE(ECSCore).update();
    REQUIRE(scene.objectId("World_CoatTriangle"));
    GET_MODULE(VulkanManageCore).waitIdle();
}

} // namespace Pelican
