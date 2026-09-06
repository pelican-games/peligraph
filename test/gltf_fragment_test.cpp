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
#include "scene_load_test_support.hpp"
#include "../src/core/material/materialcontainer.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/gltf.hpp"
#include "../src/core/asset/model.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/project/importmanifest.hpp"
#include "../src/project/materialformat.hpp"
#include "../src/project/sceneformat.hpp"
#include "gltf_fragment_fixture.hpp"
#include "morph_fixture.hpp"
#include "skeletal_fixture.hpp"
#include "vulkan_test_support.hpp"
#include "vat_fixture.hpp"
#include "raytracing_rt3_1_fixture.hpp"
#include "../src/core/renderer/polygoninstancecontainer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <nlohmann/json.hpp>
#include <set>

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

std::uint32_t fixtureU32(const std::vector<std::uint8_t> &bytes,
                         std::size_t offset) {
    return static_cast<std::uint32_t>(bytes.at(offset)) |
           (static_cast<std::uint32_t>(bytes.at(offset + 1)) << 8) |
           (static_cast<std::uint32_t>(bytes.at(offset + 2)) << 16) |
           (static_cast<std::uint32_t>(bytes.at(offset + 3)) << 24);
}

std::vector<std::uint8_t> mutateFixtureGlb(
    std::vector<std::uint8_t> source,
    const std::function<void(nlohmann::json &)> &mutate_json,
    const std::function<void(std::vector<std::uint8_t> &)> &mutate_bin = {}) {
    const auto json_size = fixtureU32(source, 12);
    const auto bin_header = 20 + json_size;
    const auto bin_size = fixtureU32(source, bin_header);
    auto document = nlohmann::json::parse(
        source.begin() + 20, source.begin() + bin_header);
    std::vector<std::uint8_t> bin(
        source.begin() + static_cast<std::ptrdiff_t>(bin_header + 8),
        source.begin() + static_cast<std::ptrdiff_t>(bin_header + 8 + bin_size));
    mutate_json(document);
    if (mutate_bin) mutate_bin(bin);
    auto json_bytes = document.dump();
    while (json_bytes.size() % 4 != 0) json_bytes.push_back(' ');
    while (bin.size() % 4 != 0) bin.push_back(0);
    std::vector<std::uint8_t> result;
    TestGltfFragmentFixture::detail::appendU32(result, 0x46546c67);
    TestGltfFragmentFixture::detail::appendU32(result, 2);
    TestGltfFragmentFixture::detail::appendU32(
        result, static_cast<std::uint32_t>(12 + 8 + json_bytes.size() +
                                           8 + bin.size()));
    TestGltfFragmentFixture::detail::appendU32(
        result, static_cast<std::uint32_t>(json_bytes.size()));
    TestGltfFragmentFixture::detail::appendU32(result, 0x4e4f534a);
    result.insert(result.end(), json_bytes.begin(), json_bytes.end());
    TestGltfFragmentFixture::detail::appendU32(
        result, static_cast<std::uint32_t>(bin.size()));
    TestGltfFragmentFixture::detail::appendU32(result, 0x004e4942);
    result.insert(result.end(), bin.begin(), bin.end());
    return result;
}

std::set<std::string> occurrenceIds(const ModelTemplate &model) {
    std::set<std::string> result;
    for (const auto &material : model.material_primitives) {
        for (const auto &primitive : material.primitives) {
            REQUIRE(primitive.occurrence);
            REQUIRE_FALSE(primitive.occurrence->occurrence_id.empty());
            result.insert(primitive.occurrence->occurrence_id);
        }
    }
    return result;
}

} // namespace

