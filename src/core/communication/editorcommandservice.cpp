#include "editorcommandservice.hpp"

#include <algorithm>
#include <picosha2.h>
#include <type_traits>
#include <utility>

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

[[noreturn]] void invalidParams(const std::string &message) {
    throw EditorCommandError{EditorCommandErrorCode::InvalidParams, message};
}

const Json &requireObjectParams(const Json &params, std::string_view method) {
    if (!params.is_object()) invalidParams(std::string{method} + " params must be an object");
    return params;
}

void requireOnlyFields(const Json &params, std::initializer_list<std::string_view> fields,
                       std::string_view method) {
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (std::find(fields.begin(), fields.end(), it.key()) == fields.end()) {
            invalidParams(std::string{method} + " unknown field: " + it.key());
        }
    }
}

std::optional<std::string> optionalString(const Json &params, std::string_view field,
                                          std::string_view method) {
    const auto found = params.find(field);
    if (found == params.end()) return std::nullopt;
    if (!found->is_string() || found->get_ref<const std::string &>().empty()) {
        invalidParams(std::string{method} + " " + std::string{field} +
                      " must be a non-empty string");
    }
    return found->get<std::string>();
}

std::uint64_t exactUnsignedInteger(const Json &value, std::string_view path) {
    std::uint64_t result = 0;
    if (value.is_number_unsigned()) {
        result = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value < 0) invalidParams(std::string{path} + " must be non-negative");
        result = static_cast<std::uint64_t>(signed_value);
    } else {
        invalidParams(std::string{path} + " must be an integer token");
    }
    if (result > maxExactEditorJsonInteger) {
        invalidParams(std::string{path} + " exceeds 2^53-1");
    }
    return result;
}

std::string_view schemaTypeName(StructFieldType type) noexcept {
    switch (type) {
    case StructFieldType::I8: return "i8";
    case StructFieldType::I16: return "i16";
    case StructFieldType::I32: return "i32";
    case StructFieldType::I64: return "i64";
    case StructFieldType::U8: return "u8";
    case StructFieldType::U16: return "u16";
    case StructFieldType::U32: return "u32";
    case StructFieldType::U64: return "u64";
    case StructFieldType::F32: return "f32";
    case StructFieldType::F64: return "f64";
    case StructFieldType::Vec2: return "vec2";
    case StructFieldType::Vec3: return "vec3";
    case StructFieldType::Vec4: return "vec4";
    case StructFieldType::Quat: return "quat";
    case StructFieldType::String: return "string";
    case StructFieldType::Bool: return "bool";
    case StructFieldType::Enum: return "enum";
    }
    return "unknown";
}

OrderedJson schemaFieldJson(const StructFieldSchema &field) {
    OrderedJson result{{"name", field.name}, {"type", schemaTypeName(field.type)}};
    std::visit(
        [&](const auto &range) {
            using Range = std::decay_t<decltype(range)>;
            if constexpr (!std::same_as<Range, std::monostate>) {
                result["range"] = OrderedJson::array({range.first, range.second});
            }
        },
        field.range);
    if (!field.unit.empty()) result["unit"] = field.unit;
    return result;
}

EditorSceneTreeRequest parseSceneTreeRequest(const Json &params) {
    constexpr auto method = "scene_tree";
    requireObjectParams(params, method);
    requireOnlyFields(params, {"scene_id"}, method);
    return {.scene_id = optionalString(params, "scene_id", method)};
}

EditorGetComponentsRequest parseGetComponentsRequest(const Json &params) {
    constexpr auto method = "get_components";
    requireObjectParams(params, method);
    requireOnlyFields(params, {"scene_id", "authoring_object_id", "name"}, method);
    EditorGetComponentsRequest request{.scene_id = optionalString(params, "scene_id", method),
                                       .name = optionalString(params, "name", method)};
    if (const auto found = params.find("authoring_object_id"); found != params.end()) {
        const auto value = exactUnsignedInteger(*found, "get_components authoring_object_id");
        if (value == 0) invalidParams("get_components authoring_object_id must be non-zero");
        request.authoring_object_id = AuthoringObjectId{value};
    }
    if (request.authoring_object_id.has_value() == request.name.has_value()) {
        invalidParams("get_components requires exactly one of authoring_object_id or name");
    }
    return request;
}

