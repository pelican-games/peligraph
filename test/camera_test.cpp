#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/renderer/camera.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Pelican {

namespace {

constexpr float pi = 3.14159265358979323846f;

struct Sandbox {
    std::filesystem::path base;
    std::filesystem::path root;

    Sandbox() {
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        base = std::filesystem::temp_directory_path() / ("pelican_camera_" + suffix);
        root = base / "project";
        std::filesystem::create_directories(root);
    }

    ~Sandbox() {
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
    }
};

void writeText(const std::filesystem::path &path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary};
    file << text;
}

std::string readText(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file},
                       std::istreambuf_iterator<char>{}};
}

void ensureLogger() {
    static const bool initialized = [] {
        setupLogger();
        return true;
    }();
    (void)initialized;
}

nlohmann::json projectJson() {
    return nlohmann::json{
        {"schema", "pelican.project"},
        {"version", 1},
        {"name", "camera-test"},
        {"engine_min_version", "0.1.0"},
        {"basic_config",
         {
             {"window_size", {{"width", 100}, {"height", 100}}},
             {"camera", {{"yfov", 0.5}, {"znear", 0.1}, {"zfar", 100.0}, {"up", {0.0, 1.0, 0.0}}}},
             {"default_scene_id", "default_scene"},
             {"scene_data_json", "scene.json"},
         }},
    };
}

const char *cameraSceneJson() {
    return R"json({
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": [
        {
          "name": "CanonicalCam",
          "components": [
            {"name": "camera", "yfov": 1.0471975511965976, "znear": 0.2, "zfar": 20.0}
          ]
        },
        {
          "name": "GltfCam",
          "components": [
            {"name": "camera", "type": "perspective", "yfov": 0.4, "znear": 0.3, "zfar": 30.0, "aspect": 1.25}
          ]
        },
        {
          "name": "ViewportCam",
          "components": [
            {"name": "camera", "type": "perspective", "yfov": 0.6, "znear": 0.4, "zfar": 40.0}
          ]
        },
        {
          "name": "OrthoCam",
          "components": [
            {"name": "camera", "type": "orthographic", "xmag": 2.0, "ymag": 3.0, "znear": 0.5, "zfar": 50.0,
             "sprite": {"pixel_perfect": "strict", "sort": "y_down"}}
          ]
        }
      ]
    }
  }
})json";
}

const char *cameraControllerSceneJson() {
    return R"json({
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": [
        {
          "name": "Target",
          "components": [
            {"name": "transform", "pos": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0]}
          ]
        },
        {
          "name": "OrbitCam",
          "components": [
            {
              "name": "camera",
              "controller": {
                "type": "orbit",
                "target": "Target",
                "distance": 4.0,
                "yaw_degrees": 90.0,
                "pitch_degrees": 10.0,
                "damping": 3.0,
                "sensitivity": 0.5
              }
            }
          ]
        },
        {
          "name": "FollowCam",
          "components": [
            {
              "name": "camera",
              "params": {
                "controller": {
                  "type": "follow",
                  "target": "Target",
                  "offset": [1.0, 2.0, 3.0],
                  "damping": 2.0
                }
              }
            }
          ]
        },
        {
          "name": "FlyCam",
          "components": [
            {
              "name": "camera",
              "controller": {
                "type": "fly",
                "speed": 6.5,
                "sensitivity": 1.25
              }
            }
          ]
        }
      ]
    }
  }
})json";
}

const char *invalidCameraControllerSceneJson() {
    return R"json({
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": [
        {
          "name": "BadCam",
          "components": [
            {"name": "camera", "controller": {"type": "rail"}}
          ]
        }
      ]
    }
  }
})json";
}

} // namespace

