#include "../src/core/container.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/ecs/predefined/modelview.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/material/materialcontainer.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/gltf.hpp"
#include "../src/core/asset/model.hpp"
#include "../src/core/vkcore/core.hpp"
#include "gltf_fragment_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

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
    TestGltfFragmentFixture::writeGlb(glb_path);
    TestGltfFragmentFixture::writeGlb(duplicate_path, true);
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

} // namespace Pelican
