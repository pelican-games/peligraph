#include "basicconfig.hpp"
#include "../log.hpp"
#include "projectsrc.hpp"
#include <fstream>
#include <nlohmann/json.hpp>

namespace Pelican {

ProjectBasicConfig::ProjectBasicConfig() {
    const auto proj_data = GET_MODULE(ProjectSource).loadSource();
    const auto parsed = nlohmann::json::parse(proj_data);

    const auto config = parsed.at("basic_config");
    window_title = config.at("window_title");
    initial_window_size.width = config.at("window_size").at("width");
    initial_window_size.height = config.at("window_size").at("height");
    initial_fullscr_state = config.at("fullscreen");
    framerate_target = config.at("framerate");

    const auto camera = config.at("camera");
    camera_prop.fov_y = camera.at("fov_y");
    camera_prop.near = camera.at("near");
    camera_prop.far = camera.at("far");
    const auto camera_up = camera.at("up");
    camera_prop.up = {camera_up[0], camera_up[1], camera_up[2]};

    default_scene_id = config.at("default_scene_id");
    {
        const std::string scene_data_json_path = config.at("scene_data_json");

        const auto sz = std::filesystem::file_size(scene_data_json_path);
        std::ifstream f{scene_data_json_path, std::ios_base::binary};
        std::string loaded_data;
        loaded_data.resize(sz, '\0');
        f.read(loaded_data.data(), sz);

        scene_data_json = loaded_data;
    }

    LOG_INFO(logger, "project basic config loaded");
}

std::string ProjectBasicConfig::windowTitle() const { return window_title; }
ProjectBasicConfig::window_size ProjectBasicConfig::initialWindowSize() const { return initial_window_size; }
bool ProjectBasicConfig::initialFullScreenState() const { return initial_fullscr_state; }

float ProjectBasicConfig::framerateTarget() const { return framerate_target; }

ProjectBasicConfig::InitialCameraProperty ProjectBasicConfig::initailCameraProperty() const { return camera_prop; }

std::string ProjectBasicConfig::defaultSceneId() const { return default_scene_id; }
std::string ProjectBasicConfig::sceneDataJson() const { return scene_data_json; }

} // namespace Pelican