TEST_CASE("Camera parses v1 perspective and orthographic projection parameters", "[camera]") {
    ensureLogger();
    Sandbox sandbox;
    writeText(sandbox.root / "scene.json", cameraSceneJson());

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(ProjectSource).setProjectData(projectJson().dump());

    auto &camera = GET_MODULE(Camera);
    REQUIRE(camera.hasSceneCamera("CanonicalCam"));
    REQUIRE(camera.hasSceneCamera("GltfCam"));
    REQUIRE(camera.hasSceneCamera("OrthoCam"));
    REQUIRE(camera.activeCameraName().empty());

    camera.setActiveCamera("CanonicalCam");
    auto spec = camera.getProjectionSpec();
    REQUIRE(spec.kind == CameraProjectionKind::Perspective);
    REQUIRE(spec.yfov == Catch::Approx(pi / 3.0f));
    REQUIRE(spec.znear == Catch::Approx(0.2f));
    REQUIRE(spec.zfar == Catch::Approx(20.0f));
    const auto perspective_projection = camera.getProjectionMatrix();
    const auto perspective_near =
        perspective_projection * glm::vec4{0.0f, 0.0f, -spec.znear, 1.0f};
    const auto perspective_far =
        perspective_projection * glm::vec4{0.0f, 0.0f, -spec.zfar, 1.0f};
    REQUIRE(perspective_near.z / perspective_near.w == Catch::Approx(0.0f).margin(1.0e-6f));
    REQUIRE(perspective_far.z / perspective_far.w == Catch::Approx(1.0f).margin(1.0e-6f));

    camera.setActiveCamera("GltfCam");
    spec = camera.getProjectionSpec();
    REQUIRE(spec.kind == CameraProjectionKind::Perspective);
    REQUIRE(spec.yfov == Catch::Approx(0.4f));
    REQUIRE(spec.aspect.has_value());
    REQUIRE(*spec.aspect == Catch::Approx(1.25f));
    const auto fixed_aspect_projection = camera.getProjectionMatrix();
    camera.setScreenSize(200, 100);
    REQUIRE(camera.getProjectionMatrix()[0][0] == Catch::Approx(fixed_aspect_projection[0][0]));

    camera.setActiveCamera("ViewportCam");
    camera.setScreenSize(100, 100);
    const auto square_projection = camera.getProjectionMatrix();
    camera.setScreenSize(200, 100);
    REQUIRE(camera.getProjectionMatrix()[0][0] != Catch::Approx(square_projection[0][0]));

    camera.setActiveCamera("OrthoCam");
    spec = camera.getProjectionSpec();
    REQUIRE(spec.kind == CameraProjectionKind::Orthographic);
    REQUIRE(spec.xmag == Catch::Approx(2.0f));
    REQUIRE(spec.ymag == Catch::Approx(3.0f));
    REQUIRE(spec.znear == Catch::Approx(0.5f));
    REQUIRE(spec.zfar == Catch::Approx(50.0f));
    REQUIRE(camera.getProjectionMatrix()[0][0] == Catch::Approx(0.5f));
    REQUIRE(camera.getProjectionMatrix()[1][1] == Catch::Approx(1.0f / 3.0f));
    const auto ortho_projection = camera.getProjectionMatrix();
    const auto ortho_near = ortho_projection * glm::vec4{0.0f, 0.0f, -spec.znear, 1.0f};
    const auto ortho_far = ortho_projection * glm::vec4{0.0f, 0.0f, -spec.zfar, 1.0f};
    REQUIRE(ortho_near.z / ortho_near.w == Catch::Approx(0.0f).margin(1.0e-6f));
    REQUIRE(ortho_far.z / ortho_far.w == Catch::Approx(1.0f).margin(1.0e-6f));
    REQUIRE(camera.getSpritePolicy().pixel_perfect == CameraPixelPerfectMode::strict);
    REQUIRE(camera.getSpritePolicy().sort == CameraSpriteSortPolicy::y_down);

    REQUIRE_THROWS_WITH(camera.setActiveCamera("MissingCam"),
                        Catch::Matchers::ContainsSubstring("MissingCam"));
}

