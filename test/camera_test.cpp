#include "../src/core/container.hpp"
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
          "name": "LegacyCam",
          "components": [
            {"name": "camera", "fov_y": 60.0, "near": 0.2, "far": 20.0}
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
            {"name": "camera", "type": "orthographic", "xmag": 2.0, "ymag": 3.0, "znear": 0.5, "zfar": 50.0}
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

TEST_CASE("Camera parses glTF parameters, legacy aliases, and orthographic projection", "[camera]") {
    ensureLogger();
    Sandbox sandbox;
    writeText(sandbox.root / "scene.json", cameraSceneJson());

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(ProjectSource).setProjectData(projectJson().dump());

    auto &camera = GET_MODULE(Camera);
    REQUIRE(camera.hasSceneCamera("LegacyCam"));
    REQUIRE(camera.hasSceneCamera("GltfCam"));
    REQUIRE(camera.hasSceneCamera("OrthoCam"));

    camera.setActiveCamera("LegacyCam");
    auto spec = camera.getProjectionSpec();
    REQUIRE(spec.kind == CameraProjectionKind::Perspective);
    REQUIRE(spec.yfov == Catch::Approx(pi / 3.0f));
    REQUIRE(spec.znear == Catch::Approx(0.2f));
    REQUIRE(spec.zfar == Catch::Approx(20.0f));

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

    REQUIRE_THROWS_WITH(camera.setActiveCamera("MissingCam"),
                        Catch::Matchers::ContainsSubstring("MissingCam"));
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

} // namespace Pelican
