#include "basicconfig.hpp"
#include "../startup.hpp"
#include "../log.hpp"
#include "pathresolver.hpp"
#include "projectsrc.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <limits>
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

    std::optional<nlohmann::json> getOptionalVal(std::string_view path) const {
        auto dat = cli_json.getVal(path);
        if (dat.has_value())
            return dat;
        auto dat2 = project_json.getVal(path);
        if (dat2.has_value())
            return dat2;
        auto dat3 = default_json.getVal(path);
        if (dat3.has_value())
            return dat3;
        return std::nullopt;
    }
};

void rejectUnknownObjectFields(const JsonLoader &loader, std::string_view path,
                               std::string_view display_name,
                               std::initializer_list<std::string_view> allowed) {
    const JsonHelper *sources[] = {&loader.cli_json, &loader.project_json, &loader.default_json};
    for (const auto *source : sources) {
        const auto object = source->getVal(path);
        if (!object) continue;
        if (!object->is_object()) {
            throw std::runtime_error(std::string{display_name} + " must be an object");
        }
        for (const auto &[field, value] : object->items()) {
            (void)value;
            if (std::find(allowed.begin(), allowed.end(), field) == allowed.end()) {
                throw std::runtime_error(std::string{display_name} +
                                         " has unknown field '" + field + "'");
            }
        }
    }
}

std::string cameraPath(std::string_view field) {
    return "basic_config/camera/" + std::string{field};
}

std::optional<nlohmann::json> getCameraParam(const JsonLoader &loader, std::string_view field) {
    const JsonHelper *sources[] = {&loader.cli_json, &loader.project_json, &loader.default_json};
    const auto path = cameraPath(field);

    for (const auto *source : sources) {
        if (auto value = source->getVal(path)) {
            return value;
        }
    }
    return std::nullopt;
}

void rejectDeprecatedCameraFields(const JsonLoader &loader) {
    struct DeprecatedField {
        std::string_view name;
        std::string_view replacement;
        std::string_view suffix;
    };
    static constexpr DeprecatedField deprecated_fields[] = {
        {"fov_y", "yfov", " (radians)"},
        {"near", "znear", ""},
        {"far", "zfar", ""},
    };
    const JsonHelper *sources[] = {&loader.cli_json, &loader.project_json, &loader.default_json};
    for (const auto &field : deprecated_fields) {
        for (const auto *source : sources) {
            if (source->getVal(cameraPath(field.name))) {
                throw std::runtime_error("basic_config.camera field '" + std::string{field.name} +
                                         "' is not supported in v1; use '" +
                                         std::string{field.replacement} + "'" + std::string{field.suffix});
            }
        }
    }
}

float numberFromJson(const nlohmann::json &value, std::string_view field) {
    if (!value.is_number()) {
        throw std::runtime_error("basic_config.camera field '" + std::string{field} + "' must be numeric");
    }
    return value.get<float>();
}

std::uint64_t seedFromJson(const nlohmann::json &value) {
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value >= 0) {
            return static_cast<std::uint64_t>(signed_value);
        }
    }
    throw std::runtime_error("basic_config.seed must be a non-negative integer");
}

float requireCameraNumber(const JsonLoader &loader, std::string_view field) {
    const auto param = getCameraParam(loader, field);
    if (!param) {
        throw std::runtime_error("basic_config.camera requires numeric field '" + std::string{field} + "'");
    }
    return numberFromJson(*param, field);
}

std::optional<float> optionalCameraNumber(const JsonLoader &loader, std::string_view field) {
    const auto param = getCameraParam(loader, field);
    if (!param) {
        return std::nullopt;
    }
    return numberFromJson(*param, field);
}

std::optional<std::string> optionalCameraType(const JsonLoader &loader) {
    const auto param = getCameraParam(loader, "type");
    if (!param) {
        return std::nullopt;
    }
    if (!param->is_string()) {
        throw std::runtime_error("basic_config.camera field 'type' must be a string");
    }
    return param->get<std::string>();
}

std::string requireCameraString(const JsonLoader &loader, std::string_view field) {
    const auto param = getCameraParam(loader, field);
    if (!param || !param->is_string()) {
        throw std::runtime_error("basic_config.camera field '" + std::string{field} +
                                 "' must be a string");
    }
    return param->get<std::string>();
}