TEST_CASE("Project sprite ppu and default camera policy are validated", "[camera][sprite][pixel]") {
    ensureLogger();
    Sandbox sandbox;
    writeText(sandbox.root / "scene.json", R"json({
      "schema":"pelican.scene","version":1,
      "scenes":{"default_scene":{"objects":[]}}
    })json");
    auto project = projectJson();
    project["basic_config"]["sprite"] = {{"pixels_per_unit", 8.0}};
    project["basic_config"]["camera"]["sprite"] = {
        {"pixel_perfect", "strict"}, {"sort", "declaration"}};

    {
        FastModuleContainer modules;
        GET_MODULE(PathResolver).setup(sandbox.root, false);
        GET_MODULE(ProjectSource).setProjectData(project.dump());
        REQUIRE(GET_MODULE(ProjectBasicConfig).spritePixelsPerUnit() == Catch::Approx(8.0f));
        const auto policy = GET_MODULE(Camera).getSpritePolicy();
        REQUIRE(policy.pixel_perfect == CameraPixelPerfectMode::strict);
        REQUIRE(policy.sort == CameraSpriteSortPolicy::declaration);
    }

    project["basic_config"]["sprite"]["pixels_per_unit"] = 0.0;
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(ProjectSource).setProjectData(project.dump());
    REQUIRE_THROWS_WITH(GET_MODULE(ProjectBasicConfig),
                        Catch::Matchers::ContainsSubstring("pixels_per_unit"));
}

TEST_CASE("Project camera up is a finite non-zero vec3", "[camera][config]") {
    ensureLogger();

    struct InvalidUp {
        const char *name;
        nlohmann::json value;
        const char *message;
    };
    const InvalidUp invalid_values[] = {
        {"too short", nlohmann::json::array({0.0, 1.0}), "exactly 3 numbers"},
        {"too long", nlohmann::json::array({0.0, 1.0, 0.0, 0.0}),
         "exactly 3 numbers"},
        {"not an array", "up", "exactly 3 numbers"},
        {"non-numeric component", nlohmann::json::array({0.0, "up", 0.0}),
         "exactly 3 numbers"},
        {"zero vector", nlohmann::json::array({0.0, 0.0, 0.0}), "non-zero"},
    };

    for (const auto &invalid : invalid_values) {
        DYNAMIC_SECTION(invalid.name) {
            Sandbox sandbox;
            auto project = projectJson();
            project["basic_config"]["camera"]["up"] = invalid.value;

            FastModuleContainer modules;
            GET_MODULE(PathResolver).setup(sandbox.root, false);
            GET_MODULE(ProjectSource).setProjectData(project.dump());
            REQUIRE_THROWS_WITH(
                GET_MODULE(ProjectBasicConfig),
                Catch::Matchers::ContainsSubstring(invalid.message));
        }
    }
}

TEST_CASE("Scene camera sprite policy is closed and names invalid values", "[camera][sprite]") {
    ensureLogger();
    Sandbox sandbox;
    auto scene = nlohmann::json::parse(cameraSceneJson());
    auto &sprite = scene["scenes"]["default_scene"]["objects"][0]["components"][0]["sprite"];
    sprite = {{"pixel_perfect", "strict"}, {"sort", "z"}, {"snap_each_sprite", true}};
    writeText(sandbox.root / "scene.json", scene.dump());

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(ProjectSource).setProjectData(projectJson().dump());
    REQUIRE_THROWS_WITH(GET_MODULE(Camera),
                        Catch::Matchers::ContainsSubstring("snap_each_sprite"));
}

TEST_CASE("Project sprite policy objects are closed", "[camera][sprite][pixel]") {
    ensureLogger();
    Sandbox sandbox;
    writeText(sandbox.root / "scene.json", R"json({
      "schema":"pelican.scene","version":1,
      "scenes":{"default_scene":{"objects":[]}}
    })json");
    auto project = projectJson();

    SECTION("project sprite settings") {
        project["basic_config"]["sprite"] = {
            {"pixels_per_unit", 8.0}, {"logical_resolution", true}};
        FastModuleContainer modules;
        GET_MODULE(PathResolver).setup(sandbox.root, false);
        GET_MODULE(ProjectSource).setProjectData(project.dump());
        REQUIRE_THROWS_WITH(GET_MODULE(ProjectBasicConfig),
                            Catch::Matchers::ContainsSubstring("logical_resolution"));
    }

    SECTION("camera sprite settings") {
        project["basic_config"]["camera"]["sprite"] = {
            {"pixel_perfect", "strict"}, {"sort", "z"}, {"camera_snap", true}};
        FastModuleContainer modules;
        GET_MODULE(PathResolver).setup(sandbox.root, false);
        GET_MODULE(ProjectSource).setProjectData(project.dump());
        REQUIRE_THROWS_WITH(GET_MODULE(ProjectBasicConfig),
                            Catch::Matchers::ContainsSubstring("camera_snap"));
    }
}

