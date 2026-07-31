#include "../src/core/container.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/log.hpp"
#include "../src/devcli/gltfsceneextract.hpp"
#include "../src/project/sceneformat.hpp"
#include "gltf_scene_fixture.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

namespace Pelican {

namespace {

struct TempDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("pelican_wp79_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() { std::filesystem::create_directories(path); }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void writeText(const std::filesystem::path &path, std::string_view text) {
    std::ofstream file{path, std::ios::binary};
    file << text;
}

const nlohmann::json &objectNamed(const nlohmann::json &objects, std::string_view name) {
    for (const auto &object : objects) {
        if (object.value("name", std::string{}) == name) {
            return object;
        }
    }
    throw std::runtime_error("missing extracted object: " + std::string{name});
}

const nlohmann::json &componentNamed(const nlohmann::json &object, std::string_view name) {
    for (const auto &component : object.at("components")) {
        if (component.value("name", std::string{}) == name) {
            return component;
        }
    }
    throw std::runtime_error("missing extracted component: " + std::string{name});
}

void requireWorld(const SceneLoader &loader, std::string_view name, float x, float y, float z) {
    const auto transform = loader.objectTransform(name);
    REQUIRE(transform.pos.x == Catch::Approx(x).margin(1.0e-5f));
    REQUIRE(transform.pos.y == Catch::Approx(y).margin(1.0e-5f));
    REQUIRE(transform.pos.z == Catch::Approx(z).margin(1.0e-5f));
}

} // namespace

TEST_CASE("glTF scene extraction is deterministic and maps hierarchy components", "[gltf][extract]") {
    TempDir temp;
    const auto glb = temp.path / "fixture.glb";
    TestGltfSceneFixture::writeGlb(glb, true);

    const auto first = DevCli::extractGltfScene(glb, "assets/fixture.glb");
    const auto second = DevCli::extractGltfScene(glb, "assets/fixture.glb");
    REQUIRE(first.dump(2) + "\n" == second.dump(2) + "\n");
    REQUIRE(std::filesystem::is_regular_file(glb));

    const auto normalized = normalizeSceneDataJson(first);
    const auto &objects = normalized.scenes.at("default_scene").at("objects");
    REQUIRE(objects.size() == 4);
    REQUIRE(objectNamed(objects, "Root").find("parent") == objectNamed(objects, "Root").end());
    REQUIRE(objectNamed(objects, "MeshNode").at("parent") == "Root");
    REQUIRE(objectNamed(objects, "CameraRig").at("parent") == "Root");
    REQUIRE(objectNamed(objects, "SpotNode").at("parent") == "CameraRig");

    const auto &model = componentNamed(objectNamed(objects, "MeshNode"), "simplemodelview");
    REQUIRE(model.at("model") == "assets/fixture.glb#node/Root/MeshNode");
    REQUIRE(model.at("params").at("gameplay_tag") == "mesh_extra");
    REQUIRE(model.at("params").at("lod_bias") == 2);

    const auto &camera = componentNamed(objectNamed(objects, "CameraRig"), "camera");
    REQUIRE(camera.at("type") == "perspective");
    REQUIRE(camera.at("yfov").get<double>() == Catch::Approx(0.7));
    REQUIRE(camera.at("aspect").get<double>() == Catch::Approx(1.5));

    const auto &light = componentNamed(objectNamed(objects, "SpotNode"), "light");
    REQUIRE(light.at("type") == "spot");
    REQUIRE(light.at("position").at(0).get<double>() == Catch::Approx(7.0).margin(1.0e-9));
    REQUIRE(light.at("position").at(1).get<double>() == Catch::Approx(0.0).margin(1.0e-9));
    REQUIRE(light.at("position").at(2).get<double>() == Catch::Approx(4.0).margin(1.0e-9));
    REQUIRE(light.at("range").get<double>() == Catch::Approx(25.0));
}

TEST_CASE("extracted scene loads through SceneLoader with glTF world transforms", "[gltf][extract][roundtrip]") {
    setupLogger();
    TempDir temp;
    const auto glb = temp.path / "hierarchy.glb";
    TestGltfSceneFixture::writeGlb(glb, true);
    const auto extracted = DevCli::extractGltfScene(glb, "hierarchy.glb");
    writeText(temp.path / "scene.json", extracted.dump(2) + "\n");
    writeText(temp.path / "assets.json",
              R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(temp.path, false);
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

    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &loader = GET_MODULE(SceneLoader);
    loader.load("default_scene");
    GET_MODULE(ECSCore).update();

    // Direct glTF parent_world * child_local analytic values for the fixture.
    requireWorld(loader, "Root", 10.0f, 0.0f, 0.0f);
    requireWorld(loader, "MeshNode", 10.0f, 2.0f, 0.0f);
    requireWorld(loader, "CameraRig", 7.0f, 0.0f, 0.0f);
    requireWorld(loader, "SpotNode", 7.0f, 0.0f, 4.0f);
}

} // namespace Pelican
