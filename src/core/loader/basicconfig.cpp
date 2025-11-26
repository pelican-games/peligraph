#include "basicconfig.hpp"
#include "../log.hpp"
#include "projectsrc.hpp"
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>

#include "battery/embed.hpp"

namespace Pelican {

namespace {

struct JsonHelper {
    const nlohmann::json _json;

    JsonHelper(std::string_view str) : _json{nlohmann::json::parse(str)} {}

    std::optional<nlohmann::json> getVal(const std::string_view path, const nlohmann::json &json) {
        const auto d_pos = path.find('/');
        if (d_pos == std::string_view::npos) {
            auto it = json.find(path);
            if (it == json.end())
                return std::nullopt;
            return std::make_optional(it.value());
        } else {
            auto key = path.substr(0, d_pos);
            auto it = json.find(key);
            if (it == json.end())
                return std::nullopt;
            return getVal(path.substr(d_pos + 1), it.value());
        }
    };
    std::optional<nlohmann::json> getVal(const std::string_view path) { return getVal(path, _json); }
};

struct JsonLoader {
    JsonHelper json;
    JsonHelper default_json;

    JsonLoader(std::string src, std::string src_default) : json{src}, default_json{src_default} {}
    nlohmann::json getVal(std::string_view path) {
        auto dat = json.getVal(path);
        if (dat.has_value())
            return *dat;
        auto dat2 = default_json.getVal(path);
        if (dat2.has_value())
            return *dat2;
        throw std::runtime_error("config not found: " + std::string(path));
    }
};

} // namespace

ProjectBasicConfig::ProjectBasicConfig() {
    JsonLoader loader{
        GET_MODULE(ProjectSource).loadSource(),
        b::embed<"default_config.json">().str(),
    };

    window_title = loader.getVal("basic_config/window_title");
    initial_window_size.width = loader.getVal("basic_config/window_size/width");
    initial_window_size.height = loader.getVal("basic_config/window_size/height");
    initial_fullscr_state = loader.getVal("basic_config/fullscreen");
    framerate_target = loader.getVal("basic_config/framerate");

    const auto camera = loader.getVal("basic_config/camera");
    camera_prop.fov_y = loader.getVal("basic_config/camera/fov_y");
    camera_prop.near = loader.getVal("basic_config/camera/near");
    camera_prop.far = loader.getVal("basic_config/camera/far");
    const auto camera_up = loader.getVal("basic_config/camera/up");
    camera_prop.up = {camera_up[0], camera_up[1], camera_up[2]};

    default_scene_id = loader.getVal("basic_config/default_scene_id");
    {
        const std::string scene_data_json_path = loader.getVal("basic_config/scene_data_json");

        const auto sz = std::filesystem::file_size(scene_data_json_path);
        std::ifstream f{scene_data_json_path, std::ios_base::binary};
        std::string loaded_data;
        loaded_data.resize(sz, '\0');
        f.read(loaded_data.data(), sz);

        scene_data_json = loaded_data;
    }
    {
        const std::string asset_data_json_path = loader.getVal("basic_config/asset_data_json");

        const auto sz = std::filesystem::file_size(asset_data_json_path);
        std::ifstream f{asset_data_json_path, std::ios_base::binary};
        std::string loaded_data;
        loaded_data.resize(sz, '\0');
        f.read(loaded_data.data(), sz);

        asset_data_json = loaded_data;
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
std::string ProjectBasicConfig::assetDataJson() const { return asset_data_json; }

} // namespace Pelican
