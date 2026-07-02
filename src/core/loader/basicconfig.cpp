#include "basicconfig.hpp"
#include "../log.hpp"
#include "pathresolver.hpp"
#include "projectsrc.hpp"
#include <algorithm>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace Pelican {

namespace {

constexpr std::string_view project_schema = "pelican.project";
constexpr int supported_project_version = 1;
constexpr std::string_view current_engine_version = "0.1.0";

struct JsonHelper {
    const nlohmann::json _json;

    JsonHelper(std::string_view str) : _json{nlohmann::json::parse(str)} {}

    std::optional<nlohmann::json> getVal(const std::string_view path, const nlohmann::json &json) const {
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
    std::optional<nlohmann::json> getVal(const std::string_view path) const { return getVal(path, _json); }
};

struct JsonLoader {
    JsonHelper cli_json;
    JsonHelper project_json;
    JsonHelper default_json;

    JsonLoader(std::string cli_src, std::string project_src, std::string default_src)
        : cli_json{cli_src}, project_json{project_src}, default_json{default_src} {}

    nlohmann::json getVal(std::string_view path) const {
        auto dat = cli_json.getVal(path);
        if (dat.has_value())
            return *dat;
        auto dat2 = project_json.getVal(path);
        if (dat2.has_value())
            return *dat2;
        auto dat3 = default_json.getVal(path);
        if (dat3.has_value())
            return *dat3;
        throw std::runtime_error("config not found: " + std::string{path});
    }
};

std::vector<int> parseVersion(std::string_view version) {
    std::vector<int> parts;
    std::string token;
    std::istringstream stream{std::string{version}};
    while (std::getline(stream, token, '.')) {
        if (token.empty()) {
            throw std::runtime_error("invalid version string: " + std::string{version});
        }
        size_t parsed_chars = 0;
        const int value = std::stoi(token, &parsed_chars, 10);
        if (parsed_chars != token.size() || value < 0) {
            throw std::runtime_error("invalid version string: " + std::string{version});
        }
        parts.push_back(value);
    }
    return parts;
}

int compareVersions(std::string_view lhs, std::string_view rhs) {
    auto lhs_parts = parseVersion(lhs);
    auto rhs_parts = parseVersion(rhs);
    const auto count = std::max(lhs_parts.size(), rhs_parts.size());
    lhs_parts.resize(count, 0);
    rhs_parts.resize(count, 0);
    for (size_t i = 0; i < count; ++i) {
        if (lhs_parts[i] < rhs_parts[i]) {
            return -1;
        }
        if (lhs_parts[i] > rhs_parts[i]) {
            return 1;
        }
    }
    return 0;
}

void validateProjectJson(const nlohmann::json &project, bool ignore_engine_version) {
    if (!project.is_object()) {
        throw std::runtime_error("project.json must be an object");
    }
    if (project.value("schema", std::string{}) != project_schema) {
        throw std::runtime_error("project.json schema is not supported");
    }
    if (!project.contains("version") || !project.at("version").is_number_integer()) {
        throw std::runtime_error("project.json requires numeric version");
    }
    const auto version = project.at("version").get<int>();
    if (version > supported_project_version) {
        throw std::runtime_error("project.json version is newer than this engine supports");
    }

    if (!project.contains("engine_min_version")) {
        return;
    }
    if (!project.at("engine_min_version").is_string()) {
        throw std::runtime_error("project.json engine_min_version must be a string");
    }
    const auto min_version = project.at("engine_min_version").get<std::string>();
    if (compareVersions(min_version, current_engine_version) <= 0) {
        return;
    }

    const auto message = "project.json engine_min_version " + min_version +
                         " is newer than this engine (" + std::string{current_engine_version} + ")";
    if (!ignore_engine_version) {
        throw std::runtime_error(message);
    }
    LOG_WARNING(logger, "{}; continuing because --ignore-engine-version was specified", message);
}

std::string projectBasicConfigSource(const ProjectSource &source) {
    if (!source.hasProjectSource()) {
        return "{}";
    }

    auto project = nlohmann::json::parse(source.loadProjectSource());
    validateProjectJson(project, source.ignoresEngineVersion());
    return nlohmann::json{{"basic_config", project.value("basic_config", nlohmann::json::object())}}.dump();
}

std::string resolveExistingFileString(PathResolver &resolver, const std::string &ref) {
    return resolver.resolveExistingFile(ref).string();
}

std::string rewriteAssetPaths(std::string data) {
    auto json = nlohmann::json::parse(data);
    if (!json.contains("models") || !json.at("models").is_array()) {
        return data;
    }

    auto &resolver = GET_MODULE(PathResolver);
    for (auto &model : json.at("models")) {
        if (model.is_object() && model.contains("path") && model.at("path").is_string()) {
            model["path"] = resolveExistingFileString(resolver, model.at("path").get<std::string>());
        }
    }
    return json.dump();
}

std::string rewriteUiPaths(std::string data) {
    auto json = nlohmann::json::parse(data);
    if (!json.contains("images") || !json.at("images").is_array()) {
        return data;
    }

    auto &resolver = GET_MODULE(PathResolver);
    for (auto &image : json.at("images")) {
        if (image.is_object() && image.contains("file") && image.at("file").is_string()) {
            image["file"] = resolveExistingFileString(resolver, image.at("file").get<std::string>());
        }
    }
    return json.dump();
}

} // namespace

ProjectBasicConfig::ProjectBasicConfig() {
    const auto &source = GET_MODULE(ProjectSource);
    project_source = source.hasProjectSource();
    JsonLoader loader{
        source.loadSource(),
        projectBasicConfigSource(source),
        GET_MODULE(PathResolver).loadText("engine://default_config.json"),
    };

    window_title = loader.getVal("basic_config/window_title");
    initial_window_size.width = loader.getVal("basic_config/window_size/width");
    initial_window_size.height = loader.getVal("basic_config/window_size/height");
    initial_fullscr_state = loader.getVal("basic_config/fullscreen");
    framerate_target = loader.getVal("basic_config/framerate");

    camera_prop.fov_y = loader.getVal("basic_config/camera/fov_y");
    camera_prop.near = loader.getVal("basic_config/camera/near");
    camera_prop.far = loader.getVal("basic_config/camera/far");
    const auto camera_up = loader.getVal("basic_config/camera/up");
    camera_prop.up = {camera_up[0], camera_up[1], camera_up[2]};

    default_scene_id = loader.getVal("basic_config/default_scene_id");
    rendering_config_json_ref = loader.getVal("basic_config/rendering_config_json");
    default_rendering_pass = loader.getVal("basic_config/default_rendering_pass");
    ui_config_json_ref = loader.getVal("basic_config/ui_config_json");
    scene_data_json_ref = loader.getVal("basic_config/scene_data_json");
    asset_data_json_ref = loader.getVal("basic_config/asset_data_json");

    LOG_INFO(logger, "project basic config loaded");
}

std::string ProjectBasicConfig::windowTitle() const { return window_title; }
ProjectBasicConfig::window_size ProjectBasicConfig::initialWindowSize() const { return initial_window_size; }
bool ProjectBasicConfig::initialFullScreenState() const { return initial_fullscr_state; }

float ProjectBasicConfig::framerateTarget() const { return framerate_target; }

ProjectBasicConfig::InitialCameraProperty ProjectBasicConfig::initailCameraProperty() const { return camera_prop; }

std::string ProjectBasicConfig::defaultSceneId() const { return default_scene_id; }
std::string ProjectBasicConfig::sceneDataJson() const {
    if (!scene_data_json) {
        scene_data_json = GET_MODULE(PathResolver).loadText(scene_data_json_ref);
    }
    return *scene_data_json;
}

std::string ProjectBasicConfig::assetDataJson() const {
    if (!asset_data_json) {
        asset_data_json = rewriteAssetPaths(GET_MODULE(PathResolver).loadText(asset_data_json_ref));
    }
    return *asset_data_json;
}

std::string ProjectBasicConfig::renderingConfigJson() const {
    if (!rendering_config_json) {
        rendering_config_json = GET_MODULE(PathResolver).loadText(rendering_config_json_ref);
    }
    return *rendering_config_json;
}

std::string ProjectBasicConfig::defaultRenderingPass() const { return default_rendering_pass; }

std::string ProjectBasicConfig::uiConfigJson() const {
    if (!ui_config_json) {
        ui_config_json = rewriteUiPaths(GET_MODULE(PathResolver).loadText(ui_config_json_ref));
    }
    return *ui_config_json;
}

} // namespace Pelican