CameraSpritePolicySpec parseBasicCameraSpritePolicy(const JsonLoader &loader) {
    CameraSpritePolicySpec result;
    const auto pixel_perfect = requireCameraString(loader, "sprite/pixel_perfect");
    if (pixel_perfect == "off") result.pixel_perfect = CameraPixelPerfectMode::off;
    else if (pixel_perfect == "strict") result.pixel_perfect = CameraPixelPerfectMode::strict;
    else {
        throw std::runtime_error(
            "basic_config.camera field 'sprite/pixel_perfect' must be 'off' or 'strict'");
    }

    const auto sort = requireCameraString(loader, "sprite/sort");
    if (sort == "z") result.sort = CameraSpriteSortPolicy::z;
    else if (sort == "y_down") result.sort = CameraSpriteSortPolicy::y_down;
    else if (sort == "declaration") result.sort = CameraSpriteSortPolicy::declaration;
    else {
        throw std::runtime_error(
            "basic_config.camera field 'sprite/sort' must be 'z', 'y_down', or 'declaration'");
    }
    return result;
}

CameraProjectionSpec parseBasicCameraProjection(const JsonLoader &loader) {
    rejectDeprecatedCameraFields(loader);
    CameraProjectionSpec projection;

    const auto type = optionalCameraType(loader);
    const auto has_ortho_fields = optionalCameraNumber(loader, "xmag").has_value() ||
                                  optionalCameraNumber(loader, "ymag").has_value();
    if (type && *type != "perspective" && *type != "orthographic") {
        throw std::runtime_error("basic_config.camera type must be 'perspective' or 'orthographic'");
    }

    if ((type && *type == "orthographic") || (!type && has_ortho_fields)) {
        projection.kind = CameraProjectionKind::Orthographic;
        projection.xmag = requireCameraNumber(loader, "xmag");
        projection.ymag = requireCameraNumber(loader, "ymag");
        projection.znear = requireCameraNumber(loader, "znear");
        projection.zfar = requireCameraNumber(loader, "zfar");
        return projection;
    }

    projection.kind = CameraProjectionKind::Perspective;
    projection.yfov = requireCameraNumber(loader, "yfov");
    projection.znear = requireCameraNumber(loader, "znear");
    projection.zfar = requireCameraNumber(loader, "zfar");
    projection.aspect = optionalCameraNumber(loader, "aspect");
    return projection;
}

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
    if (version != supported_project_version) {
        throw std::runtime_error("project.json version must be exactly 1");
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

std::string resolveExistingModelReferenceString(PathResolver &resolver, const std::string &ref) {
    const auto resolved = resolver.resolveExistingFileReference(ref);
    if (const auto *fragment = std::get_if<ResolvedPathFragment>(&resolved)) {
        return fragment->path.string() + "#" + fragment->fragment.kind + "/" +
               fragment->fragment.path;
    }
    return std::get<std::filesystem::path>(resolved).string();
}

std::string rewriteAssetPaths(std::string data) {
    auto json = nlohmann::json::parse(data);
    if (!json.contains("models") || !json.at("models").is_array()) {
        return data;
    }

    auto &resolver = GET_MODULE(PathResolver);
    for (auto &model : json.at("models")) {
        if (model.is_object() && model.contains("path") && model.at("path").is_string()) {
            model["path"] =
                resolveExistingModelReferenceString(resolver, model.at("path").get<std::string>());
        }
    }
    return json.dump();
}

} // namespace

