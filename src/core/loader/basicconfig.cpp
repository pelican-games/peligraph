#include "basicconfig.hpp"
#include "../startup.hpp"
#include "../log.hpp"
#include "../watch/contentdigest.hpp"
#include "pathresolver.hpp"
#include "projectsrc.hpp"
#include "../../project/assetdataformat.hpp"
#include "../../project/projectformat.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <picosha2.h>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace Pelican {

namespace {

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

glm::vec3 parseBasicCameraUp(const JsonLoader &loader) {
    const auto value = loader.getVal("basic_config/camera/up");
    if (!value.is_array() || value.size() != 3) {
        throw std::runtime_error(
            "basic_config.camera.up must be an array of exactly 3 numbers");
    }

    glm::vec3 result{};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto &component = value.at(i);
        if (!component.is_number()) {
            throw std::runtime_error(
                "basic_config.camera.up must be an array of exactly 3 numbers");
        }
        const auto decoded = component.get<double>();
        if (!std::isfinite(decoded) ||
            std::abs(decoded) > std::numeric_limits<float>::max()) {
            throw std::runtime_error(
                "basic_config.camera.up components must be finite float values");
        }
        result[static_cast<glm::vec3::length_type>(i)] =
            static_cast<float>(decoded);
    }
    if (std::hypot(static_cast<double>(result.x),
                   static_cast<double>(result.y),
                   static_cast<double>(result.z)) == 0.0) {
        throw std::runtime_error("basic_config.camera.up must be non-zero");
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

std::string projectBasicConfigSource(const ProjectSource &source) {
    if (!source.hasProjectSource()) {
        return "{}";
    }

    const auto parsed = parseProjectEnvelopeText(
        source.loadProjectSource(),
        {.ignore_engine_version = source.ignoresEngineVersion()});
    for (const auto &warning : parsed.warnings) {
        LOG_WARNING(logger, "{}", warning);
    }
    return nlohmann::json{{"basic_config", parsed.envelope.basic_config}}.dump();
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
    (void)parseAssetDataFormatJson(json);

    auto &resolver = GET_MODULE(PathResolver);
    for (auto &model : json.at("models")) {
        if (model.is_object() && model.contains("path") && model.at("path").is_string()) {
            model["path"] =
                resolveExistingModelReferenceString(resolver, model.at("path").get<std::string>());
        }
    }
    return json.dump();
}

std::string sceneBytesDigest(std::string_view bytes) {
    return picosha2::hash256_hex_string(bytes.begin(), bytes.end());
}

std::filesystem::path sceneFilePath(std::string_view reference) {
    try {
        return GET_MODULE(PathResolver).resolveExistingFile(reference);
    } catch (const std::exception &error) {
        throw SceneSaveError{
            SceneSaveErrorCode::Unavailable,
            "scene source is not a writable project file: " +
                std::string{error.what()},
        };
    }
}

std::string stableDiskDigest(const std::filesystem::path &path) {
    const auto digest = watch::readStableContentDigest(path);
    switch (digest.status) {
    case watch::DigestReadStatus::stable:
        return digest.sha256;
    case watch::DigestReadStatus::missing:
        throw SceneSaveError{SceneSaveErrorCode::ExternalModification,
                             "scene source was removed outside the editor"};
    case watch::DigestReadStatus::retry:
    case watch::DigestReadStatus::cancelled:
    case watch::DigestReadStatus::error:
        throw SceneSaveError{
            SceneSaveErrorCode::IoFailure,
            "failed to read a stable scene source digest: " + digest.error,
        };
    }
    throw SceneSaveError{SceneSaveErrorCode::IoFailure,
                         "failed to read scene source digest"};
}

std::string readSceneFile(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input.is_open()) {
        throw SceneSaveError{SceneSaveErrorCode::IoFailure,
                             "failed to open temporary scene file"};
    }
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0) {
        throw SceneSaveError{SceneSaveErrorCode::IoFailure,
                             "failed to size temporary scene file"};
    }
    std::string bytes(static_cast<std::size_t>(end), '\0');
    input.seekg(0, std::ios::beg);
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input && !bytes.empty()) {
        throw SceneSaveError{SceneSaveErrorCode::IoFailure,
                             "failed to read temporary scene file"};
    }
    return bytes;
}