EditorListAssetsRequest parseListAssetsRequest(const Json &params) {
    constexpr auto method = "list_assets";
    requireObjectParams(params, method);
    requireOnlyFields(params, {"store"}, method);
    return {.store = optionalString(params, "store", method)};
}

ExportSceneSnapshotRequestV1 parseExportSceneSnapshotRequest(const Json &params) {
    constexpr auto method = "export_scene_snapshot";
    requireObjectParams(params, method);
    requireOnlyFields(params, {"schema_version", "allow_pending"}, method);
    const auto version = params.find("schema_version");
    if (version == params.end()) invalidParams("export_scene_snapshot schema_version is required");
    ExportSceneSnapshotRequestV1 request;
    request.schema_version = exactUnsignedInteger(*version, "export_scene_snapshot schema_version");
    if (const auto allow = params.find("allow_pending"); allow != params.end()) {
        if (!allow->is_boolean()) invalidParams("export_scene_snapshot allow_pending must be a boolean");
        request.allow_pending = allow->get<bool>();
    }
    return request;
}

} // namespace

std::string_view editorCommandErrorCodeName(EditorCommandErrorCode code) noexcept {
    switch (code) {
    case EditorCommandErrorCode::InvalidParams: return "invalid_params";
    case EditorCommandErrorCode::SceneNotFound: return "scene_not_found";
    case EditorCommandErrorCode::ObjectNotFound: return "object_not_found";
    case EditorCommandErrorCode::UnsupportedSnapshotVersion: return "unsupported_snapshot_version";
    case EditorCommandErrorCode::SnapshotBusy: return "snapshot_busy";
    case EditorCommandErrorCode::SnapshotTooLarge: return "snapshot_too_large";
    }
    return "unknown_editor_error";
}

EditorCommandError::EditorCommandError(EditorCommandErrorCode code, const std::string &message)
    : std::runtime_error{message}, code_{code} {}

EditorCommandService::EditorCommandService(EditorCommandServiceDependencies dependencies)
    : dependencies_{std::move(dependencies)} {
    if (!dependencies_.document || !dependencies_.current_scene_id) {
        throw std::invalid_argument("EditorCommandService requires document and current_scene_id providers");
    }
}

const AuthoringSceneDocument &EditorCommandService::document() const {
    return dependencies_.document();
}

const AuthoringSceneView &
EditorCommandService::selectScene(const std::vector<AuthoringSceneView> &scenes,
                                  const std::optional<std::string> &requested_scene) const {
    const auto scene_id = requested_scene.value_or(dependencies_.current_scene_id());
    const auto found = std::find_if(scenes.begin(), scenes.end(),
                                    [&](const auto &scene) { return scene.scene_id == scene_id; });
    if (found == scenes.end()) {
        throw EditorCommandError{EditorCommandErrorCode::SceneNotFound,
                                 "scene not found: " + scene_id};
    }
    return *found;
}

EditorObjectQueryResult EditorCommandService::queryObject(const AuthoringSceneDocument &source,
                                                          const AuthoringSceneView &scene,
                                                          const AuthoringObjectView &object) const {
    EditorRuntimeObjectState runtime;
    if (dependencies_.runtime_query) runtime = dependencies_.runtime_query(scene, object);
    if (!runtime.component_runtime_json.empty() &&
        runtime.component_runtime_json.size() != object.components.size()) {
        throw std::logic_error("editor runtime adapter returned a component count mismatch");
    }
    if (!runtime.component_pending.empty() && runtime.component_pending.size() != object.components.size()) {
        throw std::logic_error("editor runtime adapter returned a pending count mismatch");
    }

    EditorObjectQueryResult result{.scene_revision = source.revision(),
                                   .authoring_object_id = object.authoring_object_id,
                                   .name = object.name,
                                   .parent = object.parent,
                                   .entity_id = runtime.entity_id};
    result.components.reserve(object.components.size());
    for (std::size_t index = 0; index < object.components.size(); ++index) {
        const auto &component = object.components[index];
        const auto name = component.authoredJson().at("name").get<std::string>();
        EditorComponentQueryResult component_result{
            .name = name,
            .authored_json = component.authoredJson(),
            .editable = component.codec.editable,
            .codec_state = component.codec.state,
            .codec_name = std::string{component.codec.codec_name},
            .pending = !runtime.component_pending.empty() && runtime.component_pending[index],
        };
        if (!runtime.component_runtime_json.empty()) {
            component_result.runtime_json = runtime.component_runtime_json[index];
        }
        if (const auto *codec = findComponentCodec(name)) {
            component_result.schema_state = EditorComponentSchemaState::Available;
            const auto fields = codec->fieldSchema();
            component_result.schema_fields.assign(fields.begin(), fields.end());
        }
        result.components.push_back(std::move(component_result));
    }
    return result;
}