TEST_CASE(
    "glTF loading derives ray-query deformation flags from source geometry",
    "[wp282][gltf][ray-query]") {
    setupLogger();
    const auto temp_dir = std::filesystem::temp_directory_path() /
                          ("pelican_wp282_flags_" + std::to_string(
                               std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
    std::filesystem::create_directories(temp_dir);
    const TempDirGuard temp_guard{temp_dir};
    const auto static_path = temp_dir / "static.glb";
    const auto skinned_path = temp_dir / "skinned.glb";
    const auto morph_path = temp_dir / "morph.glb";
    TestGltfFragmentFixture::writeGlb(static_path);
    TestSkeletalFixture::writeGlb(skinned_path);
    TestMorphFixture::writeGlb(morph_path);
#if PELICAN_WITH_VAT
    const auto vat_path = temp_dir / "vat.glb";
    TestVatFixture::writeTinyVatGlb(vat_path);
#endif

    FastModuleContainer modules;
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    TestSupport::requireVulkanDevice(
        "Vulkan glTF geometry-flag loading unavailable");
    (void)GET_MODULE(StandardMaterialResource);

    auto &loader = GET_MODULE(GltfLoader);
    const auto static_model = loader.loadGltfBinary(
        static_path.string(), fragment("mesh", "MeshA"));
    REQUIRE(static_model.material_primitives.size() == 1);
    REQUIRE(static_model.material_primitives.front().primitives.size() == 1);
    const auto &static_primitive =
        static_model.material_primitives.front().primitives.front();
    CHECK_FALSE(static_primitive.skinned);
    CHECK_FALSE(static_primitive.morph_deformed);
    CHECK_FALSE(static_primitive.vat_deformed);

    const auto skinned_model =
        loader.loadGltfBinary(skinned_path.string());
    REQUIRE(skinned_model.material_primitives.size() == 1);
    REQUIRE(skinned_model.material_primitives.front().primitives.size() == 1);
    const auto &skinned_primitive =
        skinned_model.material_primitives.front().primitives.front();
    CHECK(skinned_primitive.skinned);
    CHECK_FALSE(skinned_primitive.morph_deformed);
    CHECK_FALSE(skinned_primitive.vat_deformed);

    const auto morph_model =
        loader.loadGltfBinary(morph_path.string());
    REQUIRE(morph_model.material_primitives.size() == 1);
    REQUIRE(morph_model.material_primitives.front().primitives.size() == 1);
    const auto &morph_primitive =
        morph_model.material_primitives.front().primitives.front();
    CHECK_FALSE(morph_primitive.skinned);
    CHECK(morph_primitive.morph_deformed);
    CHECK_FALSE(morph_primitive.vat_deformed);

#if PELICAN_WITH_VAT
    const auto vat_model = loader.loadGltfBinary(vat_path.string());
    REQUIRE(vat_model.material_primitives.size() == 1);
    REQUIRE(vat_model.material_primitives.front().primitives.size() == 1);
    const auto &vat_primitive =
        vat_model.material_primitives.front().primitives.front();
    CHECK_FALSE(vat_primitive.skinned);
    CHECK_FALSE(vat_primitive.morph_deformed);
    CHECK(vat_primitive.vat_deformed);
#endif

    GET_MODULE(VulkanManageCore).waitIdle();
}

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
    const auto routing_path =
        temp_dir / "material_routing.glb";
    TestGltfFragmentFixture::writeGlb(glb_path);
    TestGltfFragmentFixture::writeGlb(duplicate_path, true);
    TestGltfFragmentFixture::
        writeNormalizedPositionGlb(normalized_path);
    TestGltfFragmentFixture::
        writeMaterialRoutingGlb(routing_path);
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
              R"json({"schema":"pelican.asset_data","version":1,"models":[{"name":"selected","path":"fragment.glb#mesh/MeshA"}]})json");

    FastModuleContainer modules;
    std::filesystem::create_directories(temp_dir / "passes");
    writeText(temp_dir / "passes/main_rendering_config.json",
              R"json({"pipeline":{"preset":"engine://render_pipelines/hybrid_v1.json"},"features":[]})json");
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

    TestSupport::requireVulkanDevice(
        "Vulkan headless rendering unavailable");
    (void)GET_MODULE(StandardMaterialResource);

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
    REQUIRE(selected.named_materials.size() == 1);
    const auto selected_material =
        selected.named_materials.front().material;
    const auto selected_base_color =
        materials.materialTextureForTesting(
            selected_material, 0);
    const auto selected_metallic_roughness =
        materials.materialTextureForTesting(
            selected_material, 1);
    const auto selected_occlusion =
        materials.materialTextureForTesting(
            selected_material,
            PELICAN_MATERIAL_OCCLUSION_TEXTURE_BINDING);
    REQUIRE(selected_base_color.has_value());
    REQUIRE(materials.textureMipLevelsForTesting(
                *selected_base_color) == 3);
    REQUIRE(selected_metallic_roughness.has_value());
    REQUIRE(materials.textureMipLevelsForTesting(
                *selected_metallic_roughness) == 3);
    REQUIRE(selected_occlusion.has_value());
    REQUIRE(*selected_occlusion ==
            *selected_metallic_roughness);

    const auto routed =
        loader.loadGltfBinary(
            routing_path.string());
    REQUIRE(routed.named_materials.size() == 3);
    const auto material_by_name =
        [&](std::string_view name) {
            const auto found = std::find_if(
                routed.named_materials.begin(),
                routed.named_materials.end(),
                [&](const auto &entry) {
                    return entry.name == name;
                });
            REQUIRE(found !=
                    routed.named_materials.end());
            return found->material;
        };
    REQUIRE(
        materials.routeForMaterial(
            material_by_name("OpaqueMaterial")) ==
        MaterialRouteClass::deferred_geometry);
    REQUIRE(
        materials.routeForMaterial(
            material_by_name("MaskMaterial")) ==
        MaterialRouteClass::deferred_geometry);
    REQUIRE(
        materials.routeForMaterial(
            material_by_name("BlendMaterial")) ==
        MaterialRouteClass::forward_transparent);

    const auto full_path =
        loader.loadGltfBinary(glb_path.string(), fragment("mesh", "RootB/Cube"));
    REQUIRE(primitiveCount(full_path) == 1);

    const auto subtree = loader.loadGltfBinary(glb_path.string(), fragment("node", "RootA"));
    REQUIRE(primitiveCount(subtree) == 1);

    const auto material_only =
        loader.loadGltfBinary(glb_path.string(), fragment("material", "MatB"));
    REQUIRE(material_only.material_primitives.size() == 1);
    REQUIRE(material_only.material_primitives.front().primitives.empty());
    REQUIRE(material_only.named_materials.size() == 1);
    const auto material_only_occlusion =
        materials.materialTextureForTesting(
            material_only.named_materials.front().material,
            PELICAN_MATERIAL_OCCLUSION_TEXTURE_BINDING);
    REQUIRE(material_only_occlusion.has_value());
    REQUIRE(*material_only_occlusion ==
            GET_MODULE(StandardMaterialResource)
                .occlusionDefaultTexture());

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
    test_support::loadScene(GET_MODULE(SceneLoader), "default_scene");
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
    writeText(temp_dir / "assets.json",
              R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");

    FastModuleContainer modules;
    std::filesystem::create_directories(temp_dir / "passes");
    writeText(temp_dir / "passes/main_rendering_config.json",
              R"json({"pipeline":{"preset":"engine://render_pipelines/hybrid_v1.json"},"features":[]})json");
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

    TestSupport::requireVulkanDevice(
        "Vulkan headless rendering unavailable");
    (void)GET_MODULE(StandardMaterialResource);

    auto &loader = GET_MODULE(GltfLoader);
    REQUIRE(primitiveCount(loader.loadGltfBinary((yup / "model.glb").string())) == 1);
    REQUIRE(primitiveCount(loader.loadGltfBinary(
                (fixture_root / "root_zup_cm_usda" / "model.glb").string())) == 4);
    REQUIRE(primitiveCount(loader.loadGltfBinary(
                (fixture_root / "root_zup_cm_usdz" / "model.glb").string())) == 4);

    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &scene = GET_MODULE(SceneLoader);
    Pelican::test_support::loadScene(scene, "default_scene");
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
    writeText(temp_dir / "assets.json",
              R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");

    FastModuleContainer modules;
    std::filesystem::create_directories(temp_dir / "passes");
    writeText(temp_dir / "passes/main_rendering_config.json",
              R"json({"pipeline":{"preset":"engine://render_pipelines/hybrid_v1.json"},"features":[]})json");
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
    TestSupport::requireVulkanDevice(
        "Vulkan headless rendering unavailable");
    (void)GET_MODULE(StandardMaterialResource);

    auto model = GET_MODULE(GltfLoader).loadGltfBinary((delivery / "model.glb").string());
    REQUIRE(primitiveCount(model) == 1);
    applyPrimitiveMaterialBindings(model, bindings, "wp124_generated_coat");
    REQUIRE(primitiveCount(model) == 1);

    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &scene = GET_MODULE(SceneLoader);
    Pelican::test_support::loadScene(scene, "default_scene");
    GET_MODULE(ECSCore).update();
    REQUIRE(scene.objectId("World_CoatTriangle"));
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("WP382c glTF occurrence identity survives authored reorderings and reports collisions",
          "[wp382c][gltf][gpu]") {
    setupLogger();
    const auto temp_dir = std::filesystem::temp_directory_path() /
                          ("pelican_wp382c_" + std::to_string(
                              std::chrono::steady_clock::now()
                                  .time_since_epoch().count()));
    std::filesystem::create_directories(temp_dir);
    const TempDirGuard temp_guard{temp_dir};
    auto base = TestGltfFragmentFixture::makeGlb(false, false, false, true);
    base = mutateFixtureGlb(std::move(base), [](nlohmann::json &json) {
        json["nodes"][1]["extras"] = {{"id", "body-node"}};
        json["meshes"][0]["primitives"][0]["extras"] =
            {{"id", "body-a"}};
        auto second = json["meshes"][0]["primitives"][0];
        second["extras"] = {{"id", "body-b"}};
        json["meshes"][0]["primitives"].push_back(std::move(second));
    });
    const auto leading_node = mutateFixtureGlb(
        base, [](nlohmann::json &json) {
            auto nodes = nlohmann::json::array();
            nodes.push_back({{"name", "Unrelated"}});
            for (auto node : json["nodes"]) {
                if (node.contains("children"))
                    for (auto &child : node["children"])
                        child = child.get<int>() + 1;
                nodes.push_back(std::move(node));
            }
            json["nodes"] = std::move(nodes);
            for (auto &root : json["scenes"][0]["nodes"])
                root = root.get<int>() + 1;
        });
    const auto primitive_reorder = mutateFixtureGlb(
        base, [](nlohmann::json &json) {
            auto &primitives = json["meshes"][0]["primitives"];
            std::swap(primitives[0], primitives[1]);
        });
    const auto geometry_replacement = mutateFixtureGlb(
        base,
        [](nlohmann::json &json) {
            for (auto &primitive : json["meshes"][0]["primitives"])
                primitive.erase("extras");
        },
        [](std::vector<std::uint8_t> &bin) {
            const float replacement = -0.25F;
            std::memcpy(bin.data(), &replacement, sizeof(replacement));
        });
    const auto ambiguous = mutateFixtureGlb(
        TestGltfFragmentFixture::makeGlb(false, false, false, true),
        [](nlohmann::json &json) {
            json["meshes"][0]["primitives"].push_back(
                json["meshes"][0]["primitives"][0]);
        });
    const auto write = [&](std::string_view name,
                           const std::vector<std::uint8_t> &bytes) {
        const auto path = temp_dir / name;
        TestGltfFragmentFixture::detail::writeBytes(path, bytes);
        return path;
    };
    const auto base_path = write("base.glb", base);
    const auto leading_path = write("leading.glb", leading_node);
    const auto reordered_path = write("reordered.glb", primitive_reorder);
    const auto replaced_path = write("replaced.glb", geometry_replacement);
    const auto ambiguous_path = write("ambiguous.glb", ambiguous);

    FastModuleContainer modules;
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    TestSupport::requireVulkanDevice(
        "Vulkan glTF occurrence inspection unavailable");
    (void)GET_MODULE(StandardMaterialResource);
    auto &loader = GET_MODULE(GltfLoader);
    const auto inspect = [&](const std::filesystem::path &path) {
        return loader.inspect(loader.prepareGltfBinary(
            path.string(), std::nullopt,
            CanonicalModelAssetKey{"logical.hero"}));
    };

    const auto original = inspect(base_path);
    const auto asset_table_reordered = inspect(base_path);
    REQUIRE(occurrenceIds(asset_table_reordered) == occurrenceIds(original));
    REQUIRE(occurrenceIds(inspect(leading_path)) == occurrenceIds(original));
    REQUIRE(occurrenceIds(inspect(reordered_path)) == occurrenceIds(original));
    REQUIRE(occurrenceIds(inspect(replaced_path)) != occurrenceIds(original));

    bool parsed_node_extras = false;
    bool parsed_primitive_extras = false;
    for (const auto &material : original.material_primitives) {
        for (const auto &primitive : material.primitives) {
            parsed_node_extras |=
                primitive.occurrence->node_explicit_id == "body-node";
            parsed_primitive_extras |=
                primitive.occurrence->primitive_explicit_id == "body-a" ||
                primitive.occurrence->primitive_explicit_id == "body-b";
        }
    }
    REQUIRE(parsed_node_extras);
    REQUIRE(parsed_primitive_extras);

    const auto collision = inspect(ambiguous_path);
    std::size_t ambiguous_count = 0;
    for (const auto &material : collision.material_primitives)
        for (const auto &primitive : material.primitives)
            ambiguous_count += primitive.occurrence->ambiguous ? 1 : 0;
    REQUIRE(ambiguous_count == 2);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("WP382c glTF node extras.id drives occurrence identity through the production loader",
          "[wp382c][gltf][gpu]") {
    setupLogger();
    const auto temp_dir = std::filesystem::temp_directory_path() /
                          ("pelican_wp382c_extras_" + std::to_string(
                              std::chrono::steady_clock::now()
                                  .time_since_epoch().count()));
    std::filesystem::create_directories(temp_dir);
    const TempDirGuard temp_guard{temp_dir};

    // Decision 29 orders NodeIdentity as tag 1 (extras.id) before tag 2
    // (name path). These three GLBs isolate that order on the production
    // GltfLoader::inspect route: node 1 is "RootA/Cube" in every one of them
    // and node 3 ("RootB/Cube") is never touched, so it pins the comparison.
    const auto authored = [&](std::string_view id,
                              bool rename_path) {
        return mutateFixtureGlb(
            TestGltfFragmentFixture::makeGlb(false, false, false, true),
            [id, rename_path](nlohmann::json &json) {
                json["nodes"][1]["extras"] = {{"id", std::string{id}}};
                if (rename_path) {
                    json["nodes"][0]["name"] = "RenamedRoot";
                    json["nodes"][1]["name"] = "RenamedCube";
                }
            });
    };
    const auto write = [&](std::string_view name,
                           const std::vector<std::uint8_t> &bytes) {
        const auto path = temp_dir / name;
        TestGltfFragmentFixture::detail::writeBytes(path, bytes);
        return path;
    };
    const auto alpha_path = write("alpha.glb", authored("alpha", false));
    const auto beta_path = write("beta.glb", authored("beta", false));
    const auto renamed_path =
        write("alpha_renamed.glb", authored("alpha", true));

    FastModuleContainer modules;
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    TestSupport::requireVulkanDevice(
        "Vulkan glTF occurrence inspection unavailable");
    (void)GET_MODULE(StandardMaterialResource);
    auto &loader = GET_MODULE(GltfLoader);
    const auto inspect = [&](const std::filesystem::path &path) {
        return loader.inspect(loader.prepareGltfBinary(
            path.string(), std::nullopt,
            CanonicalModelAssetKey{"logical.hero"}));
    };

    const auto alpha = inspect(alpha_path);
    const auto beta = inspect(beta_path);
    const auto renamed = inspect(renamed_path);
    const auto alpha_ids = occurrenceIds(alpha);
    const auto beta_ids = occurrenceIds(beta);
    const auto renamed_ids = occurrenceIds(renamed);
    REQUIRE(alpha_ids.size() == 2);
    REQUIRE(beta_ids.size() == 2);

    bool parsed_alpha = false;
    for (const auto &material : alpha.material_primitives)
        for (const auto &primitive : material.primitives)
            parsed_alpha |=
                primitive.occurrence->node_explicit_id == "alpha";
    REQUIRE(parsed_alpha);

    // Same name path, same content, different extras.id: different occurrence.
    // Exactly one of the two nodes moves, so the untouched RootB/Cube node
    // stays shared and the symmetric difference is the authored node only.
    REQUIRE(alpha_ids != beta_ids);
    std::set<std::string> shared;
    std::set_intersection(alpha_ids.begin(), alpha_ids.end(),
                          beta_ids.begin(), beta_ids.end(),
                          std::inserter(shared, shared.end()));
    REQUIRE(shared.size() == 1);

    // Same extras.id, different node names: the same occurrence. The name
    // path is not an input to the identity once extras.id is authored.
    REQUIRE(renamed_ids == alpha_ids);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("WP382d imported variants retain every override in the shared ray plan",
          "[WP382d][gltf][occurrence][importer-matrix]") {
    setupLogger();
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_wp382d_variants_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const TempDirGuard guard{root};
    TestGltfFragmentFixture::writeGlb(root / "static.glb");
    TestSkeletalFixture::writeGlb(root / "skinned.glb");
    TestMorphFixture::writeGlb(root / "morph.glb");
    TestMorphFixture::writeGlb(root / "split.vrm",
        {.skinned = true, .vrm_first_person = true});
    struct VariantCase {
        std::string file;
        std::vector<ImporterVariantV1> variants;
        bool eligible;
    };
    std::vector<VariantCase> cases{
        {"static.glb", {ImporterVariantV1::none, ImporterVariantV1::none}, true},
        {"skinned.glb", {ImporterVariantV1::skinned}, false},
        {"morph.glb", {ImporterVariantV1::morph_expanded}, false},
        {"split.vrm", {ImporterVariantV1::vrm_split_both,
                       ImporterVariantV1::vrm_split_third_person_only}, false},
    };
#if PELICAN_WITH_VAT
    TestVatFixture::writeTinyVatGlb(root / "vat.glb");
    cases.push_back({"vat.glb", {ImporterVariantV1::vat_deformed}, false});
#endif
    FastModuleContainer modules;
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    TestSupport::requireVulkanDevice("Vulkan importer matrix unavailable");
    (void)GET_MODULE(StandardMaterialResource);
    auto &loader = GET_MODULE(GltfLoader);
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    auto project = std::make_shared<RayTracingProjectConfig>();
    project->layout = RayTracingRt31Fixture::v2Layout(std::nullopt);
    constexpr std::uint64_t object = 382;
    const nlohmann::ordered_json witness{
        {"type", "pelican_ray_instance"}, {"occurrence", 1},
        {"origin", {{"kind", "authored"}, {"authoring_component_index", 1}}}};
    for (const auto &test : cases) {
        CAPTURE(test.file);
        auto model = loader.commit(loader.prepareGltfBinary(
            (root / test.file).string(), std::nullopt,
            CanonicalModelAssetKey{"matrix." + test.file}));
        std::vector<ImporterVariantV1> variants;
        std::set<std::string> occurrence_ids;
        RayInstanceComponent declaration;
        declaration.overrides.emplace();
        std::uint32_t next_row = 1;
        for (const auto &material : model.material_primitives) {
            for (const auto &primitive : material.primitives) {
                REQUIRE(primitive.occurrence);
                REQUIRE_FALSE(primitive.occurrence->error);
                REQUIRE_FALSE(primitive.occurrence->occurrence_id.empty());
                variants.push_back(primitive.occurrence->variant.kind);
                REQUIRE(occurrence_ids.insert(primitive.occurrence->occurrence_id).second);
                declaration.overrides->rows.push_back({
                    .envelope = {.id = next_row++},
                    .value = {.occurrence_id = primitive.occurrence->occurrence_id, .mask = 9},
                });
            }
        }
        declaration.overrides->next_id = next_row;
        std::sort(variants.begin(), variants.end());
        auto expected = test.variants;
        std::sort(expected.begin(), expected.end());
        REQUIRE(variants == expected);
        const auto id = instances.placeModelInstance(model);
        instances.setRayInstanceDeclaration(id, object, declaration);
        auto candidate = instances.snapshotPreparedScene();
        candidate.ray_scene_declarations = std::make_shared<const RaySceneDeclarationCatalog>(
            RaySceneDeclarationCatalog{{object, {declaration, witness}}});
        instances.prepareRayScenePlan(candidate, project);
        instances.publishPreparedScene(std::move(candidate));
        const auto plan = instances.rayAsPlan();
        const auto inventory = instances.rayAsInventory();
        REQUIRE(plan);
        REQUIRE(inventory->instances.size() == occurrence_ids.size());
        REQUIRE(plan->occurrence_overrides.size() == occurrence_ids.size());
        const auto diagnostics = rayScenePlanDiagnostics(*plan, object);
        if (test.eligible) {
            REQUIRE(plan->rows.size() == occurrence_ids.size());
            CHECK(diagnostics.rejected_rows.empty());
        } else {
            CHECK(plan->rows.empty());
            REQUIRE(diagnostics.rejected_rows.size() == occurrence_ids.size());
        }
        std::set<std::string> resolved_ids;
        for (const auto &row : plan->occurrence_overrides) {
            resolved_ids.insert(row.occurrence_id);
            CHECK(row.ray_instance_component_witness == witness);
            if (test.eligible) {
                REQUIRE(row.q.has_value());
                CHECK(row.error.empty());
                const auto found = std::find_if(plan->rows.begin(), plan->rows.end(),
                    [&](const auto &value) { return value.q == *row.q; });
                REQUIRE(found != plan->rows.end());
                CHECK(inventory->instances.at(found->source_index).occurrence_id == row.occurrence_id);
                CHECK(found->instance_mask == 9);
            } else {
                CHECK_FALSE(row.q.has_value());
                CHECK(row.error == "OccurrenceNotRayTraced");
                const auto rejected = std::find_if(diagnostics.rejected_rows.begin(),
                    diagnostics.rejected_rows.end(), [&](const auto &value) {
                        return value.address.row_id == row.row_id;
                    });
                REQUIRE(rejected != diagnostics.rejected_rows.end());
                CHECK(rejected->address.component_witness == witness);
                CHECK(rejected->error.at("code") == "OccurrenceNotRayTraced");
                CHECK(rejected->error.at("occurrence_id") == row.occurrence_id);
            }
        }
        CHECK(resolved_ids == occurrence_ids);
        REQUIRE(instances.removeModelInstance(id));
    }
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("glTF collision declarations retain node-owned CPU triangles", "[gltf][collision-import][gpu]") {
    setupLogger();
    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("pelican_collision_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(temp_dir);
    const TempDirGuard guard{temp_dir};
    auto bytes = mutateFixtureGlb(TestGltfFragmentFixture::makeGlb(false, true, false, true),
        [](nlohmann::json &json) {
            json["nodes"][1]["extras"] = {{"pelican.collision", {{"shape", "mesh"}}}};
            json["nodes"][1]["translation"] = {2, 0, 0};
            auto second = json["meshes"][0]["primitives"][0];
            second.erase("indices");
            json["meshes"][0]["primitives"].push_back(second);
        });
    const auto path = temp_dir / "collision.glb";
    { std::ofstream out{path, std::ios::binary}; out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()); }
    FastModuleContainer modules;
    GET_MODULE(EngineLaunchConfig).headless = true;
    GET_MODULE(EngineLaunchConfig).headless_extent = vk::Extent2D{16, 16};
    TestSupport::requireVulkanDevice("glTF collision import unavailable");
    auto &loader = GET_MODULE(GltfLoader);
    const auto loaded = loader.inspect(loader.prepareGltfBinary(path.string()));
    REQUIRE(loaded.collision_meshes);
    REQUIRE(loaded.collision_meshes->size() == 1);
    const auto &mesh = loaded.collision_meshes->at(0);
    CHECK(mesh.node_index == 1);
    CHECK(mesh.positions.size() == 6);
    CHECK(mesh.indices == std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5});
    CHECK(mesh.positions.at(0).x == 1.5f);
    auto invalid = mutateFixtureGlb(bytes, [](nlohmann::json &json) {
        json["nodes"][1]["extras"]["pelican.collision"]["shape"] = "convex";
    });
    const auto bad_path = temp_dir / "invalid.glb";
    { std::ofstream out{bad_path, std::ios::binary}; out.write(reinterpret_cast<const char *>(invalid.data()), invalid.size()); }
    CHECK_THROWS_WITH(loader.inspect(loader.prepareGltfBinary(bad_path.string())),
                      Catch::Matchers::ContainsSubstring("pelican.collision supports only"));
}

} // namespace Pelican