std::filesystem::path temporaryScenePath(
    const std::filesystem::path &destination) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto nonce = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    for (std::uint64_t attempt = 0; attempt < 128; ++attempt) {
        auto candidate = destination.parent_path() /
            (destination.filename().string() + ".pelican-save-" +
             std::to_string(nonce) + "-" +
             std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) +
             ".tmp");
        std::error_code error;
        if (!std::filesystem::exists(candidate, error) && !error) {
            return candidate;
        }
    }
    throw SceneSaveError{SceneSaveErrorCode::IoFailure,
                         "failed to reserve a temporary scene file name"};
}

class TemporarySceneFile {
    std::filesystem::path path_;

  public:
    explicit TemporarySceneFile(std::filesystem::path path)
        : path_{std::move(path)} {}
    ~TemporarySceneFile() {
        if (path_.empty()) return;
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    const std::filesystem::path &path() const noexcept { return path_; }
};

void writeAndFlushSceneFile(const std::filesystem::path &path,
                            std::string_view bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output.is_open()) {
        throw SceneSaveError{SceneSaveErrorCode::IoFailure,
                             "failed to open temporary scene file for writing"};
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output) {
        throw SceneSaveError{SceneSaveErrorCode::IoFailure,
                             "failed to write and flush temporary scene file"};
    }
    output.close();
    if (!output) {
        throw SceneSaveError{SceneSaveErrorCode::IoFailure,
                             "failed to close temporary scene file"};
    }
}

void replaceSceneFileAtomically(const std::filesystem::path &temporary,
                                const std::filesystem::path &destination) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const std::error_code error{static_cast<int>(GetLastError()),
                                    std::system_category()};
        throw SceneSaveError{SceneSaveErrorCode::IoFailure,
                             "atomic scene replace failed: " + error.message()};
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        throw SceneSaveError{SceneSaveErrorCode::IoFailure,
                             "atomic scene replace failed: " + error.message()};
    }
#endif
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
    camera_prop.up = parseBasicCameraUp(loader);

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

void ProjectBasicConfig::publishPreparedSceneDocument(
    AuthoringSceneDocument &document) const noexcept {
    const auto next_object_id = document.next_authoring_object_id_value_;
    const auto revision = document.revision_.value;
    scene_document->swap(document);
    next_scene_revision = revision + 1U;
    next_authoring_object_id = next_object_id;
}

const AuthoringSceneDocument &ProjectBasicConfig::sceneDocument() const {
    if (!scene_document) {
        auto bytes = GET_MODULE(PathResolver).loadText(scene_data_json_ref);
        auto baseline = sceneBytesDigest(bytes);
        publishSceneDocument(bytes);
        scene_baseline_digest = std::move(baseline);
    }
    return *scene_document;
}

void ProjectBasicConfig::updateSceneDocument(std::string_view scene_v1_bytes) {
    publishSceneDocument(scene_v1_bytes);
}

void ProjectBasicConfig::invalidateSceneDocument() noexcept {
    scene_document.reset();
    scene_baseline_digest.reset();
}

SceneRevision ProjectBasicConfig::importSceneDocument(
    std::string_view scene_v1_bytes, const std::function<void()> &reload) {
    if (!reload) {
        throw std::invalid_argument("scene snapshot import requires a reload callback");
    }
    (void)sceneDocument();
    if (next_scene_revision == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("SceneRevision space exhausted");
    }

    // Parse, semantic validation, fresh AuthoringObjectId allocation, and all
    // candidate allocation complete before the live cache is touched.
    auto candidate = AuthoringSceneDocument::load(
        scene_v1_bytes, SceneRevision{next_scene_revision},
        next_authoring_object_id);
    const auto committed_revision = candidate.revision();
    const auto previous_next_revision = next_scene_revision;
    const auto previous_next_object_id = next_authoring_object_id;
    if (scene_import_fault == SceneImportFaultPoint::AfterCandidatePrepare) {
        throw std::runtime_error(
            "injected scene import fault after candidate prepare");
    }

    // This is the same allocation-free document swap used by SAVE0. The disk
    // baseline is deliberately retained: snapshot import changes only the
    // in-memory scene source.
    publishPreparedSceneDocument(candidate);
    try {
        if (scene_import_fault == SceneImportFaultPoint::AfterPublication) {
            throw std::runtime_error(
                "injected scene import fault after publication");
        }
        reload();
    } catch (...) {
        // candidate owns the old live document after the first swap. Restore
        // it without parse/allocation and rewind both allocation authorities.
        scene_document->swap(candidate);
        next_scene_revision = previous_next_revision;
        next_authoring_object_id = previous_next_object_id;
        throw;
    }
    return committed_revision;
}