TEST_CASE("Project camera aliases are rejected with v1 replacement names", "[camera]") {
    ensureLogger();
    struct AliasCase {
        const char *name;
        const char *replacement;
        double value;
    };
    const AliasCase aliases[] = {
        {"fov_y", "yfov", 45.0},
        {"near", "znear", 0.1},
        {"far", "zfar", 100.0},
    };

    for (const auto &alias : aliases) {
        DYNAMIC_SECTION(alias.name) {
            Sandbox sandbox;
            auto project = projectJson();
            auto &camera_json = project["basic_config"]["camera"];
            camera_json.erase(alias.replacement);
            camera_json[alias.name] = alias.value;

            FastModuleContainer modules;
            GET_MODULE(PathResolver).setup(sandbox.root, false);
            GET_MODULE(ProjectSource).setProjectData(project.dump());

            std::string message;
            try {
                (void)GET_MODULE(ProjectBasicConfig);
            } catch (const std::exception &ex) {
                message = ex.what();
            }
            REQUIRE(message.find(alias.name) != std::string::npos);
            REQUIRE(message.find(alias.replacement) != std::string::npos);
            if (std::string_view{alias.name} == "fov_y") {
                REQUIRE(message.find("radians") != std::string::npos);
            }
        }
    }
}

TEST_CASE("Scene camera aliases are rejected with camera and v1 replacement names", "[camera]") {
    ensureLogger();
    struct AliasCase {
        const char *name;
        const char *replacement;
        double value;
    };
    const AliasCase aliases[] = {
        {"fov_y", "yfov", 60.0},
        {"near", "znear", 0.2},
        {"far", "zfar", 20.0},
    };

    for (const auto &alias : aliases) {
        DYNAMIC_SECTION(alias.name) {
            Sandbox sandbox;
            nlohmann::json camera_component{
                {"name", "camera"}, {"yfov", 0.5}, {"znear", 0.1}, {"zfar", 100.0}};
            camera_component.erase(alias.replacement);
            camera_component[alias.name] = alias.value;
            const nlohmann::json scene{
                {"schema", "pelican.scene"},
                {"version", 1},
                {"scenes",
                 {{"default_scene",
                   {{"objects",
                     nlohmann::json::array({{{"name", "LegacyAliasCam"},
                                             {"components", nlohmann::json::array({camera_component})}}})}}}}},
            };
            writeText(sandbox.root / "scene.json", scene.dump());

            FastModuleContainer modules;
            GET_MODULE(PathResolver).setup(sandbox.root, false);
            GET_MODULE(ProjectSource).setProjectData(projectJson().dump());

            std::string message;
            try {
                (void)GET_MODULE(Camera);
            } catch (const std::exception &ex) {
                message = ex.what();
            }
            REQUIRE(message.find("LegacyAliasCam") != std::string::npos);
            REQUIRE(message.find(alias.name) != std::string::npos);
            REQUIRE(message.find(alias.replacement) != std::string::npos);
            if (std::string_view{alias.name} == "fov_y") {
                REQUIRE(message.find("radians") != std::string::npos);
            }
        }
    }
}