EditorSceneTreeResult EditorCommandService::sceneTree(const EditorSceneTreeRequest &request) const {
    const auto &source = document();
    const auto scenes = source.query();
    const auto &scene = selectScene(scenes, request.scene_id);
    EditorSceneTreeResult result{.scene_revision = source.revision(), .scene_id = scene.scene_id};
    result.objects.reserve(scene.objects.size());
    for (const auto &object : scene.objects) result.objects.push_back(queryObject(source, scene, object));
    return result;
}

EditorObjectQueryResult
EditorCommandService::getComponents(const EditorGetComponentsRequest &request) const {
    if (request.authoring_object_id.has_value() == request.name.has_value()) {
        throw EditorCommandError{EditorCommandErrorCode::InvalidParams,
                                 "get_components requires exactly one object selector"};
    }
    const auto &source = document();
    const auto scenes = source.query();
    const auto &scene = selectScene(scenes, request.scene_id);
    const auto found = std::find_if(scene.objects.begin(), scene.objects.end(), [&](const auto &object) {
        if (request.authoring_object_id) return object.authoring_object_id == *request.authoring_object_id;
        return object.name && *object.name == *request.name;
    });
    if (found == scene.objects.end()) {
        throw EditorCommandError{EditorCommandErrorCode::ObjectNotFound,
                                 "object not found in scene: " + scene.scene_id};
    }
    return queryObject(source, scene, *found);
}

EditorListAssetsResult EditorCommandService::listAssets(const EditorListAssetsRequest &request) const {
    EditorListAssetsResult result;
    if (!dependencies_.assets) return result;
    for (auto asset : dependencies_.assets()) {
        if (!request.store || asset.store == *request.store) result.assets.push_back(std::move(asset));
    }
    return result;
}

ExportSceneSnapshotResponseV1
EditorCommandService::exportSceneSnapshot(const ExportSceneSnapshotRequestV1 &request) const {
    if (request.schema_version != 1) {
        throw EditorCommandError{EditorCommandErrorCode::UnsupportedSnapshotVersion,
                                 "unsupported snapshot version: " +
                                     std::to_string(request.schema_version)};
    }
    const auto snapshot_state = dependencies_.snapshot_state ? dependencies_.snapshot_state()
                                                               : EditorSnapshotState{};
    if (snapshot_state.preview_epoch > maxExactEditorJsonInteger) {
        throw std::overflow_error("preview_epoch exceeds 2^53-1");
    }
    if (!request.allow_pending &&
        (!snapshot_state.pending_ticket_ids.empty() || snapshot_state.open_preview_lease)) {
        throw EditorCommandError{EditorCommandErrorCode::SnapshotBusy,
                                 "snapshot export is busy"};
    }

    const auto &source = document();
    if (source.revision().value > maxExactEditorJsonInteger) {
        throw std::overflow_error("SceneRevision exceeds 2^53-1");
    }
    const auto current_scene = dependencies_.current_scene_id();
    if (source.scenesJson().find(current_scene) == source.scenesJson().end()) {
        throw EditorCommandError{EditorCommandErrorCode::SceneNotFound,
                                 "scene not found: " + current_scene};
    }
    auto bytes = source.encodeSemantic();
    if (bytes.size() > maxSceneSnapshotBytes) {
        throw EditorCommandError{EditorCommandErrorCode::SnapshotTooLarge,
                                 "snapshot exceeds 64 MiB"};
    }

    auto digest = picosha2::hash256_hex_string(bytes.begin(), bytes.end());
    return ExportSceneSnapshotResponseV1{
        .schema_version = 1,
        .scene_revision = source.revision(),
        .current_scene_id = current_scene,
        .semantic_scene_bytes = std::move(bytes),
        .digest = {.algorithm = "sha256", .hex = std::move(digest)},
        .pending_ticket_ids = snapshot_state.pending_ticket_ids,
        .preview_epoch = snapshot_state.preview_epoch,
    };
}