ProjectBasicConfig::ProjectBasicConfig() {
    StartupPhaseTimer startup_timer{&StartupMetrics::addConfig};
    const auto &source = GET_MODULE(ProjectSource);
    project_source = source.hasProjectSource();
    JsonLoader loader{
        source.loadSource(),
        projectBasicConfigSource(source),
        GET_MODULE(PathResolver).loadText("engine://default_config.json"),
    };
    rejectUnknownObjectFields(loader, "basic_config/sprite", "basic_config.sprite",
                              {"pixels_per_unit"});
    rejectUnknownObjectFields(loader, "basic_config/camera/sprite", "basic_config.camera.sprite",
                              {"pixel_perfect", "sort"});

    window_title = loader.getVal("basic_config/window_title");
    initial_window_size.width = loader.getVal("basic_config/window_size/width");
    initial_window_size.height = loader.getVal("basic_config/window_size/height");
    initial_fullscr_state = loader.getVal("basic_config/fullscreen");
    framerate_target = loader.getVal("basic_config/framerate");
    deterministic_seed = seedFromJson(loader.getVal("basic_config/seed"));

    const auto ppu = loader.getVal("basic_config/sprite/pixels_per_unit");
    if (!ppu.is_number()) {
        throw std::runtime_error("basic_config.sprite.pixels_per_unit must be numeric");
    }
    sprite_pixels_per_unit = ppu.get<float>();
    if (!std::isfinite(sprite_pixels_per_unit) || sprite_pixels_per_unit <= 0.0f) {
        throw std::runtime_error("basic_config.sprite.pixels_per_unit must be finite and positive");
    }

    camera_prop.projection = parseBasicCameraProjection(loader);
    camera_prop.sprite = parseBasicCameraSpritePolicy(loader);
    const auto camera_up = loader.getVal("basic_config/camera/up");
    camera_prop.up = {camera_up[0], camera_up[1], camera_up[2]};

    default_scene_id = loader.getVal("basic_config/default_scene_id");
    rendering_config_json_ref = loader.getVal("basic_config/rendering_config_json");
    default_rendering_pass = loader.getVal("basic_config/default_rendering_pass");
    ui_config_json_ref = loader.getVal("basic_config/ui_config_json");
    scene_data_json_ref = loader.getVal("basic_config/scene_data_json");
    asset_data_json_ref = loader.getVal("basic_config/asset_data_json");
    if (const auto input_actions_ref = loader.getOptionalVal("basic_config/input_actions_json")) {
        if (!input_actions_ref->is_string()) {
            throw std::runtime_error("basic_config.input_actions_json must be a string");
        }
        input_actions_json_ref = input_actions_ref->get<std::string>();
    }
    if (const auto profiles = loader.getOptionalVal("basic_config/input_profiles")) {
        if (!profiles->is_object()) {
            throw std::runtime_error("basic_config.input_profiles must be an object");
        }
        for (const auto &[name, value] : profiles->items()) {
            if (name.empty() || !value.is_string() || value.get<std::string>().empty()) {
                throw std::runtime_error("basic_config.input_profiles entries must have named string references");
            }
            input_profile_json_refs.emplace(name, value.get<std::string>());
        }
    }
    if (const auto profile = loader.getOptionalVal("basic_config/input_profile")) {
        if (!profile->is_string() || profile->get<std::string>().empty()) {
            throw std::runtime_error("basic_config.input_profile must be a non-empty string");
        }
        default_input_profile = profile->get<std::string>();
    }

    LOG_INFO(logger, "project basic config loaded");
}

std::string ProjectBasicConfig::windowTitle() const { return window_title; }
ProjectBasicConfig::window_size ProjectBasicConfig::initialWindowSize() const { return initial_window_size; }
bool ProjectBasicConfig::initialFullScreenState() const { return initial_fullscr_state; }

float ProjectBasicConfig::framerateTarget() const { return framerate_target; }
std::uint64_t ProjectBasicConfig::seed() const { return deterministic_seed; }
float ProjectBasicConfig::spritePixelsPerUnit() const { return sprite_pixels_per_unit; }

ProjectBasicConfig::InitialCameraProperty ProjectBasicConfig::initailCameraProperty() const { return camera_prop; }

std::string ProjectBasicConfig::defaultSceneId() const { return default_scene_id; }

void ProjectBasicConfig::publishSceneDocument(std::string_view scene_v1_bytes) const {
    if (next_scene_revision == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("SceneRevision space exhausted");
    }
    auto candidate = AuthoringSceneDocument::load(scene_v1_bytes, SceneRevision{next_scene_revision},
                                                   next_authoring_object_id);
    const auto next_object_id = candidate.next_authoring_object_id_value_;
    scene_document = std::move(candidate);
    ++next_scene_revision;
    next_authoring_object_id = next_object_id;
}

const AuthoringSceneDocument &ProjectBasicConfig::sceneDocument() const {
    if (!scene_document) {
        publishSceneDocument(GET_MODULE(PathResolver).loadText(scene_data_json_ref));
    }
    return *scene_document;
}

void ProjectBasicConfig::updateSceneDocument(std::string_view scene_v1_bytes) {
    publishSceneDocument(scene_v1_bytes);
}

void ProjectBasicConfig::invalidateSceneDocument() noexcept {
    scene_document.reset();
}

std::string ProjectBasicConfig::sceneDataJson() const {
    return sceneDocument().encodeSemantic();
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
        ui_config_json = GET_MODULE(PathResolver).loadText(ui_config_json_ref);
    }
    return *ui_config_json;
}

std::optional<std::string> ProjectBasicConfig::inputActionsJson() const {
    if (!input_actions_json_ref) {
        return std::nullopt;
    }
    if (!input_actions_json) {
        input_actions_json = GET_MODULE(PathResolver).loadText(*input_actions_json_ref);
    }
    return input_actions_json;
}

std::unordered_map<std::string, std::string> ProjectBasicConfig::inputProfileJsons() const {
    for (const auto &[name, reference] : input_profile_json_refs) {
        if (!input_profile_jsons.contains(name)) {
            input_profile_jsons.emplace(name, GET_MODULE(PathResolver).loadText(reference));
        }
    }
    return input_profile_jsons;
}

std::optional<std::string> ProjectBasicConfig::defaultInputProfile() const {
    return default_input_profile;
}

} // namespace Pelican