SceneSaveResult ProjectBasicConfig::saveSceneDocument() {
    const auto &source = sceneDocument();
    if (!scene_baseline_digest) {
        throw SceneSaveError{SceneSaveErrorCode::Unavailable,
                             "scene source has no baseline disk digest"};
    }
    if (next_scene_revision == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("SceneRevision space exhausted");
    }

    // This is deliberately the only serialization call in SAVE0. Everything
    // below consumes these exact semantic bytes.
    auto semantic_bytes = source.encodeSemantic();
    const auto inject = [&](SceneSaveFaultPoint point) {
        if (scene_save_fault == point) {
            throw std::runtime_error("injected scene save fault at " +
                                     std::to_string(static_cast<unsigned>(point)));
        }
    };
    inject(SceneSaveFaultPoint::AfterEncode);

    const auto destination = sceneFilePath(scene_data_json_ref);
    const auto disk_digest = stableDiskDigest(destination);
    if (disk_digest != *scene_baseline_digest) {
        throw SceneSaveError{
            SceneSaveErrorCode::ExternalModification,
            "scene source changed outside the editor (external_modification)",
        };
    }
    inject(SceneSaveFaultPoint::AfterDiskDigest);

    TemporarySceneFile temporary{temporaryScenePath(destination)};
    writeAndFlushSceneFile(temporary.path(), semantic_bytes);
    inject(SceneSaveFaultPoint::AfterTemporaryWrite);

    const auto temporary_bytes = readSceneFile(temporary.path());
    if (temporary_bytes != semantic_bytes) {
        throw SceneSaveError{SceneSaveErrorCode::IoFailure,
                             "temporary scene bytes differ after flush"};
    }
    const auto validated = AuthoringSceneDocument::load(
        temporary_bytes, source.revision(), source.next_authoring_object_id_value_);
    if (validated.rawJson() != source.rawJson()) {
        throw SceneSaveError{SceneSaveErrorCode::IoFailure,
                             "temporary scene semantic validation changed the document"};
    }
    inject(SceneSaveFaultPoint::AfterTemporaryValidation);

    auto next_document = source.stage(source.rawJson(),
                                      SceneRevision{next_scene_revision});
    const auto committed_revision = next_document.revision();
    auto next_baseline_digest = sceneBytesDigest(semantic_bytes);
    SceneSaveResult result{
        .scene_revision = committed_revision,
        .digest = next_baseline_digest,
        .byte_count = semantic_bytes.size(),
    };
    inject(SceneSaveFaultPoint::AfterCachePrepare);

    // Close the practical TOCTOU window made by temporary preparation. The
    // final fault point is after this check and immediately before replace.
    if (stableDiskDigest(destination) != *scene_baseline_digest) {
        throw SceneSaveError{
            SceneSaveErrorCode::ExternalModification,
            "scene source changed during save (external_modification)",
        };
    }
    inject(SceneSaveFaultPoint::BeforeReplace);

    replaceSceneFileAtomically(temporary.path(), destination);

    // Publication after file replacement is allocation/decode/I/O-free. RPC
    // and ImGui call SAVE0 on the engine thread, so no reader can enter this
    // short no-throw interval and observe a mixed file/cache/revision state.
    publishPreparedSceneDocument(next_document);
    scene_baseline_digest->swap(next_baseline_digest);
    return result;
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

std::string ProjectBasicConfig::renderingConfigReference() const {
    return rendering_config_json_ref;
}

void ProjectBasicConfig::publishRenderingConfigJson(
    std::string prepared_json) noexcept {
    rendering_config_json =
        std::move(prepared_json);
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