OrderedJson editorQueryJson(const EditorComponentQueryResult &component) {
    OrderedJson result{{"name", component.name}, {"authored_json", component.authored_json}};
    if (component.runtime_json) result["runtime_json"] = *component.runtime_json;
    result["editable"] = component.editable;
    result["codec"] = OrderedJson{{"state", component.codec_state == ComponentCodecState::Registered
                                                   ? "registered"
                                                   : "missing"}};
    if (!component.codec_name.empty()) result["codec"]["name"] = component.codec_name;
    result["schema"] = OrderedJson{{"state", component.schema_state == EditorComponentSchemaState::Available
                                                    ? "available"
                                                    : "missing"}};
    if (component.schema_state == EditorComponentSchemaState::Available) {
        result["schema"]["fields"] = OrderedJson::array();
        for (const auto &field : component.schema_fields) {
            result["schema"]["fields"].push_back(schemaFieldJson(field));
        }
    }
    result["pending"] = component.pending;
    return result;
}

OrderedJson editorQueryJson(const EditorObjectQueryResult &object) {
    OrderedJson result{{"scene_revision", object.scene_revision.value},
                       {"authoring_object_id", object.authoring_object_id.value}};
    if (object.name) result["name"] = *object.name;
    if (object.parent) result["parent"] = *object.parent;
    if (object.entity_id) {
        result["entity_id"] = OrderedJson{{"index", object.entity_id->index},
                                           {"gen", object.entity_id->generation}};
    }
    result["components"] = OrderedJson::array();
    for (const auto &component : object.components) {
        result["components"].push_back(editorQueryJson(component));
    }
    return result;
}

OrderedJson editorQueryJson(const EditorSceneTreeResult &scene) {
    OrderedJson result{{"scene_revision", scene.scene_revision.value}, {"scene_id", scene.scene_id}};
    result["objects"] = OrderedJson::array();
    for (const auto &object : scene.objects) result["objects"].push_back(editorQueryJson(object));
    return result;
}

OrderedJson editorQueryJson(const EditorListAssetsResult &assets) {
    OrderedJson result;
    result["assets"] = OrderedJson::array();
    for (const auto &asset : assets.assets) {
        result["assets"].push_back(OrderedJson{{"id", asset.id},
                                               {"kind", asset.kind},
                                               {"path", asset.path},
                                               {"store", asset.store},
                                               {"status", asset.status}});
    }
    return result;
}

OrderedJson editorQueryJson(const ExportSceneSnapshotResponseV1 &snapshot) {
    return OrderedJson{
        {"schema_version", snapshot.schema_version},
        {"scene_revision", snapshot.scene_revision.value},
        {"current_scene_id", snapshot.current_scene_id},
        {"semantic_scene_bytes", snapshot.semantic_scene_bytes},
        {"digest", OrderedJson{{"algorithm", snapshot.digest.algorithm}, {"hex", snapshot.digest.hex}}},
        {"pending_ticket_ids", snapshot.pending_ticket_ids},
        {"preview_epoch", snapshot.preview_epoch},
    };
}

OrderedJson EditorCommandRpcAdapter::sceneTree(const Json &params) const {
    return editorQueryJson(service_.sceneTree(parseSceneTreeRequest(params)));
}

OrderedJson EditorCommandRpcAdapter::getComponents(const Json &params) const {
    return editorQueryJson(service_.getComponents(parseGetComponentsRequest(params)));
}

OrderedJson EditorCommandRpcAdapter::listAssets(const Json &params) const {
    return editorQueryJson(service_.listAssets(parseListAssetsRequest(params)));
}

OrderedJson EditorCommandRpcAdapter::exportSceneSnapshot(const Json &params) const {
    return editorQueryJson(service_.exportSceneSnapshot(parseExportSceneSnapshotRequest(params)));
}

} // namespace Pelican