TEST_CASE("Camera parses orbit, follow, and fly controller params", "[camera]") {
    ensureLogger();
    Sandbox sandbox;
    writeText(sandbox.root / "scene.json", cameraControllerSceneJson());

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(ProjectSource).setProjectData(projectJson().dump());

    auto &camera = GET_MODULE(Camera);
    const auto *orbit = camera.sceneCameraController("OrbitCam");
    REQUIRE(orbit != nullptr);
    REQUIRE(orbit->type == Camera::SceneCameraControllerType::Orbit);
    REQUIRE(orbit->target == "Target");
    REQUIRE(orbit->distance == Catch::Approx(4.0f));
    REQUIRE(orbit->yaw == Catch::Approx(pi / 2.0f));
    REQUIRE(orbit->pitch == Catch::Approx(pi / 18.0f));
    REQUIRE(orbit->damping == Catch::Approx(3.0f));
    REQUIRE(orbit->sensitivity == Catch::Approx(0.5f));

    const auto *follow = camera.sceneCameraController("FollowCam");
    REQUIRE(follow != nullptr);
    REQUIRE(follow->type == Camera::SceneCameraControllerType::Follow);
    REQUIRE(follow->target == "Target");
    REQUIRE(follow->offset.x == Catch::Approx(1.0f));
    REQUIRE(follow->offset.y == Catch::Approx(2.0f));
    REQUIRE(follow->offset.z == Catch::Approx(3.0f));
    REQUIRE(follow->damping == Catch::Approx(2.0f));

    const auto *fly = camera.sceneCameraController("FlyCam");
    REQUIRE(fly != nullptr);
    REQUIRE(fly->type == Camera::SceneCameraControllerType::Fly);
    REQUIRE(fly->speed == Catch::Approx(6.5f));
    REQUIRE(fly->sensitivity == Catch::Approx(1.25f));
}

TEST_CASE("Camera controller parse errors include camera name and unknown type", "[camera]") {
    ensureLogger();
    Sandbox sandbox;
    writeText(sandbox.root / "scene.json", invalidCameraControllerSceneJson());

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(ProjectSource).setProjectData(projectJson().dump());

    try {
        (void)GET_MODULE(Camera);
        FAIL("Camera construction should reject an unknown controller type");
    } catch (const std::runtime_error &error) {
        const std::string message = error.what();
        REQUIRE(message.find("BadCam") != std::string::npos);
        REQUIRE(message.find("rail") != std::string::npos);
    }
}

TEST_CASE("Runtime navigation camera overlays the resolved scene without changing authored bytes",
          "[camera][free-camera][wp273]") {
    ensureLogger();
    Sandbox sandbox;
    const std::string authored = cameraControllerSceneJson();
    const auto scene_path = sandbox.root / "scene.json";
    writeText(scene_path, authored);

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(ProjectSource).setProjectData(projectJson().dump());
    GET_MODULE(EngineLaunchConfig).free_camera = EngineLaunchFreeCamera{
        .preset = EngineLaunchFreeCameraPreset::Blender,
        .orbit_distance = 7.0f,
        .sensitivity = 2.0f,
    };

    auto &camera = GET_MODULE(Camera);
    const auto runtime_name = camera.activeCameraName();
    REQUIRE(runtime_name.starts_with("__pelican_runtime_free_camera"));
    const auto *runtime = camera.sceneCameraController(runtime_name);
    REQUIRE(runtime != nullptr);
    REQUIRE(runtime->type == Camera::SceneCameraControllerType::Orbit);
    REQUIRE(runtime->target.empty());
    REQUIRE(runtime->distance == Catch::Approx(7.0f));
    REQUIRE(runtime->sensitivity == Catch::Approx(2.0f));

    const auto *authored_fly = camera.sceneCameraController("FlyCam");
    REQUIRE(authored_fly != nullptr);
    REQUIRE(authored_fly->type == Camera::SceneCameraControllerType::Fly);
    REQUIRE(authored_fly->speed == Catch::Approx(6.5f));
    REQUIRE(authored_fly->sensitivity == Catch::Approx(1.25f));

    REQUIRE(GET_MODULE(ProjectBasicConfig).sceneDataJson().find(runtime_name) ==
            std::string::npos);
    REQUIRE(readText(scene_path) == authored);
}

} // namespace Pelican
